#include "gtsam_mapping.h"

namespace plane_slam
{

GTMapping::GTMapping(ros::NodeHandle &nh, Viewer * viewer)
    : nh_(nh)
    , viewer_(viewer)
    , map_frame_("/world")
    , mapping_config_server_( ros::NodeHandle( nh_, "GTMapping" ) )
    , isam2_parameters_()
    , factor_graph_()
    , initial_estimate_()
    , pose_count_( 0 )
    , landmark_max_count_( 0 )
    , use_keyframe_( true )
    , keyframe_linear_threshold_( 0.05f )
    , keyframe_angular_threshold_( 5.0f )
    , isam2_relinearize_threshold_( 0.05f )
    , isam2_relinearize_skip_( 1 )
    , plane_match_direction_threshold_( 10.0*DEG_TO_RAD )   // 10 degree
    , plane_match_distance_threshold_( 0.1 )    // 0.1meter
    , plane_match_check_overlap_( true )
    , plane_match_overlap_alpha_( 0.5 )
    , plane_inlier_leaf_size_( 0.05f )  // 0.05meter
    , plane_hull_alpha_( 0.5 )
    , rng_(12345)
{
    // reconfigure
    mapping_config_callback_ = boost::bind(&GTMapping::gtMappingReconfigCallback, this, _1, _2);
    mapping_config_server_.setCallback(mapping_config_callback_);

    // Pose and Map
    optimized_pose_publisher_ = nh_.advertise<geometry_msgs::PoseStamped>("optimized_pose", 10);
    optimized_path_publisher_ = nh_.advertise<nav_msgs::Path>("optimized_path", 10);
    map_cloud_publisher_ = nh_.advertise<sensor_msgs::PointCloud2>("map_cloud", 10);
    marker_publisher_ = nh_.advertise<visualization_msgs::Marker>("visualization_marker", 10);

    //
    optimize_graph_service_server_ = nh_.advertiseService("optimize_graph", &GTMapping::optimizeGraphCallback, this );
    save_graph_service_server_ = nh_.advertiseService("save_graph", &GTMapping::saveGraphCallback, this );
    save_map_service_server_ = nh_.advertiseService("save_map_pcd", &GTMapping::saveMapPCDCallback, this );
    remove_bad_inlier_service_server_ = nh_.advertiseService("remove_bad_inlier", &GTMapping::removeBadInlierCallback, this );

    // ISAM2
    isam2_parameters_.relinearizeThreshold = isam2_relinearize_threshold_; // 0.1
    isam2_parameters_.relinearizeSkip = isam2_relinearize_skip_; // 1
    isam2_parameters_.factorization = ISAM2Params::Factorization::QR;
    isam2_parameters_.print( "ISAM2 parameters:" );
    isam2_ = new ISAM2( isam2_parameters_ );

}


bool GTMapping::mapping( const Frame &frame )
{
    bool success;
    if( !landmarks_.size() )    // first frame, add prior
    {
        success = addFirstFrame( frame );
    }
    else    // do mapping
    {
        if( use_keyframe_ )
        {
            if( isKeyFrame( frame ))
                success = doMapping( frame );   // do mapping
            else
                success = false;
        }
        else
        {
            success = doMapping( frame );   // do mapping
        }
    }

    // Map visualization
    if( success )
        updateMapViewer();

    cout << GREEN << " GTMapping, success = " << (success?"true":"false") << "." << RESET << endl;

    return success;
}

bool GTMapping::doMapping( const Frame &frame )
{
    if( !landmarks_.size() )
    {
        ROS_ERROR("You should call mapping() instead of doMapping().");
        exit(1);
    }

    const std::vector<PlaneType> &planes = frame.segment_planes_;
    const gtsam::Pose3 new_pose = tfToPose3( frame.pose_ );
    const gtsam::Pose3 rel_pose = last_estimated_pose_.inverse() * new_pose;

    // Convert observations to OrientedPlane3
    std::vector<OrientedPlane3> observations;
    for( int i = 0; i < planes.size(); i++)
    {
        observations.push_back( OrientedPlane3(planes[i].coefficients) );
    }

    // Get predicted-observation
    std::vector<OrientedPlane3> predicted_observations = getPredictedObservation( estimated_planes_, new_pose );

    // Match observations with predicted ones
    std::vector<PlanePair> pairs;
    matchPlanes( predicted_observations, landmarks_, observations, planes, new_pose, pairs);

    // Print pairs info
    cout << GREEN << " find pairs(obs, lm): " << pairs.size() << RESET << endl;
    for( int i = 0; i < pairs.size(); i++)
    {
        const PlanePair &pair = pairs[i];
        cout << "  - " << pair.iobs << ", " << pair.ilm << endl;
    }

//    // check pairs
//    if( pairs.size() < 3 )
//        return new_pose;

    // Add odometry factors
    Vector odom_sigmas(6);
//    odom_sigmas << rel_pose.translation().vector()*0.1, rel_pose.rotation().rpy() * 0.1;
    odom_sigmas << 0.05, 0.05, 0.05, 0.05, 0.05, 0.05;
    noiseModel::Diagonal::shared_ptr odometry_noise =
            noiseModel::Diagonal::Sigmas( odom_sigmas );
//    cout << GREEN << "odom noise dim: " << odometry_noise->dim() << RESET << endl;
    Key pose_key = Symbol('x', pose_count_);
    Key last_key = Symbol('x', pose_count_-1);
    factor_graph_.push_back(BetweenFactor<Pose3>(last_key, pose_key, rel_pose, odometry_noise));
    // Add pose guess
    initial_estimate_.insert<Pose3>( pose_key, new_pose );
    pose_count_ ++;

    // Add factor to exist landmark
    Eigen::VectorXi unpairs = Eigen::VectorXi::Ones( planes.size() );
    for( int i = 0; i < pairs.size(); i++)
    {
        PlanePair &pair = pairs[i];
        const PlaneType &obs = planes[pair.iobs];
        unpairs[pair.iobs] = 0;
        Key ln =  Symbol( 'l', pair.ilm);
        noiseModel::Diagonal::shared_ptr obs_noise = noiseModel::Diagonal::Sigmas( obs.sigmas );
        factor_graph_.push_back( OrientedPlane3Factor(obs.coefficients, obs_noise, pose_key, ln) );
    }

    // Add new landmark for unpaired observation
    for( int i = 0; i < unpairs.size(); i++ )
    {
        if( unpairs[i] )
        {
            // Add factor
            const PlaneType &obs = planes[i];
            Key ln = Symbol('l', landmark_max_count_);
            noiseModel::Diagonal::shared_ptr obs_noise = noiseModel::Diagonal::Sigmas( obs.sigmas );
            factor_graph_.push_back( OrientedPlane3Factor(obs.coefficients, obs_noise, pose_key, ln) );

            // Add initial guess
            OrientedPlane3 lmn( obs.coefficients );
            OrientedPlane3 glmn = lmn.transform( new_pose.inverse() );
            initial_estimate_.insert<OrientedPlane3>( ln, glmn );

            //
            landmark_max_count_ ++;
        }
    }

    // Update graph
    isam2_->update(factor_graph_, initial_estimate_);
    isam2_->update(); // call additionally

    // Update estimated poses and planes
    updateSlamResult( estimated_poses_, estimated_planes_ );

    // Update pose
    Pose3 current_estimate = estimated_poses_[estimated_poses_.size() - 1];

    // Update landmarks
    ros::Time utime =  ros::Time::now();
    updateLandmarks( landmarks_, planes, pairs, current_estimate, estimated_planes_ );
    cout << YELLOW << " - landmarks update time: " << (ros::Time::now() - utime).toSec()*1000
         << " ms " << RESET << endl;

    // Clear the factor graph and values for the next iteration
    factor_graph_.resize(0);
    initial_estimate_.clear();

    // Refine map
    if( refine_planar_map_ )
    {
        if( refinePlanarMap() )
        {
            isam2_->update(); // call once if map is refined
//            updateSlamResult( estimated_poses_, estimated_planes_ );
//            current_estimate = estimated_poses_[estimated_poses_.size() - 1];
        }
    }

    // update estimated pose
    last_estimated_pose_ = current_estimate;
    last_estimated_pose_tf_ = pose3ToTF( last_estimated_pose_ );
    return true;
}

bool GTMapping::addFirstFrame( const Frame &frame )
{
    if( frame.segment_planes_.size() == 0 )
        return false;

    const std::vector<PlaneType> &planes = frame.segment_planes_;
    const gtsam::Pose3 init_pose = tfToPose3( frame.pose_ );

    // clear
    pose_count_ = 0;
    landmark_max_count_ = 0;
    initial_estimate_.clear();
    estimated_poses_.clear();
    landmarks_.clear();
    estimated_planes_.clear();

    // Add a prior factor
    pose_count_ = 0;
    Key x0 = Symbol('x', 0);
    Vector pose_sigmas(6);
//    pose_sigmas << init_pose.translation().vector()*0.2, init_pose.rotation().rpy() * 0.2;
    pose_sigmas << 0.001, 0.001, 0.001, 0.0001, 0.001, 0.001;
    noiseModel::Diagonal::shared_ptr poseNoise = noiseModel::Diagonal::Sigmas( pose_sigmas ); // 30cm std on x,y,z 0.1 rad on roll,pitch,yaw
//    noiseModel::Diagonal::shared_ptr poseNoise = //
//        noiseModel::Diagonal::Variances((Vector(6) << 1e-6, 1e-6, 1e-6, 1e-4, 1e-4, 1e-4).finished());
    factor_graph_.push_back( PriorFactor<Pose3>( x0, init_pose, poseNoise ) );

    // Add an initial guess for the current pose
    initial_estimate_.insert<Pose3>( x0, init_pose );
    // Add to estimated pose
    estimated_poses_.push_back( init_pose );
    pose_count_++;

    // Add a prior landmark
    Key l0 = Symbol('l', 0);
    OrientedPlane3 lm0(planes[0].coefficients);
    OrientedPlane3 glm0 = lm0.transform(init_pose.inverse());
    noiseModel::Diagonal::shared_ptr lm_noise = noiseModel::Diagonal::Sigmas( (Vector(2) << planes[0].sigmas[0], planes[0].sigmas[1]).finished() );
    factor_graph_.push_back( OrientedPlane3DirectionPrior( l0, glm0.planeCoefficients(), lm_noise) );

    // Add new landmark
    landmark_max_count_ = 0;
    for(int i = 0; i < planes.size(); i++)
    {
        const PlaneType &plane = planes[i];
        Key ln = Symbol('l', i);

        cout << YELLOW << " lm" << i << " coefficents: " << plane.coefficients[0]
             << ", " << plane.coefficients[1] << ", " << plane.coefficients[2]
             << ", " << plane.coefficients[3] << ", centroid: " << plane.centroid.x
             << ", " << plane.centroid.y << ", " << plane.centroid.z << RESET << endl;

        // Add observation factor
        noiseModel::Diagonal::shared_ptr obs_noise = noiseModel::Diagonal::Sigmas( plane.sigmas );
        factor_graph_.push_back( OrientedPlane3Factor(plane.coefficients, obs_noise, x0, ln) );

        // Add initial guesses to all observed landmarks
//        cout << "Key: " << ln << endl;
        OrientedPlane3 lmn( plane.coefficients );
        OrientedPlane3 glmn = lmn.transform( init_pose.inverse() );
        initial_estimate_.insert<OrientedPlane3>( ln,  glmn );

        // Add to estimated plane
        estimated_planes_.push_back( glmn );

        // Add to landmarks buffer
        PlaneType global_plane;
        global_plane.coefficients = glmn.planeCoefficients();
        global_plane.color.Blue = rng_.uniform(0, 255);
        global_plane.color.Green = rng_.uniform(0, 255);
        global_plane.color.Red = rng_.uniform(0, 255);
        global_plane.color.Alpha = 255;
        Eigen::Matrix4d transform = init_pose.matrix();
        PointCloudTypePtr cloud_filtered( new PointCloudType );
        voxelGridFilter( plane.cloud, cloud_filtered, plane_inlier_leaf_size_ );
        transformPointCloud( *cloud_filtered, *global_plane.cloud, transform, global_plane.color );
//        transformPointCloud( *plane.cloud_boundary, *global_plane.cloud_boundary, transform );
//        transformPointCloud( *plane.cloud_hull, *global_plane.cloud_hull, transform );
        Eigen::Vector4f cen;
        pcl::compute3DCentroid( *global_plane.cloud, cen );
        global_plane.centroid.x = cen[0];
        global_plane.centroid.y = cen[1];
        global_plane.centroid.z = cen[2];
        landmarks_.push_back( global_plane );

        //
        landmark_max_count_ ++;
    }

    // Optimize factor graph
//    isam2_->update( graph_, initial_estimate_ );
    last_estimated_pose_ = init_pose;
    last_estimated_pose_tf_ = pose3ToTF( last_estimated_pose_ );

//    // Clear the factor graph and values for the next iteration
//    graph_.resize(0);
//    initial_estimate_.clear();
//    //

    cout << GREEN << " Register first frame in the map." << endl;
//    initial_estimate_.print(" - Init pose: ");
    cout << " - Initial pose: " << endl;
    printTransform( init_pose.matrix() );
    cout << RESET << endl;

    return true;
}

bool GTMapping::isKeyFrame( const Frame &frame )
{
    tf::Transform rel_tf = last_estimated_pose_tf_.inverse() * frame.pose_;
    double rad, distance;
    calAngleAndDistance( rel_tf, rad, distance );

    if( distance > keyframe_linear_threshold_ || rad > keyframe_angular_threshold_ )
        return true;
    else
        return false;
}

// get predicted landmarks
std::vector<OrientedPlane3> GTMapping::getPredictedObservation( const std::vector<OrientedPlane3> &landmarks,
                                                              const Pose3 &pose )
{
    std::vector<OrientedPlane3> predicted_observations;
    for(int i = 0; i < landmarks.size(); i++)
    {
        predicted_observations.push_back( landmarks[i].transform( pose ) );
    }
    return predicted_observations;
}

// simple euclidian distance
void GTMapping::matchPlanes( const std::vector<OrientedPlane3> &predicted_observations,
                           const std::vector<PlaneType> &landmarks,
                           const std::vector<OrientedPlane3> &observations,
                           const std::vector<PlaneType> &observed_planes,
                           const Pose3 pose,
                           std::vector<PlanePair> &pairs)
{
//    Eigen::VectorXd paired = Eigen::VectorXd::Zero( predicted_observations.size() );
    for( int i = 0; i < observations.size(); i++)
    {
        const PlaneType &observed = observed_planes[i];
        const OrientedPlane3 &obs = observations[i];
        // plane coordinate
        Point3 point( observed.centroid.x, observed.centroid.y, observed.centroid.z );
        Point3 col3 = obs.normal().point3();
        Matrix32 basis = obs.normal().basis();
        Point3 col1( basis(0,0), basis(1,0), basis(2,0) );
        Point3 col2( basis(0,1), basis(1,1), basis(2,1) );
        Rot3 rot3( col1, col2, col3 );
        Pose3 local( rot3, point);
//        local.print("local coord: ");
        // Transform observation to local frame
        const OrientedPlane3 &lobs = obs.transform( local );
        int min_index = -1;
        int max_size = 0;
        for( int l = 0; l < predicted_observations.size(); l++)
        {
//            if( paired[l] )
//                continue;

            const PlaneType &glm = landmarks[l];
            const OrientedPlane3 &plm = predicted_observations[l];

            // check if alive
            if( !glm.valid )
                continue;

            // Transform landmark to local frame
            const OrientedPlane3 &llm = plm.transform( local );
            double cs = lobs.normal().dot( llm.normal() );
            double dr_error = acos( cs );
            double ds_error = fabs( lobs.distance() - llm.distance() );
//            cout << CYAN << "  - " << i << "*" << l << ": " << dr_error << "("<< plane_match_direction_threshold_ << "), "
//                 << ds_error << "(" << plane_match_distance_threshold_ << ")" << RESET << endl;

//            double dir_error = acos( obs.normal().dot( plm.normal() ));
//            double dis_error = fabs( obs.distance() - plm.distance() );
//            cout << YELLOW << "  - " << i << "*" << l << ": " << dir_error << "("<< plane_match_direction_threshold_ << "), "
//                 << dis_error << "(" << plane_match_distance_threshold_ << ")" << RESET << endl;
            if( (fabs(dr_error) < plane_match_direction_threshold_)
                    && (ds_error < plane_match_distance_threshold_) )
            {
                if( glm.cloud->size() > max_size )
                {
                    if( plane_match_check_overlap_ && !checkOverlap( glm.cloud, glm.coefficients, observed.cloud, pose ) )
                        continue;
    //                min_d = d;
                    min_index = l;
                    max_size = glm.cloud->size();

                }
            }
        }
        if( min_index >= 0 )
        {
//            paired[min_index] = 1;
            pairs.push_back( PlanePair(i, min_index) );
        }
    }
}

bool GTMapping::checkOverlap( const PointCloudTypePtr &landmark_cloud,
                            const OrientedPlane3 &landmark,
                            const PointCloudTypePtr &observation,
                            const Pose3 &pose)
{
    // transform and project pointcloud
    PointCloudTypePtr cloud( new PointCloudType ), cloud_projected( new PointCloudType );
    transformPointCloud( *observation, *cloud, pose.matrix() );
    projectPoints( *cloud, landmark.planeCoefficients(), *cloud_projected );

    // build octree
    float resolution = plane_inlier_leaf_size_;
    pcl::octree::OctreePointCloud<PointType> octreeD (resolution);
    octreeD.setInputCloud( landmark_cloud );
    octreeD.addPointsFromInputCloud();

    // check if occupied
    int collision = 0;
    PointCloudType::iterator it = cloud_projected->begin();
    for( ; it != cloud_projected->end(); it++)
    {
        PointType &pt = *it;
        if( octreeD.isVoxelOccupiedAtPoint( pt ) )
            collision ++;
    }

    cout << GREEN << "  - collision: " << collision << "/" << cloud_projected->size() << RESET << endl;

    double alpha = ((float)collision) / (float)cloud_projected->size();
    if( alpha < plane_match_overlap_alpha_ )
        return false;
    else
        return true;

    return true;
}

// indices of lm1 must bigger than that of lm2
bool GTMapping::checkLandmarksOverlap( const PlaneType &lm1, const PlaneType &lm2)
{
    // project lm2 inlier to lm1 plane
    PointCloudTypePtr cloud_projected( new PointCloudType );
    projectPoints( *lm2.cloud, lm1.coefficients, *cloud_projected );

    // build octree from lm1
    float resolution = plane_inlier_leaf_size_;
    pcl::octree::OctreePointCloud<PointType> octreeD (resolution);
    octreeD.setInputCloud( lm1.cloud );
    octreeD.addPointsFromInputCloud();

    // check if occupied
    int collision = 0;
    PointCloudType::iterator it = cloud_projected->begin();
    for( ; it != cloud_projected->end(); it++)
    {
        PointType &pt = *it;
        if( octreeD.isVoxelOccupiedAtPoint( pt ) )
        {
            collision ++;
            if(collision > 10)
                return true;
        }
    }

//    cout << GREEN << "  - collision: " << collision << "/" << cloud_projected->size() << RESET << endl;

//    double alpha = ((float)collision) / (float)cloud_projected->size();
//    if( alpha < plane_match_overlap_alpha_ )
//        return false;
//    else
//        return true;

    return false;
}

// just do project and voxel filter
void GTMapping::mergeLandmarkInlier( PlaneType &from, PlaneType &to)
{
    PointCloudTypePtr cloud( new PointCloudType );
    projectPoints( *from.cloud, to.coefficients, *cloud);
    *cloud += *to.cloud;
    voxelGridFilter( cloud, to.cloud, plane_inlier_leaf_size_ );
}

// return true if being refined.
bool GTMapping::refinePlanarMap()
{
//    cout << RED << ", lm size: " << landmarks_.size()
//         << ", pl size: " << estimated_planes_.size() << endl;

    // find co-planar landmark pair
    bool find_coplanar = false;
    const int num = landmarks_.size();
    const double direction_threshold = planar_merge_direction_threshold_;
    const double distance_threshold = planar_merge_distance_threshold_;
    for( int i = 0; i < (num - 1 ); i++)
    {
        PlaneType &p1 = landmarks_[i];
        OrientedPlane3 &lm1 = estimated_planes_[i];

        // check is alive
        if( !p1.valid )
            continue;

        // plane coordinate
        Point3 point( p1.centroid.x, p1.centroid.y, p1.centroid.z );
        Point3 col3 = lm1.normal().point3();
        Matrix32 basis = lm1.normal().basis();
        Point3 col1( basis(0,0), basis(1,0), basis(2,0) );
        Point3 col2( basis(0,1), basis(1,1), basis(2,1) );
        Rot3 rot3( col1, col2, col3 );
        Pose3 local( rot3, point);
        // Transform observation to local frame
        const OrientedPlane3 &llm1 = lm1.transform( local );

        for( int j = i+1; j < num; j++)
        {
            PlaneType &p2 = landmarks_[j];
            OrientedPlane3 &lm2 = estimated_planes_[j];

            // check if alive
            if( !p2.valid )
                continue;

            /// Match two plane
            // Transform landmark to local frame
            const OrientedPlane3 &llm2 = lm2.transform( local );
            double dr_error = acos( llm1.normal().dot( llm2.normal() ));
            double ds_error = fabs( llm1.distance() - llm2.distance() );
//            cout << CYAN << "  - " << i << "*" << j << ": " << dr_error << "("<< direction_threshold << "), "
//                 << ds_error << "(" << distance_threshold << ")" << RESET << endl;
            if( (fabs(dr_error) < direction_threshold)
                    && (ds_error < distance_threshold) )
            {
                // check if overlap
                bool overlap = false;
                if( p1.cloud->size() < p2.cloud->size() )
                    overlap = checkLandmarksOverlap( p2, p1);
                else
                    overlap = checkLandmarksOverlap( p1, p2);

                if( overlap )
                {
                    // merge, make smaller one invalid
                    if( p1.cloud->size() < p2.cloud->size() )
                    {
                        mergeLandmarkInlier( p1, p2);
                        p1.valid = false;
                        find_coplanar = true;
                        cout << RED << "  -- merge co-planar: " << i << " to " << j << RESET << endl;
                        break;
                    }
                    else
                    {
                        mergeLandmarkInlier( p2, p1);
                        find_coplanar = true;
                        cout << RED << "  -- merge co-planar: " << j << " to " << i << RESET << endl;
                        p2.valid = false;
                    }
                } // end of if overlap

            }

        } // end of for( int j = i+1; j < num; j++)

    } // end of for( int i = 0; i < (num - 1 ); i++)

    return find_coplanar;
}

bool GTMapping::removeBadInlier()
{
    float radius = 0.1;
    int min_neighbors = M_PI * radius *radius
            / (plane_inlier_leaf_size_ * plane_inlier_leaf_size_) *  planar_bad_inlier_alpha_;
    cout << GREEN << " Remove bad inlier, radius = " << radius
         << ", minimum neighbours = " << min_neighbors << RESET << endl;
    for( int i = 0; i < landmarks_.size(); i++)
    {
        PlaneType &lm = landmarks_[i];
        if( !lm.valid )
            continue;
        // build the filter
        pcl::RadiusOutlierRemoval<PointType> ror;
        ror.setInputCloud( lm.cloud );
        ror.setRadiusSearch( radius );
        ror.setMinNeighborsInRadius ( min_neighbors );
        // apply filter
        PointCloudTypePtr cloud_filtered( new PointCloudType );
        ror.filter( *cloud_filtered );
        // swap
        lm.cloud->swap( *cloud_filtered );
    }
}

void GTMapping::updateSlamResult( std::vector<Pose3> &poses, std::vector<OrientedPlane3> &planes )
{
    // clear buffer
    poses.clear();
    planes.clear();

    //
    Values values = isam2_->calculateBestEstimate();
    for(int i = 0; i < pose_count_; i++)
    {
        Key xn = Symbol('x', i);
        Pose3 pose = values.at(xn).cast<Pose3>();
        poses.push_back( pose );
    }
    for(int i = 0; i < landmark_max_count_; i++)
    {

        Key ln = Symbol('l', i);
        if( values.exists(ln) )
        {
            OrientedPlane3 predicted = values.at(ln).cast<OrientedPlane3>();
            planes.push_back( predicted );
        }
        else
        {
            planes.push_back( OrientedPlane3() );
        }
    }
}

void GTMapping::updateLandmarks( std::vector<PlaneType> &landmarks,
                               const std::vector<PlaneType> &observations,
                               const std::vector<PlanePair> &pairs,
                               const Pose3 &estimated_pose,
                               const std::vector<OrientedPlane3> &estimated_planes )
{
//    if( landmarks.size() != estimated_planes.size() )
//    {
//        cout << RED << "[Error]: landmark.size() != estimated_planes.size()" << RESET << endl;
//        return;
//    }

    Eigen::Matrix4d transform = estimated_pose.matrix();

    for( int i = 0; i < landmarks.size(); i++)
    {
        PlaneType &lm = landmarks[i] ;
        lm.coefficients = estimated_planes[i].planeCoefficients();
    }

    Eigen::VectorXi unpairs = Eigen::VectorXi::Ones( observations.size() );

    for( int i = 0; i < pairs.size(); i++)
    {
        const int iobs = pairs[i].iobs;
        const int ilm = pairs[i].ilm;
        unpairs[iobs] = 0;
        const PlaneType &obs = observations[ iobs ];
        PlaneType &lm = landmarks[ ilm ] ;

        PointCloudTypePtr cloud( new PointCloudType ), cloud_filtered( new PointCloudType );
//        PointCloudTypePtr cloud_boundary( new PointCloudType );
//        PointCloudTypePtr cloud_hull( new PointCloudType );

        // downsample
        voxelGridFilter( obs.cloud, cloud_filtered, plane_inlier_leaf_size_ );
        // transform cloud
        transformPointCloud( *cloud_filtered, *cloud, transform, lm.color );
//        transformPointCloud( *obs.cloud_boundary, *cloud_boundary, transform );
//        transformPointCloud( *obs.cloud_hull, *cloud_hull, transform );

        // sum
        *cloud += *lm.cloud;
//        *lm.cloud_boundary += *cloud_boundary;
//        *lm.cloud_hull += *cloud_hull;

        // project
        projectPoints( *cloud, lm.coefficients, *cloud_filtered );
//        projectPoints( *lm.cloud_boundary, lm.coefficients, *cloud_boundary );
//        projectPoints( *lm.cloud_hull, lm.coefficients, *cloud_hull );

        // refresh inlier, boundary, hull
        voxelGridFilter( cloud_filtered, lm.cloud, plane_inlier_leaf_size_ );
//        cloudHull( cloud_boundary, lm.cloud_boundary );
//        cloudHull( cloud, lm.cloud_hull );

        // compute new centroid
        Eigen::Vector4d cen;
        pcl::compute3DCentroid( *lm.cloud, cen);
        lm.centroid.x = cen[0];
        lm.centroid.y = cen[1];
        lm.centroid.z = cen[2];
    }

    // Add new to landmarks buffer
    int lm_num = landmarks.size();
    for( int i = 0; i < unpairs.rows(); i++)
    {
        if( unpairs[i] )
        {
            const PlaneType &obs = observations[i];
            PlaneType global_plane;
            global_plane.coefficients = estimated_planes[lm_num].planeCoefficients();
            global_plane.color.Blue = rng_.uniform(0, 255);
            global_plane.color.Green = rng_.uniform(0, 255);
            global_plane.color.Red = rng_.uniform(0, 255);
            global_plane.color.Alpha = 255;
            PointCloudTypePtr cloud_filtered( new PointCloudType );
            voxelGridFilter( obs.cloud, cloud_filtered, plane_inlier_leaf_size_ );
            transformPointCloud( *cloud_filtered, *global_plane.cloud, transform, global_plane.color );
//            transformPointCloud( *cloud_filtered, *global_plane.cloud, transform );
//            transformPointCloud( *obs.cloud_boundary, *global_plane.cloud_boundary, transform );
//            transformPointCloud( *obs.cloud_hull, *global_plane.cloud_hull, transform );
            Eigen::Vector4f cen;
            pcl::compute3DCentroid( *global_plane.cloud, cen );
            global_plane.centroid.x = cen[0];
            global_plane.centroid.y = cen[1];
            global_plane.centroid.z = cen[2];

            landmarks.push_back( global_plane );
            lm_num = landmarks.size();
        }
    }

}

void GTMapping::voxelGridFilter( const PointCloudTypePtr &cloud,
                               PointCloudTypePtr &cloud_filtered,
                               float leaf_size)
{
    // Create the filtering object
    pcl::VoxelGrid<PointType> sor;
    sor.setInputCloud ( cloud );
    sor.setLeafSize ( leaf_size, leaf_size, leaf_size );
    sor.filter ( *cloud_filtered );
}

void GTMapping::optimizeGraph( int n )
{
    while( n > 0 )
    {
        isam2_->update();
        n--;
    }
//    isam2_->print( "Map Graph");
}

void GTMapping::publishOptimizedPose()
{
    geometry_msgs::PoseStamped msg = pose3ToGeometryPose( last_estimated_pose_ );
    msg.header.frame_id = map_frame_;
    msg.header.stamp = ros::Time::now();
    optimized_pose_publisher_.publish( msg );
}

void GTMapping::publishOptimizedPath()
{
    // publish trajectory
    nav_msgs::Path path;
    path.header.frame_id = map_frame_;
    path.header.stamp = ros::Time::now();
    for(int i = 0; i < estimated_poses_.size(); i++)
    {
        path.poses.push_back( pose3ToGeometryPose( estimated_poses_[i] ) );
    }
    optimized_path_publisher_.publish( path );
    cout << GREEN << " Publisher optimized path, p = " << estimated_poses_.size() << RESET << endl;
}

void GTMapping::publishMapCloud()
{
    PointCloudTypePtr cloud ( new PointCloudType );

    for( int i = 0; i < landmarks_.size(); i++)
    {
        const PlaneType &lm = landmarks_[i];
        if( !lm.valid )
            continue;
       *cloud += *lm.cloud;
    }

    sensor_msgs::PointCloud2 cloud2;
    pcl::toROSMsg( *cloud, cloud2);
    cloud2.header.frame_id = map_frame_;
    cloud2.header.stamp = ros::Time::now();
    cloud2.is_dense = false;

    map_cloud_publisher_.publish( cloud2 );

    cout << GREEN << " Publisher map as sensor_msgs/PointCloud2." << RESET << endl;
}

void GTMapping::updateMapViewer()
{
    // Pose, Path, MapCloud
    publishOptimizedPose();
    publishOptimizedPath();
    publishMapCloud();
    // Map in pcl visualization
    viewer_->removeMap();
    viewer_->displayMapLandmarks( landmarks_, "Map" );
    viewer_->spinMapOnce();
}

void GTMapping::saveMapPCD( const std::string &filename )
{
    PointCloudTypePtr cloud ( new PointCloudType );
    for( int i = 0; i < landmarks_.size(); i++)
    {
        PlaneType &lm = landmarks_[i];
        if( !lm.valid )
            continue;
        // Add color
        setPointCloudColor( *(lm.cloud), lm.color );
        *cloud += *(lm.cloud);
    }

    pcl::io::savePCDFileASCII ( filename, *cloud);
}

std::vector<geometry_msgs::PoseStamped> GTMapping::getOptimizedPath()
{
    std::vector<geometry_msgs::PoseStamped> poses;
    for( int i = 0; i < estimated_poses_.size(); i++)
    {
        poses.push_back( pose3ToGeometryPose( estimated_poses_[i] ) );
    }
    return poses;
}


void GTMapping::gtMappingReconfigCallback(plane_slam::GTMappingConfig &config, uint32_t level)
{
    //
    use_keyframe_ = config.use_keyframe;
    keyframe_linear_threshold_ = config.keyframe_linear_threshold;
    keyframe_angular_threshold_ = config.keyframe_angular_threshold * DEG_TO_RAD;
    //
    isam2_relinearize_threshold_ = config.isam2_relinearize_threshold;
    isam2_relinearize_skip_ = config.isam2_relinearize_skip;
    //
    plane_match_direction_threshold_ = config.plane_match_direction_threshold * DEG_TO_RAD;
    plane_match_distance_threshold_ = config.plane_match_distance_threshold;
    plane_match_check_overlap_ = config.plane_match_check_overlap;
    plane_match_overlap_alpha_ = config.plane_match_overlap_alpha;
    plane_inlier_leaf_size_ = config.plane_inlier_leaf_size;
    plane_hull_alpha_ = config.plane_hull_alpha;
    //
    refine_planar_map_ = config.refine_planar_map;
    planar_merge_direction_threshold_ = config.planar_merge_direction_threshold * DEG_TO_RAD;
    planar_merge_distance_threshold_ = config.planar_merge_distance_threshold;
    planar_bad_inlier_alpha_ = config.planar_bad_inlier_alpha;
    //
    publish_optimized_path_ = config.publish_optimized_path;

    cout << GREEN <<" GTSAM Mapping Config." << RESET << endl;
}

bool GTMapping::optimizeGraphCallback(std_srvs::Trigger::Request &req, std_srvs::Trigger::Response &res)
{
    if( isam2_->empty() )
    {
        res.success = false;
        res.message = " Failed, graph is empty.";
    }
    else
    {
        optimizeGraph();    // graph optimizing
        updateMapViewer();  // update map visualization
        res.success = true;
        res.message = " Optimize graph for 10 times, and update map viewer.";
    }

    cout << GREEN << res.message << RESET << endl;
    return true;
}

bool GTMapping::saveGraphCallback(std_srvs::Trigger::Request &req, std_srvs::Trigger::Response &res)
{
    if( isam2_->empty() )
    {
        res.success = false;
        res.message = " Failed, graph is empty.";
    }
    else
    {
        std::string filename = "/home/lizhi/bags/result/"+timeToStr()+"_graph.dot";
        saveGraphDot( filename );
        res.success = true;
        res.message = " Save isam2 graph as dot file: " + filename + ".";
    }

    cout << GREEN << res.message << RESET << endl;
    return true;
}

bool GTMapping::saveMapPCDCallback(std_srvs::Trigger::Request &req, std_srvs::Trigger::Response &res)
{
    if( !landmarks_.size() )
    {
        res.success = false;
        res.message = " Failed, map is empty.";
    }
    else
    {
        std::string filename = "/home/lizhi/bags/result/" + timeToStr() + "_map.pcd";
        saveMapPCD( filename );
        res.success = true;
        res.message = " Save map as pcd file: " + filename + ".";
    }

    cout << GREEN << res.message << RESET << endl;
    return true;
}

bool GTMapping::removeBadInlierCallback(std_srvs::Trigger::Request &req, std_srvs::Trigger::Response &res)
{
    if( !landmarks_.size() )
    {
        res.success = false;
        res.message = " Failed, map is empty.";
    }
    else
    {
        removeBadInlier();  // remove bad inlier in landmarks
        updateMapViewer();  // update map visualization
        //
        res.success = true;
        res.message = " Remove landmark bad inlier.";
    }

    cout << GREEN << res.message << RESET << endl;
    return true;
}

} // end of namespace plane_slam
