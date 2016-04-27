#ifndef PLANE_SLAM_H
#define PLANE_SLAM_H

#include <gtsam/geometry/Pose3.h>
#include <gtsam/geometry/OrientedPlane3.h>
#include <gtsam/inference/Key.h>
#include <gtsam/inference/Symbol.h>
#include <gtsam/slam/PriorFactor.h>
#include <gtsam/slam/BetweenFactor.h>
#include <gtsam/slam/OrientedPlane3Factor.h>
#include <gtsam/nonlinear/NonlinearFactorGraph.h>
#include <gtsam/nonlinear/GaussNewtonOptimizer.h>
#include <gtsam/nonlinear/Marginals.h>
#include <gtsam/nonlinear/Values.h>
#include <gtsam/nonlinear/ISAM2.h>

using namespace std;
using namespace gtsam;

class PlaneSlam
{
public:
    PlaneSlam();

    void initialize(const Pose3 &pose, const std::vector<Eigen::VectorXd> &plane_coeffs,
                    const std::vector<Eigen::MatrixXd> &covariances);

    void planeSlam(const Pose3 &odom, const std::vector<Eigen::VectorXd> &plane_coeffs,
                   const std::vector<Eigen::MatrixXd> &covariances);

private:

    ISAM2Params isam2_parameters_;
    ISAM2* isam2_;

    // Create a Factor Graph and Values to hold the new data
    NonlinearFactorGraph graph_; // factor graph
    Values initial_estimate_; // initial guess

    bool first_pose_;
    vector<Pose3> odom_poses_;
    int pose_count_;
    int landmakr_count_;
};

#endif // PLANE_SLAM_H
