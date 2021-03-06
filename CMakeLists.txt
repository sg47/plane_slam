cmake_minimum_required(VERSION 2.8.3)
project(plane_slam)


# Check C++11 or C++0x support
include(CheckCXXCompilerFlag)
CHECK_CXX_COMPILER_FLAG("-std=c++11" COMPILER_SUPPORTS_CXX11)
CHECK_CXX_COMPILER_FLAG("-std=c++0x" COMPILER_SUPPORTS_CXX0X)
if(COMPILER_SUPPORTS_CXX11)
   set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11")
   add_definitions(-DCOMPILEDWITHC11)
   message(STATUS "Using flag -std=c++11.")
elseif(COMPILER_SUPPORTS_CXX0X)
   set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++0x")
   add_definitions(-DCOMPILEDWITHC0X)
   message(STATUS "Using flag -std=c++0x.")
else()
   message(FATAL_ERROR "The compiler ${CMAKE_CXX_COMPILER} has no C++11 support. Please use a different C++ compiler.")
endif()

#SET( CMAKE_BUILD_TYPE RelWithDebInfo )
SET(CMAKE_BUILD_TYPE Release)
SET(ROS_BUILD_TYPE Release)

## Find catkin macros and libraries
## if COMPONENTS list like find_package(catkin REQUIRED COMPONENTS xyz)
## is used, also find other catkin packages
find_package(catkin REQUIRED COMPONENTS
  cv_bridge
  dynamic_reconfigure
  image_transport
  message_filters
  octomap_ros
  octomap
  pcl_ros
  line_based_plane_segment
  roscpp
  sensor_msgs
  tf
)

## System dependencies are found with CMake's conventions
# find_package(Boost REQUIRED COMPONENTS system)

LIST( APPEND CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR}/cmake_modules )

## System dependencies are found with CMake's conventions
find_package(Boost REQUIRED COMPONENTS system)
find_package(Eigen3 REQUIRED)
find_package(OpenCV 2.4.8 REQUIRED)
find_package(PCL 1.7 REQUIRED)

link_libraries(${OpenCV_LIBS})
include_directories(SYSTEM ${OpenCV_INCLUDE_DIRS})

find_package(octomap REQUIRED)
include_directories(${OCTOMAP_INCLUDE_DIRS})
link_libraries(${OCTOMAP_LIBRARIES})

# Include GTSAM CMake tools
find_package(GTSAMCMakeTools)
include(GtsamBuildTypes) # Load build type flags and default to Debug mode
include(GtsamTesting)    # Easy functions for creating unit tests and scripts
include(GtsamMatlabWrap) # Automatic MATLAB wrapper generation

# Ensure that local folder is searched before library folders
include_directories(BEFORE "${PROJECT_SOURCE_DIR}")
#
find_package(GTSAM REQUIRED) # Uses installed package
include_directories(${GTSAM_INCLUDE_DIR})
#
# Find TBB
find_package(TBB)


# Build plane slam node
#option(BUILD_PLANE_SLAM     "Enable/Disable building of plane slam node"       ON)
#option(BUILD_TOOLS_NODE  "Enable/Disable building of tools node" OFF)
SET(BUILD_PLANE_SLAM ON)

################################################
## Declare ROS dynamic reconfigure parameters ##
################################################

## To declare and build dynamic reconfigure parameters within this
## package, follow these steps:
## * In the file package.xml:
##   * add a build_depend and a run_depend tag for "dynamic_reconfigure"
## * In this file (CMakeLists.txt):
##   * add "dynamic_reconfigure" to
##     find_package(catkin REQUIRED COMPONENTS ...)
##   * uncomment the "generate_dynamic_reconfigure_options" section below
##     and list every .cfg file to be processed

## Generate dynamic reconfigure parameters in the 'cfg' folder
generate_dynamic_reconfigure_options(
  cfg/PlaneSlam.cfg
  cfg/G2OMapping.cfg
  cfg/GTMapping.cfg
  cfg/LineBasedSegment.cfg
  cfg/OrganizedSegment.cfg
  cfg/Viewer.cfg
  cfg/Tracking.cfg
)

###################################
## catkin specific configuration ##
###################################
## The catkin_package macro generates cmake config files for your package
## Declare things to be passed to dependent projects
## INCLUDE_DIRS: uncomment this if you package contains header files
## LIBRARIES: libraries you create in this project that dependent projects also need
## CATKIN_DEPENDS: catkin_packages dependent projects also need
## DEPENDS: system dependencies of this project that dependent projects also need
catkin_package(
  INCLUDE_DIRS include
#  LIBRARIES plane_slam
#  CATKIN_DEPENDS cv_bridge dynamic_reconfigure message_filters pcl_ros plane_from_line roscpp sensor_msgs tf
#  DEPENDS system_lib
)

###########
## Build ##
###########

## Specify additional locations of header files
## Your package locations should be listed before other locations
#include_directories(include)
include_directories(
  ${PROJECT_SOURCE_DIR}
  ${PROJECT_SOURCE_DIR}/include
  ${OpenCV_INCLUDE_DIRS}
  ${EIGEN3_INCLUDE_DIR}
  ${PCL_INCLUDE_DIRS}
  ${Wrap_INCLUDE_DIR}
  ${CppUnitLite_INCLUDE_DIR}
  ${GTSAM_INCLUDE_DIRS}
  ${catkin_INCLUDE_DIRS}
)


if(BUILD_PLANE_SLAM)
    add_library( ${PROJECT_NAME}
        src/kinect_listener.cpp
        src/viewer.cpp
        src/gtsam_mapping.cpp
        src/tracking.cpp
        src/frame.cpp
        src/line_based_plane_segmentor.cpp
        src/organized_multi_plane_segmentor.cpp
        src/orb_extractor.cpp
        src/utils.cpp
        src/itree.cpp
        src/feature_adjuster.cpp
    )

    ## Specify libraries to link a library or executable target against
    target_link_libraries( ${PROJECT_NAME}
        gtsam
        line_based_plane_segment
        ${OpenCV_LIBS}
        ${EIGEN3_LIBS}
        ${PCL_LIBRARIES}
        ${CppUnitLite_LIBS}
        ${catkin_LIBRARIES}
    )
    add_dependencies(${PROJECT_NAME} ${PROJECT_NAME}_gencfg)

    ## Declare a C++ executable
    add_executable(plane_slam_node src/plane_slam_node.cpp)
    add_dependencies(plane_slam_node ${PROJECT_NAME}_gencfg)
    target_link_libraries(plane_slam_node ${PROJECT_NAME})

    ## Declare a C++ executable
    add_executable(plane_slam_node_bagfile src/plane_slam_node_bagfile.cpp)
    add_dependencies(plane_slam_node_bagfile ${PROJECT_NAME}_gencfg)
    target_link_libraries(plane_slam_node_bagfile  ${PROJECT_NAME})
endif()


#add_executable(pcd_publisher tools/pcd_publisher.cpp)
#target_link_libraries(pcd_publisher
#    ${catkin_LIBRARIES}
#    ${PCL_LIBRARIES}
#)

#add_executable(pcd_visualizer tools/pcd_visualizer.cpp)
#target_link_libraries(pcd_visualizer
#    ${catkin_LIBRARIES}
#    ${PCL_LIBRARIES}
#)

#add_executable(path_publisher tools/path_publisher.cpp)
#target_link_libraries(path_publisher
#    ${catkin_LIBRARIES}
#)

#add_executable(path_visualizer tools/path_visualizer.cpp)
#target_link_libraries(path_visualizer
#    ${catkin_LIBRARIES}
#    ${PCL_LIBRARIES}
#)

#add_executable(map_visualizer tools/map_visualizer.cpp)
#target_link_libraries(map_visualizer
#    ${catkin_LIBRARIES}
#    ${PCL_LIBRARIES}
#)

#add_executable(runtime_analyze tools/runtime_analyze.cpp)
#target_link_libraries(runtime_analyze  ${catkin_LIBRARIES})

#add_executable(gmapping_pose tools/gmapping_pose.cpp)
#target_link_libraries(gmapping_pose ${catkin_LIBRARIES})

#add_executable(path_odom_to_map tools/path_odom_to_map.cpp)
#target_link_libraries(path_odom_to_map ${catkin_LIBRARIES})

add_executable(rgb_seq_stamp tools/rgb_seq_stamp.cpp)
target_link_libraries(rgb_seq_stamp ${catkin_LIBRARIES})

#add_executable(orb_map_cloud tools/orb_map_cloud.cpp)
#target_link_libraries(orb_map_cloud ${catkin_LIBRARIES})

#add_executable(octomap_publisher tools/octomap_publisher.cpp)
#target_link_libraries(octomap_publisher ${catkin_LIBRARIES})

add_executable(assign_path tools/assign_path.cpp)
target_link_libraries(assign_path ${EIGEN3_LIBS} ${PCL_LIBRARIES} ${catkin_LIBRARIES})

add_executable(transform_point_cloud tools/transform_point_cloud.cpp)
target_link_libraries(transform_point_cloud ${EIGEN3_LIBS} ${PCL_LIBRARIES} ${catkin_LIBRARIES})
