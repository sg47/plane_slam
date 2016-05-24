#include "plane_slam.h"

PlaneSlam::PlaneSlam() :
    isam2_parameters_()
  , graph_()
  , initial_estimate_()
  , first_pose_(true)
  , poses_()
  , pose_count_(0)
  , landmakr_count_(0)
{
    isam2_parameters_.relinearizeThreshold = 0.05;
    isam2_parameters_.relinearizeSkip = 1;
    isam2_parameters_.print("ISAM2 parameters:");
    isam2_ = new ISAM2(isam2_parameters_);
}

void PlaneSlam::initialize(Pose3 &init_pose, std::vector<PlaneType> &planes)
{
    if(planes.size() == 0)
        return;

    // Add a prior factor
    pose_count_ = 0;
    Key x0 = Symbol('x', 0);
    Vector pose_sigmas(6);
    pose_sigmas << init_pose.translation().vector()*0.2, init_pose.rotation().rpy() * 0.1;
    noiseModel::Diagonal::shared_ptr poseNoise = noiseModel::Diagonal::Sigmas( pose_sigmas ); // 30cm std on x,y,z 0.1 rad on roll,pitch,yaw
    graph_.push_back( PriorFactor<Pose3>( x0, init_pose, poseNoise ) );
    // Add an initial guess for the current pose
    initial_estimate_.insert<Pose3>( x0, init_pose );
    poses_.insert( x0, init_pose );
    pose_count_++;
    // Add a prior landmark
    Key l0 = Symbol('l', 0);
    OrientedPlane3 lm0(planes[0].coefficients);
    OrientedPlane3 glm0 = lm0.transform(init_pose.inverse());
    noiseModel::Diagonal::shared_ptr lm_noise = noiseModel::Diagonal::Sigmas( (Vector(2) << planes[0].sigmas[0], planes[0].sigmas[1]).finished() );
    graph_.push_back( OrientedPlane3DirectionPrior( l0, glm0.planeCoefficients(), lm_noise) );


    landmakr_count_ = 0;
    for(int i = 0; i < planes.size(); i++)
    {
        PlaneType &plane = planes[i];
        Key ln = Symbol('l', i);

        // Add observation factor
        noiseModel::Diagonal::shared_ptr obs_noise = noiseModel::Diagonal::Sigmas( plane.sigmas );
        graph_.push_back( OrientedPlane3Factor(plane.coefficients, obs_noise, x0, ln) );

        // Add initial guesses to all observed landmarks
        cout << "Key: " << ln << endl;
        OrientedPlane3 lmn( plane.coefficients );
        initial_estimate_.insert<OrientedPlane3>( ln, lmn.transform(init_pose.inverse()) );

        //
        landmakr_count_ ++;
    }
    isam2_->update(graph_, initial_estimate_);
    // Clear the factor graph and values for the next iteration
    graph_.resize(0);
    initial_estimate_.clear();
    //
    first_pose_ = false;

    cout << GREEN << "Initialize plane slam, register first observation." << endl;
    initial_estimate_.print(" - Initial estimate: ");
    cout << RESET << endl;
}

Pose3 PlaneSlam::planeSlam(Pose3 &rel_pose, std::vector<PlaneType> &planes)
{    
    if(first_pose_)
    {
        ROS_ERROR("You should initialize the map before doing slam.");
        exit(1);
    }

    // convert to gtsam plane type
    std::vector<OrientedPlane3> observations;
    for( int i = 0; i < planes.size(); i++)
    {
        observations.push_back( OrientedPlane3(planes[i].coefficients) );
    }

    // Add odometry factors
    Vector odom_sigmas(6);
    odom_sigmas << rel_pose.translation().vector()*0.2, rel_pose.rotation().rpy() * 0.1;
    noiseModel::Diagonal::shared_ptr odometry_noise =
            noiseModel::Diagonal::Sigmas( odom_sigmas );
    cout << GREEN << "odom noise dim: " << odometry_noise->dim() << RESET << endl;
    Key pose_key = Symbol('x', pose_count_);
    Key last_key = Symbol('x', pose_count_-1);
    graph_.push_back(BetweenFactor<Pose3>(last_key, pose_key, rel_pose, odometry_noise));
    // Add pose guess
    Pose3 new_pose = poses_.at<Pose3>( last_key );
    new_pose.compose( rel_pose );
    initial_estimate_.insert<Pose3>( pose_key, new_pose );
    pose_count_ ++;

//    //
//    cout << RED;
//    rel_pose.print(" rel pose: ");
//    cout << RESET << endl;
//    //
//    cout << GREEN;
//    new_pose.print(" new pose: ");
//    cout << RESET << endl;

    // Transform modeled landmakr to pose frame
    Values values = isam2_->calculateBestEstimate();
    std::vector<OrientedPlane3> predicted_observations;
    for(int i = 0; i < landmakr_count_; i++)
    {
        Key ln = Symbol('l', i);
        OrientedPlane3 predicted = values.at(ln).cast<OrientedPlane3>();
        predicted_observations.push_back( predicted.transform( new_pose ) );
    }

    // Match modelled landmark with measurement
    std::vector<PlanePair> pairs;
    matchPlanes( predicted_observations, observations, pairs);

    // Add factor to exist landmark
    for( int i = 0; i < pairs.size(); i++)
    {
        PlanePair &pair = pairs[i];
        noiseModel::Diagonal::shared_ptr obs_noise = noiseModel::Diagonal::Sigmas( plane.sigmas );
        graph_.push_back( OrientedPlane3Factor(plane.coefficients, obs_noise, x0, ln) );
    }

    // Add new landmark


    // Update graph
    cout << "update " << endl;
    isam2_->update(graph_, initial_estimate_);
    // isam2->update(); // call additionally

    // Update pose
    cout << pose_count_ << " get current est " << endl;
    Pose3 current_estimate = isam2_->calculateEstimate(pose_key).cast<Pose3>();
    poses_.insert( pose_key, current_estimate);

    //
    cout << BLUE;
    current_estimate.print("Current estimate:");
    cout << RESET << endl;


    // Clear the factor graph and values for the next iteration
    graph_.resize(0);
    initial_estimate_.clear();

    return current_estimate;
}

void PlaneSlam::matchPlanes( std::vector<OrientedPlane3> &predicted_observations,
                             std::vector<OrientedPlane3> &observations,
                             std::vector<PlanePair> &pairs)
{

}
