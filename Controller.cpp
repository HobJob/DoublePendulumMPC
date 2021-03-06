#include "Controller.h"

Controller::Controller(std::string model_path,std::string config_path)
{
    config = YAML::LoadFile(config_path);

    //Actuation of pendulum.
    config_actuated_link = static_cast<actuated_link>(config["actuated_link"].as<int>());

    this->loadModel(model_path);
    this->loadConfig(config_path);
    
    trajectory_xs.resize(T_ROUTE, state->zero());
    trajectory_us.resize(T_ROUTE, state->zero());

    mpc_warmStart_xs.resize(T_MPC, state->zero());
    mpc_warmStart_us.resize(T_MPC, Eigen::VectorXd::Zero(actuation_model->get_nu()));
}

Controller::~Controller()
{
    std::cout << "Deleting Controller" << std::endl;
    
    #if USE_GRAPHS
    if(graph_logger) delete graph_logger;
    #endif

    if(r) delete r;
}

void Controller::loadModel(std::string path)
{
    // Create the arm model based on the URDF.
    pinocchio::urdf::buildModel(path, model);
    
    // Create the state vector. Simple pendulum has q and dot_q.
    state = boost::make_shared<crocoddyl::StateMultibody>(boost::make_shared<pinocchio::Model>(model));
    actuation_model = boost::make_shared<ActuationModelDoublePendulum>(state, 2, model.nv, config_actuated_link);

    initial_state = Eigen::VectorXd(state->get_nx());

    std::cout << "Model has: "   << std::endl << "nq: " << state->get_nq() << std::endl
    << "nx: " << state->get_nx() << std::endl
    << "nv: " << state->get_nv() << std::endl
    << "nu: " << actuation_model->get_nu() << std::endl;
}

void Controller::loadConfig(std::string configPath)
{
    
    // Init all the predefined variables.
    
    //Time interval
    dt = config["dt"].as<double>();
    
    // Costs
    activation_model_weights = Eigen::VectorXd(6);
    activation_model_weights <<  config["Sin_theta_Weight"].as<double>(), config["Sin_alpha_Weight"].as<double>(),
        config["Cos_theta_Weight"].as<double>(), config["Cos_alpha_Weight"].as<double>(),
        config["Vel_theta_Weight"].as<double>(), config["Vel_alpha_Weight"].as<double>();

    x_reg_weight = config["xReg"].as<double>();
    u_reg_weight = config["uReg"].as<double>();
    
    trajectory_node_weight = config["trajectory_node_weight"].as<double>();
    trajectory_terminal_weight = config["trajectory_terminal_weight"].as<double>();
    
    running_model_goal_weight = config["running_model_goal_weight"].as<double>();
    terminal_model_goal_weight = config["terminal_model_goal_weight"].as<double>();

    torque_limit_ub = Eigen::VectorXd(actuation_model->get_nu());
    torque_limit_lb = Eigen::VectorXd(actuation_model->get_nu());

    torque_limit_ub.fill(config["tau_ub"].as<double>());
	torque_limit_lb.fill(config["tau_lb"].as<double>());

    T_ROUTE = config["T_ROUTE"].as<double>();
    T_MPC = config["T_MPC"].as<double>();

    trajectory_solver_iterations = config["initial_solver_iterations"].as<int>();
    mpc_solver_iterations = config["solver_iterations"].as<int>();

    goto_base_position = config["goto_base_position"].as<bool>();
    zero_the_initial_position = config["zero_the_initial_position"].as<bool>();

    use_callback_verbose = config["use_callback_verbose"].as<bool>();

    control_loop_iterations = config["control_loop_iterations"].as<int>();
}

void Controller::createDOCP(bool trajectory)
{
    differential_models_running.clear();
    integrated_models_running.clear();
    
    running_cost_model_sum  = boost::make_shared<crocoddyl::CostModelSum>(state, actuation_model->get_nu());
    terminal_cost_model_sum = boost::make_shared<crocoddyl::CostModelSum>(state, actuation_model->get_nu());
    
    x_reg_cost = boost::make_shared<crocoddyl::CostModelState>(state, boost::make_shared<crocoddyl::ActivationModelQuad>(state->get_ndx()), state->zero(),
            actuation_model->get_nu());
    
    u_reg_cost = boost::make_shared<crocoddyl::CostModelControl>(state, boost::make_shared<crocoddyl::ActivationModelQuad>(2),
			actuation_model->get_nu());

    x_goal_cost = boost::make_shared<CostModelDoublePendulum>(state,
	 		boost::make_shared<crocoddyl::ActivationModelWeightedQuad>(activation_model_weights),actuation_model->get_nu());

    //Defineix la theta de referencia igual a tots els nodes. No hi ha cap WP.
    x_goal_cost->setReference(0.0, 0.0, 0.0 , 0.0);

    // Add the var regularization
    if(u_reg_weight != 0) terminal_cost_model_sum->addCost("u_reg", u_reg_cost, u_reg_weight);
    if(x_reg_weight != 0) terminal_cost_model_sum->addCost("x_reg", x_reg_cost, x_reg_weight);

    if(trajectory){
        running_cost_model_sum-> addCost("x_goal", x_goal_cost, trajectory_node_weight);
        terminal_cost_model_sum->addCost("x_goal", x_goal_cost, trajectory_terminal_weight);
    }else{
        running_cost_model_sum-> addCost("x_goal", x_goal_cost, running_model_goal_weight);
        terminal_cost_model_sum->addCost("x_goal", x_goal_cost, terminal_model_goal_weight);    
    }

    int nodes = trajectory ? T_ROUTE : T_MPC;

    for (int i = 0; i < nodes - 1; ++i)
    {
        boost::shared_ptr<crocoddyl::DifferentialActionModelFreeFwdDynamics> diff_model = 
        boost::make_shared<crocoddyl::DifferentialActionModelFreeFwdDynamics>(state, actuation_model, running_cost_model_sum);

        diff_model->set_u_ub(torque_limit_ub);
        diff_model->set_u_lb(torque_limit_lb);

        boost::shared_ptr<crocoddyl::IntegratedActionModelEuler> int_model =
        boost::make_shared<crocoddyl::IntegratedActionModelEuler>(diff_model, dt);

        differential_models_running.push_back(diff_model);
        integrated_models_running.push_back(int_model);
    }
    std::cout << "There are " << differential_models_running.size() << " diferential models running." << std::endl; 

    differential_terminal_model = boost::make_shared<crocoddyl::DifferentialActionModelFreeFwdDynamics>(state, actuation_model, terminal_cost_model_sum);
    
    differential_terminal_model->set_u_ub(torque_limit_ub);
    differential_terminal_model->set_u_lb(torque_limit_lb);
    
    integrated_terminal_model = boost::make_shared<crocoddyl::IntegratedActionModelEuler>(differential_terminal_model, dt);

    problem = boost::make_shared<crocoddyl::ShootingProblem>(initial_state, integrated_models_running, integrated_terminal_model);

    solver = boost::make_shared<crocoddyl::SolverBoxFDDP>(problem);

    if(use_callback_verbose && trajectory)
        addCallbackVerbose();
}

void Controller::addCallbackVerbose()
{
    std::vector<boost::shared_ptr<crocoddyl::CallbackAbstract>> cbs;
	cbs.emplace_back(boost::make_shared<crocoddyl::CallbackVerbose>());
    solver->setCallbacks(cbs);
}

//Very Specific code for the simple pendulum.
void Controller::connectODrive()
{
    r = new Robot(dt);

    auto *m0 = new Motor(M0);
    auto *m1 = new Motor(M1);
    
    //Link both motors. M0 angle is computed based on M1 angle.
    m0->setOtherMotor(m1);
    m1->setOtherMotor(m0);
    
    //Check if there is any odrive connected.
    if(r->odrives.empty()) return;
    odrive = r->odrives[0];

    std::cout << "Input voltage is " << odrive->inputVoltage() << std::endl;
    
    //Configure the ODrive.
    //r->configureODrive(odrive, m0, m1);

    //or Configure both motors if the odrive is already configured.
    odrive->configureMotor(m0);
    odrive->configureMotor(m1);
    
    odrive->m0->setRequestedState(AXIS_STATE_CLOSED_LOOP_CONTROL);
    odrive->m1->setRequestedState(AXIS_STATE_CLOSED_LOOP_CONTROL);
    
    if(goto_base_position)
    {
        std::cout << "Moving to starting position" << std::endl;
        odrive->m0->moveStartingPosition(100, &signalFlag);
        odrive->m1->moveStartingPosition(100, &signalFlag);
        usleep(2000000);
    }

    if(zero_the_initial_position)
    {
        std::cout << "Zeroing the position" << std::endl;
        odrive->m0->zeroPosition();
        odrive->m1->zeroPosition();
    }

    odrive->m0->setControlMode(CTRL_MODE_CURRENT_CONTROL);
    odrive->m1->setControlMode(CTRL_MODE_CURRENT_CONTROL);
}

void Controller::debugMotorAngles(){
    
    auto odrive = r->odrives[0];
    float alpha, theta;

    while(!signalFlag){
        alpha = odrive->m0->getPosEstimateInRad();
        theta = odrive->m1->getPosEstimateInRad();
        std::cout << "Theta: " << theta << std::endl << "Alpha: " << alpha << std::endl;
        usleep(100000); //250ms
    }
}

void Controller::signalHandler(int s)
{
  signalFlag = true; // something like that
}


void Controller::stopMotors()
{
    odrive->m0->disable();
    odrive->m1->disable();
}

void Controller::createTrajectory()
{
    initial_state << odrive->m0->getPosEstimateInRad(), odrive->m1->getPosEstimateInRad(),
    odrive->m0->getVelEstimateInRads(),odrive->m1->getVelEstimateInRads();

    problem->set_x0(initial_state);
    solver->solve(crocoddyl::DEFAULT_VECTOR, crocoddyl::DEFAULT_VECTOR, trajectory_solver_iterations, false, 1e-9);
    
    trajectory_xs = solver->get_xs();
    trajectory_us = solver->get_us();
    
    std::cout << "Trajectory generated! It has xs: " << trajectory_xs.size() << " and us: " << trajectory_us.size() << std::endl;

    //Assign the reference thetas from the trajectory to the RealTime MPC
    for(int node_index = 0; node_index < T_MPC - 1; node_index++)
    {
       boost::static_pointer_cast<CostModelDoublePendulum>(differential_models_running[node_index]->get_costs()->get_costs().find("x_goal")->second->cost)
       ->setReference(trajectory_xs[T_MPC - 1][0], trajectory_xs[T_MPC - 1][1],trajectory_xs[T_MPC - 1][2], trajectory_xs[T_MPC - 1][3]);
    }
    
    //Node terminal
    boost::static_pointer_cast<CostModelDoublePendulum>(differential_terminal_model->get_costs()->get_costs().find("x_goal")->second->cost)
       ->setReference(trajectory_xs[T_MPC - 1][0], trajectory_xs[T_MPC - 1][1],trajectory_xs[T_MPC - 1][2], trajectory_xs[T_MPC - 1][3]);
}

void Controller::executeTrajectoryOpenLoop(){
    std::cout << "Executing trajectory..." << std::endl;
    r->executeTrajectoryOpenLoop(trajectory_us, graph_logger);
    std::cout << "Ended trajectory! " << std::endl;
}

void Controller::controlLoop()    
{
    /*
    long iteration = 0;
    
    
    int time_skips = 0;

    int i = 0, j = 0;
    int iter_after_xt = 0;
    float elapsedTime = 0, torque = 0;
    int warmStartIndex = 0;
    

    bool fa = true;
    bool fb = true;

    //this->startGraphsThread();

    while(!signalFlag){
        
        auto start = std::chrono::high_resolution_clock::now();
        
        initial_state[0] = odrive->m0->getPosEstimateInRad();
        initial_state[1] = odrive->m0->getVelEstimateInRads();
        
        //Safety check
        if(initial_state[1] > 25 || initial_state[1] < -25) 
        {
            std::cout << "Speed limit reached!" << std::endl;
            break;
        }

        problem->set_x0(initial_state);
        solver->solve(mpc_warmStart_xs, mpc_warmStart_us, mpc_solver_iterations, false, 1e-9);
        

        torque = solver->get_us()[0][0];
        odrive->m0->setTorque(torque);
        
        //Prepare next warm start.
        if(warmStartIndex > T_ROUTE - T_MPC - 2){
            
            if(fa){
                std::cout << "End of carrot. Starting progressively RailMPC. " << iteration  << " WarmStartIndex is " << warmStartIndex << std::endl;
                fa = false;
            }

            if(iter_after_xt < T_MPC - 1){
                //Entering Rail MPC progressively
                
                iter_after_xt++;
                //Prepare the warmStart vectors:
                mpc_warmStart_xs = { trajectory_xs.begin() + warmStartIndex + iter_after_xt, trajectory_xs.begin() + warmStartIndex + T_MPC};
                mpc_warmStart_us = { trajectory_us.begin() + warmStartIndex + iter_after_xt, trajectory_us.begin() + warmStartIndex + T_MPC};

                //Append the new data.
                auto xs = solver->get_xs();
                auto us = solver->get_us();

                mpc_warmStart_xs.insert(mpc_warmStart_xs.end(), xs.end() - iter_after_xt , xs.end());
                mpc_warmStart_us.insert(mpc_warmStart_us.end(), us.end() - iter_after_xt , us.end());

                //Remove the terminal reference only once!
                if(iter_after_xt == 1){
                    auto cost_model_sum = differential_terminal_model->get_costs()->get_costs();
                   
                    auto goal_cost = boost::static_pointer_cast<CostModelDoublePendulum>(cost_model_sum.find("x_goal")->second->cost);
                    auto reg_x_cost = boost::static_pointer_cast<crocoddyl::CostModelState>(cost_model_sum.find("x_reg")->second->cost);
                    auto reg_u_cost = boost::static_pointer_cast<crocoddyl::CostModelControl>(cost_model_sum.find("u_reg")->second->cost);
                    
                    goal_cost->setReference(0);
                    reg_x_cost->set_xref(state->zero());
                    reg_u_cost->set_uref((Eigen::VectorXd::Zero(state->get_nv())));   
                }else{
                    //Remove the references from the last iteration terminal cost.
                    auto cost_model_sum = differential_models_running[T_MPC - iter_after_xt]->get_costs()->get_costs();
                   
                    auto goal_cost = boost::static_pointer_cast<CostModelDoublePendulum>(cost_model_sum.find("x_goal")->second->cost);
                    auto reg_x_cost = boost::static_pointer_cast<crocoddyl::CostModelState>(cost_model_sum.find("x_reg")->second->cost);
                    auto reg_u_cost = boost::static_pointer_cast<crocoddyl::CostModelControl>(cost_model_sum.find("u_reg")->second->cost);
                    
                    goal_cost->setReference(0);
                    reg_x_cost->set_xref(state->zero());
                    reg_u_cost->set_uref((Eigen::VectorXd::Zero(state->get_nv())));
                }

                //Update all the nodes references.
                for(int node_index = 0; node_index <= T_MPC - iter_after_xt - 1; node_index++)
                {
                    auto cost_model_sum = differential_models_running[node_index]->get_costs()->get_costs();
                    auto goal_cost = boost::static_pointer_cast<CostModelDoublePendulum>(cost_model_sum.find("x_goal")->second->cost);
                    auto reg_x_cost = boost::static_pointer_cast<crocoddyl::CostModelState>(cost_model_sum.find("x_reg")->second->cost);
                    auto reg_u_cost = boost::static_pointer_cast<crocoddyl::CostModelControl>(cost_model_sum.find("u_reg")->second->cost);
                
                    Eigen::VectorXd state = mpc_warmStart_xs[node_index];
                    goal_cost->setReference(state[0]);
                    reg_x_cost->set_xref(state);
                    reg_u_cost->set_uref(mpc_warmStart_us[node_index]);   
                }

                //Make the last node terminal by changing its weight.
                differential_models_running[T_MPC - iter_after_xt - 1]->get_costs()->get_costs().find("x_goal")->second->weight = trajectory_terminal_weight;   
                
            }else{
                    
                if(fb){
                    std::cout << "All costs are terminal" << iteration << std::endl;
                    //Remove the references from the first node (Last One to become Full Rail)
                    auto cost_model_sum = differential_models_running[0]->get_costs()->get_costs();
                   
                    auto goal_cost = boost::static_pointer_cast<CostModelDoublePendulum>(cost_model_sum.find("x_goal")->second->cost);
                    auto reg_x_cost = boost::static_pointer_cast<crocoddyl::CostModelState>(cost_model_sum.find("x_reg")->second->cost);
                    auto reg_u_cost = boost::static_pointer_cast<crocoddyl::CostModelControl>(cost_model_sum.find("u_reg")->second->cost);
                    
                    goal_cost->setReference(0);
                    reg_x_cost->set_xref(state->zero());
                    reg_u_cost->set_uref((Eigen::VectorXd::Zero(state->get_nv())));
                    fb = false;
                }

                // All costs are terminal
                mpc_warmStart_xs = solver->get_xs();
                mpc_warmStart_us = solver->get_us();
              
                if(control_loop_iterations > 0 && ++j >= control_loop_iterations)
                    break;
            }
            
        }else{

            warmStartIndex++;
            //Carrot MPC
            mpc_warmStart_xs = { trajectory_xs.begin() + warmStartIndex, trajectory_xs.begin() + warmStartIndex + T_MPC};
            mpc_warmStart_us = { trajectory_us.begin() + warmStartIndex, trajectory_us.begin() + warmStartIndex + T_MPC};
            
            for(int node_index = 0; node_index < T_MPC - 1; node_index++)
            {
                auto cost_model_sum = differential_models_running[node_index]->get_costs()->get_costs();
                auto goal_cost = boost::static_pointer_cast<CostModelDoublePendulum>(cost_model_sum.find("x_goal")->second->cost);
                auto reg_x_cost = boost::static_pointer_cast<crocoddyl::CostModelState>(cost_model_sum.find("x_reg")->second->cost);
                auto reg_u_cost = boost::static_pointer_cast<crocoddyl::CostModelControl>(cost_model_sum.find("u_reg")->second->cost);
                
                Eigen::VectorXd state = mpc_warmStart_xs[node_index];
                goal_cost->setReference(state[0]);
                reg_x_cost->set_xref(state);
                reg_u_cost->set_uref(mpc_warmStart_us[node_index]);
            }
        
            //Node terminal
            auto cost_model_sum = differential_terminal_model->get_costs()->get_costs();
            auto goal_cost = boost::static_pointer_cast<CostModelDoublePendulum>(cost_model_sum.find("x_goal")->second->cost);
            auto reg_x_cost = boost::static_pointer_cast<crocoddyl::CostModelState>(cost_model_sum.find("x_reg")->second->cost);
            auto reg_u_cost = boost::static_pointer_cast<crocoddyl::CostModelControl>(cost_model_sum.find("u_reg")->second->cost);    

            Eigen::VectorXd state = mpc_warmStart_xs[T_MPC - 1];
            goal_cost->setReference(state[0]);
            reg_x_cost->set_xref(state);
            reg_u_cost->set_uref(mpc_warmStart_us[T_MPC - 1]);
        }

        #if USE_GRAPHS
        graph_logger->appendToBuffer("computed currents", odrive->m0->castTorqueToCurrent(torque));
        graph_logger->appendToBuffer("computed positions", solver->get_xs()[0][0]);
        graph_logger->appendToBuffer("computed next positions", solver->get_xs()[1][0]);

        graph_logger->appendToBuffer("computed next control", odrive->m0->castTorqueToCurrent(solver->get_us()[1][0]));

        graph_logger->appendToBuffer("computed velocities", solver->get_xs()[0][1]);
        graph_logger->appendToBuffer("computed cost", solver->get_cost());
        graph_logger->appendToBuffer("ODrive real position", initial_state[0]);
        graph_logger->appendToBuffer("ODrive real velocity", initial_state[1]);
        graph_logger->appendToBuffer("ODrive real current", odrive->m0->getCurrent());
        
        #endif

        iteration++;
        elapsedTime = (dt * 1000000.0f) - ((float) std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::high_resolution_clock::now() - start).count());
        
        if(elapsedTime > 0){
            usleep(elapsedTime);
        }else{
            time_skips++;
            if(time_skips >= 5){
            std::cout << "Skipped 5 frames. Elapsed time was " << elapsedTime << std::endl;
               // break;
            }
        }
    }*/
}

double Controller::iterationsToSeconds(int iterations)
{
    return iterations * dt;
}

int Controller::secondsToIterations(int seconds)
{
    return std::ceil((double)seconds / dt);
}

/*******************GRAPHS*****************/

//Use this after the OCP route creation
void Controller::initGraphs()
{
    #if USE_GRAPHS
    graph_logger = new Graph_Logger(dt);

    //Extraiem les posicions i les comandes de torque inicials.
    std::vector<Eigen::VectorXd> xs_eigen = solver->get_xs();
    std::vector<Eigen::VectorXd> us_eigen = solver->get_us();

    for(auto const& x: xs_eigen)
    {
        graph_logger->appendToBuffer("Crocoddyl initial calculated position m0", x[1]);
        graph_logger->appendToBuffer("Crocoddyl initial calculated position m1", x[0]);
        graph_logger->appendToBuffer("Crocoddyl initial calculated velocity m0", x[3]);
        graph_logger->appendToBuffer("Crocoddyl initial calculated velocity m1", x[2]);
    }

    for(auto const& u: us_eigen)
    {
        graph_logger->appendToBuffer("Crocoddyl initial calculated current m0", odrive->m0->castTorqueToCurrent(u[1]));
        graph_logger->appendToBuffer("Crocoddyl initial calculated current m1", odrive->m1->castTorqueToCurrent(u[0]));
    }
    

    //Alloc memory for the graphs:
    std::vector<std::string> datasets = {
        "Crocoddyl initial calculated position m0",
        "Crocoddyl initial calculated position m1",
        "Crocoddyl initial calculated velocity m0",
        "Crocoddyl initial calculated velocity m1",
        "Crocoddyl initial calculated current m0",
        "Crocoddyl initial calculated current m1",
        "computed currents m0",
        "computed currents m1",
        "ODrive real position m0",
        "ODrive real position m1",
        "ODrive real velocity m0",
        "ODrive real velocity m1",
        "ODrive real current m0",
        "ODrive real current m1"};

    long additional_nodes = control_loop_iterations > 0 ? control_loop_iterations : (T_MPC + T_ROUTE) * 5;

    graph_logger->allocMemory(datasets, additional_nodes);
    
    //Senyals per al thread del graphlogger.    
    futureObj = exitSignal.get_future();
    #endif
}

void Controller::startGraphsThread()
{
    #if USE_GRAPHS
    graphs_thread = std::thread(&Graph_Logger::adquisitionThread, graph_logger, odrive, std::move(futureObj));
    #endif
}

void Controller::stopGraphs()
{
    #if USE_GRAPHS
    std::cout << "Asking Thread to Stop" << std::endl;
    //Set the value in promise
    exitSignal.set_value();
    
    //Wait for thread to join
    graphs_thread.join();
    #endif
}

void Controller::showGraphs()
{
    #if USE_GRAPHS
    std::vector<std::string> datasets = {"Crocoddyl initial calculated position m0",
    "ODrive real position m0"};
    graph_logger->plot("Positions m0", datasets,"dt","rad", false, false);

    datasets = {"Crocoddyl initial calculated position m1",
    "ODrive real position m1"};
    graph_logger->plot("Positions m1", datasets,"dt","rad", false, false);
    
    datasets = {"Crocoddyl initial calculated velocity m0",
    "ODrive real velocity m0"};
    graph_logger->plot("Velocities m0", datasets,"dt","rad", false, false);

    datasets = {"Crocoddyl initial calculated velocity m1",
    "ODrive real velocity m1"};
    graph_logger->plot("Velocities m1", datasets,"dt","rad", false, false);

    datasets = {"Crocoddyl initial calculated current m0",
    "computed currents m0",
    "ODrive real current m0"};
    graph_logger->plot("Currents m0", datasets,"dt","Amps", false, false);

    datasets = {"Crocoddyl initial calculated current m1",
    "computed currents m1",
    "ODrive real current m1"};
    graph_logger->plot("Currents m1", datasets,"dt","Amps", false, false);

    #endif
}