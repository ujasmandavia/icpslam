
#include "icpslam/octree_mapper.h"

#include <tf/transform_datatypes.h>

#include <pcl/filters/voxel_grid.h>
#include <pcl/registration/gicp.h>
#include <pcl/registration/icp.h>
#include <pcl_conversions/pcl_conversions.h>
#include <pcl_ros/impl/transforms.hpp>

#include "utils/geometric_utils.h"
#include "utils/messaging_utils.h"

OctreeMapper::OctreeMapper(const ros::NodeHandle& nh, const ros::NodeHandle& pnh)
    : nh_(nh),
      pnh_(pnh),
      map_cloud_(new pcl::PointCloud<pcl::PointXYZ>()),
      map_octree_(new pcl::octree::OctreePointCloudSearch<pcl::PointXYZ>(0.5)) {
  init();
}

void OctreeMapper::init() {
  loadParameters();
  resetMap();
  advertisePublishers();
  registerSubscribers();

  ROS_INFO("IcpSlam: Octree mapper started");
}

void OctreeMapper::loadParameters() {
  pnh_.param("verbosity_level", verbosity_level_, 2);

  // Tf frames
  pnh_.param("map_frame", map_frame_, std::string("map"));
  pnh_.param("odom_frame", odom_frame_, std::string("odom"));
  pnh_.param("robot_frame", robot_frame_, std::string("base_link"));
  pnh_.param("laser_frame", laser_frame_, std::string("laser"));

  // Octree
  pnh_.param("octree_resolution", octree_resolution_, 0.5);
}

void OctreeMapper::advertisePublishers() {
  map_cloud_pub_ = pnh_.advertise<sensor_msgs::PointCloud2>("octree_mapper/map_cloud", 1, true);
  nn_cloud_pub_ = pnh_.advertise<sensor_msgs::PointCloud2>("octree_mapper/nn_cloud", 1);
  registered_cloud_pub_ = pnh_.advertise<sensor_msgs::PointCloud2>("octree_mapper/registered_cloud", 1);
  refined_path_pub_ = pnh_.advertise<nav_msgs::Path>("octree_mapper/refined_path", 1, true);
}

void OctreeMapper::registerSubscribers() {
  // increment_cloud_sub_ = nh_.subscribe("octree_mapper/increment_cloud", 10, &OctreeMapper::incrementCloudCallback, this);
}

void OctreeMapper::resetMap() {
  map_octree_.reset(new pcl::octree::OctreePointCloudSearch<pcl::PointXYZ>(octree_resolution_));
  map_cloud_.reset(new pcl::PointCloud<pcl::PointXYZ>());
  map_octree_->setInputCloud(map_cloud_);
}

/* This function is inspired on https://github.com/erik-nelson/point_cloud_mapper */
void OctreeMapper::addPointsToMap(pcl::PointCloud<pcl::PointXYZ>::Ptr input_cloud) {
  for (size_t i = 0; i < input_cloud->points.size(); i++) {
    pcl::PointXYZ point = input_cloud->points[i];
    if (!map_octree_->isVoxelOccupiedAtPoint(point)) {
      map_octree_->addPointToCloud(point, map_cloud_);
    }
  }
}

// Credits to Erik Nelson, creator of BLAM! (https://github.com/erik-nelson/blam)
bool OctreeMapper::approxNearestNeighbors(
    const pcl::PointCloud<pcl::PointXYZ>::Ptr& cloud, pcl::PointCloud<pcl::PointXYZ>::Ptr& nearest_neighbors) {
  nearest_neighbors->points.clear();

  // Iterate over points in the input point cloud, finding the nearest neighbor
  // for every point and storing it in the output array.
  for (size_t ii = 0; ii < cloud->points.size(); ++ii) {
    // Search for nearest neighbor and store.
    float unused = 0.0f;
    int result_index = -1;

    map_octree_->approxNearestSearch(cloud->points[ii], result_index, unused);
    if (result_index >= 0)
      nearest_neighbors->push_back(map_cloud_->points[result_index]);
  }

  return (nearest_neighbors->points.size() > 0);
}

void OctreeMapper::transformCloudToPoseFrame(
    const pcl::PointCloud<pcl::PointXYZ>::Ptr& in_cloud, const Pose6DOF& pose, pcl::PointCloud<pcl::PointXYZ>::Ptr& out_cloud) {
  tf::Transform tf_cloud_in_pose = pose.toTFTransform();
  try {
    pcl_ros::transformPointCloud(*in_cloud, *out_cloud, tf_cloud_in_pose);
  } catch (tf::TransformException e) {
  }
}

bool OctreeMapper::estimateTransformICP(
    const pcl::PointCloud<pcl::PointXYZ>::Ptr& curr_cloud, const pcl::PointCloud<pcl::PointXYZ>::Ptr& nn_cloud, Pose6DOF& transform) {
  // ROS_INFO("Estimation of transform via ICP");
  pcl::GeneralizedIterativeClosestPoint<pcl::PointXYZ, pcl::PointXYZ> icp;
  icp.setMaximumIterations(ICP_MAX_ITERS);
  icp.setTransformationEpsilon(ICP_EPSILON);
  icp.setMaxCorrespondenceDistance(ICP_MAX_CORR_DIST);
  icp.setRANSACIterations(0);
  icp.setInputSource(curr_cloud);
  icp.setInputTarget(nn_cloud);

  pcl::PointCloud<pcl::PointXYZ>::Ptr prev_cloud_in_curr_frame(new pcl::PointCloud<pcl::PointXYZ>()),
      curr_cloud_in_prev_frame(new pcl::PointCloud<pcl::PointXYZ>()), joint_cloud(new pcl::PointCloud<pcl::PointXYZ>());
  icp.align(*curr_cloud_in_prev_frame);
  Eigen::Matrix4d T = icp.getFinalTransformation().cast<double>();

  if (icp.hasConverged()) {
    ROS_WARN("ICP converged");
    transform = Pose6DOF(T, ros::Time().now());
    return true;
  }

  return false;
}

void OctreeMapper::publishPath(const Pose6DOF& latest_pose) {
  insertPoseInPath(latest_pose.toROSPose(), map_frame_, ros::Time().now(), refined_path_);
  refined_path_.header.stamp = ros::Time().now();
  refined_path_.header.frame_id = map_frame_;
  refined_path_pub_.publish(refined_path_);
}

bool OctreeMapper::refineTransformAndGrowMap(
    const ros::Time& stamp, const pcl::PointCloud<pcl::PointXYZ>::Ptr& cloud, const Pose6DOF& raw_pose, Pose6DOF& transform) {
  pcl::PointCloud<pcl::PointXYZ>::Ptr cloud_in_map(new pcl::PointCloud<pcl::PointXYZ>());
  transformCloudToPoseFrame(cloud, raw_pose, cloud_in_map);

  if (map_cloud_->points.size() == 0) {
    ROS_WARN("IcpSlam: Octree map is empty!");
    addPointsToMap(cloud_in_map);
    return false;
  }

  pcl::PointCloud<pcl::PointXYZ>::Ptr nn_cloud_in_map(new pcl::PointCloud<pcl::PointXYZ>());
  pcl::PointCloud<pcl::PointXYZ>::Ptr nn_cloud(new pcl::PointCloud<pcl::PointXYZ>());

  // Get closest points in map to current cloud via nearest-neighbors search
  approxNearestNeighbors(cloud_in_map, nn_cloud_in_map);
  transformCloudToPoseFrame(nn_cloud_in_map, raw_pose.inverse(), nn_cloud);

  if (nn_cloud_pub_.getNumSubscribers() > 0) {
    publishPointCloud(nn_cloud, robot_frame_, stamp, &nn_cloud_pub_);
  }

  if (estimateTransformICP(cloud, nn_cloud, transform)) {
    Pose6DOF refined_pose = raw_pose + transform;
    transformCloudToPoseFrame(cloud, refined_pose, cloud_in_map);
    addPointsToMap(cloud_in_map);

    if (map_cloud_pub_.getNumSubscribers() > 0) {
      publishPointCloud(map_cloud_, map_frame_, stamp, &map_cloud_pub_);
    }
    if (refined_path_pub_.getNumSubscribers() > 0) {
      publishPath(refined_pose);
    }
    if (registered_cloud_pub_.getNumSubscribers() > 0) {
      publishPointCloud(cloud_in_map, map_frame_, stamp, &registered_cloud_pub_);
    }
    return true;
  }

  return false;
}