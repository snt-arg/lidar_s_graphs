#ifndef __ROOM_UTILS_HPP_
#define __ROOM_UTILS_HPP_
#include <g2o/types/slam3d/edge_se3.h>
#include <g2o/types/slam3d/vertex_se3.h>
#include <g2o/types/slam3d_addons/vertex_plane.h>
#include <math.h>
#include <pcl/common/common.h>
#include <pcl/filters/passthrough.h>
#include <pcl/point_cloud.h>
#include <pcl/point_types.h>
#include <pcl_conversions/pcl_conversions.h>

#include <boost/format.hpp>
#include <cmath>
#include <g2o/edge_infinite_room_plane.hpp>
#include <g2o/edge_plane.hpp>
#include <g2o/edge_room.hpp>
#include <g2o/edge_se3_plane.hpp>
#include <g2o/edge_se3_point_to_plane.hpp>
#include <g2o/edge_se3_priorquat.hpp>
#include <g2o/edge_se3_priorvec.hpp>
#include <g2o/edge_se3_priorxy.hpp>
#include <g2o/edge_se3_priorxyz.hpp>
#include <g2o/edge_wall_two_planes.hpp>
#include <g2o/vertex_infinite_room.hpp>
#include <g2o/vertex_room.hpp>
#include <g2o/vertex_wall.hpp>
#include <iostream>
#include <s_graphs/floor_mapper.hpp>
#include <s_graphs/floors.hpp>
#include <s_graphs/graph_slam.hpp>
#include <s_graphs/graph_visualizer.hpp>
#include <s_graphs/infinite_rooms.hpp>
#include <s_graphs/information_matrix_calculator.hpp>
#include <s_graphs/keyframe.hpp>
#include <s_graphs/keyframe_mapper.hpp>
#include <s_graphs/keyframe_updater.hpp>
#include <s_graphs/loop_detector.hpp>
#include <s_graphs/map_cloud_generator.hpp>
#include <s_graphs/nmea_sentence_parser.hpp>
#include <s_graphs/plane_analyzer.hpp>
#include <s_graphs/plane_mapper.hpp>
#include <s_graphs/plane_utils.hpp>
#include <s_graphs/planes.hpp>
#include <s_graphs/room_mapper.hpp>
#include <s_graphs/rooms.hpp>
#include <string>
#include <unordered_map>

#include "geometry_msgs/msg/point.hpp"
#include "graph_manager_msgs/msg/attribute.hpp"
#include "graph_manager_msgs/msg/edge.hpp"
#include "graph_manager_msgs/msg/graph.hpp"
#include "graph_manager_msgs/msg/graph_keyframes.hpp"
#include "graph_manager_msgs/msg/node.hpp"
#include "pcl_ros/transforms.hpp"
#include "rclcpp/rclcpp.hpp"
#include "s_graphs/msg/plane_data.hpp"
#include "s_graphs/msg/planes_data.hpp"
#include "s_graphs/msg/room_data.hpp"
#include "s_graphs/msg/rooms_data.hpp"
#include "sensor_msgs/msg/point_cloud2.hpp"
#include "visualization_msgs/msg/marker_array.hpp"

struct PlaneGlobalRep {
  Eigen::Vector3d normal;
  Eigen::Vector3d point;
};

std::vector<const s_graphs::VerticalPlanes*> obtain_planes_from_room(
    const s_graphs::Rooms& room,
    const std::vector<s_graphs::VerticalPlanes>& x_vert_planes,
    const std::vector<s_graphs::VerticalPlanes>& y_vert_planes);

bool is_SE3_inside_a_room(const Eigen::Isometry3d& pose,
                          const std::vector<PlaneGlobalRep>& planes);

std::optional<Eigen::Vector3d> find_intersection(const Eigen::Vector3d& point1,
                                                 const Eigen::Vector3d& direction1,
                                                 const Eigen::Vector3d& point2,
                                                 const Eigen::Vector3d& direction2);

std::optional<Eigen::Isometry3d> obtain_global_centre_of_room(
    const std::vector<PlaneGlobalRep>& planes);

std::set<g2o::VertexSE3*> publish_room_keyframes_ids(
    const s_graphs::Rooms& room,
    const std::vector<s_graphs::VerticalPlanes>& x_vert_planes,
    const std::vector<s_graphs::VerticalPlanes>& y_vert_planes);

std::set<g2o::VertexSE3*> filter_inside_room_keyframes(
    const s_graphs::Rooms& room,
    const std::set<g2o::VertexSE3*>& keyframes_candidates);

std::vector<PlaneGlobalRep> obtain_global_planes_from_room(
    const s_graphs::Rooms& room,
    const std::vector<s_graphs::VerticalPlanes>& x_vert_planes,
    const std::vector<s_graphs::VerticalPlanes>& y_vert_planes);

template <typename PointT>
typename pcl::PointCloud<PointT>::Ptr transform_pointcloud(
    typename pcl::PointCloud<PointT>::ConstPtr _cloud,
    Eigen::Isometry3d transform) {
  typename pcl::PointCloud<PointT>::Ptr cloud(new pcl::PointCloud<PointT>());
  // at the beggining add the first pc
  cloud->points.reserve(_cloud->size());
  const Eigen::Matrix4f transform_mat = transform.matrix().cast<float>();
  for (const auto& src_pt : _cloud->points) {
    PointT dst_pt;
    dst_pt.getVector4fMap() = transform_mat * src_pt.getVector4fMap();
    dst_pt.intensity = src_pt.intensity;
    cloud->emplace_back(dst_pt);
  }
  cloud->width = cloud->size();
  cloud->height = 1;
  cloud->is_dense = false;
  // std::cout << "cloud: " << cloud << " with size" << cloud->size() << std::endl;
  return cloud;
}

// REMOVE THIS
template <typename T>
pcl::PointCloud<s_graphs::PointT>::Ptr filter_room_pointcloud(
    pcl::PointCloud<s_graphs::PointT>::Ptr cloud,
    double max_dist = 0) {
  pcl::PointCloud<s_graphs::PointT>::Ptr filtered(
      new pcl::PointCloud<s_graphs::PointT>());

  // Easy filter, remove floor points -> Although this doesn't affect the descriptor
  pcl::PassThrough<s_graphs::PointT> pass;
  pass.setInputCloud(cloud);
  pass.setFilterFieldName("z");
  pass.setFilterLimits(0.3, 3.0);
  pass.filter(*filtered);
  if (max_dist) {
    pcl::PointCloud<s_graphs::PointT>::Ptr more_filtered(
        new pcl::PointCloud<s_graphs::PointT>());
    for (auto& pnt : filtered->points) {
      Eigen::Vector3d point = pnt.getVector3fMap().cast<double>();
      point.z() = 0;
      if (point.norm() > max_dist + 0.3) {
        continue;
      }
      more_filtered->points.emplace_back(pnt);
    }

    filtered = more_filtered;
  }
  filtered->width = filtered->size();
  filtered->height = 1;

  // pass.setInputCloud(filtered);
  // pass.setFilterFieldName("x");
  // pass.setFilterLimits(-5, 5);
  // pass.filter(*filtered);

  // pass.setInputCloud(filtered);
  // pass.setFilterFieldName("y");
  // pass.setFilterLimits(-5, 5);
  // pass.filter(*filtered);
  return filtered;
}

template <class IterI>
pcl::PointCloud<s_graphs::PointT>::Ptr generate_room_pointcloud(
    const s_graphs::Rooms& room,
    const Eigen::Isometry3d& room_centre,
    IterI begin,
    IterI end) {
  pcl::PointCloud<s_graphs::PointT>::Ptr room_cloud(
      new pcl::PointCloud<s_graphs::PointT>());
  for (auto it = begin; it != end; it++) {
    s_graphs::KeyFrame::Ptr keyframe = *it;
    std::cout << "Keyframe:" << keyframe->id() << std::endl;
    Eigen::Isometry3d rel_transform = room_centre.inverse() * keyframe->estimate();
    auto cloud = transform_pointcloud<s_graphs::KeyFrame::PointT>(keyframe->cloud,
                                                                  rel_transform);
    room_cloud->points.insert(room_cloud->end(), cloud->begin(), cloud->end());
  }
  room_cloud->width = room_cloud->size();
  room_cloud->height = 1;
  return room_cloud;
}

std::optional<
    std::pair<Eigen::Isometry3d, pcl::PointCloud<s_graphs::KeyFrame::PointT>::Ptr>>
generate_room_keyframe(const s_graphs::Rooms& room,
                       const std::vector<s_graphs::VerticalPlanes>& x_vert_planes,
                       const std::vector<s_graphs::VerticalPlanes>& y_vert_planes,
                       const std::vector<s_graphs::KeyFrame::Ptr>& keyframes);

struct ExtendedRooms : public s_graphs::Rooms {
  ExtendedRooms() : s_graphs::Rooms(){};
  ExtendedRooms(s_graphs::Rooms&& rooms) : s_graphs::Rooms(rooms){};
  ExtendedRooms(s_graphs::Rooms& rooms) : s_graphs::Rooms(rooms){};
  ExtendedRooms(const s_graphs::Rooms& rooms) : s_graphs::Rooms(rooms){};
  std::vector<PlaneGlobalRep> global_planes;
  Eigen::Isometry3d centre;
  std::vector<s_graphs::KeyFrame::Ptr> keyframes;
  pcl::PointCloud<s_graphs::KeyFrame::PointT>::Ptr cloud;
};

class RoomsKeyframeGenerator : public s_graphs::Rooms {
 public:
  RoomsKeyframeGenerator(const std::vector<s_graphs::VerticalPlanes>* x_vert_planes,
                         const std::vector<s_graphs::VerticalPlanes>* y_vert_planes,
                         const std::vector<s_graphs::KeyFrame::Ptr>* keyframes)
      : x_vert_planes_(x_vert_planes),
        y_vert_planes_(y_vert_planes),
        keyframes_(keyframes){};

  void addRoom(const s_graphs::Rooms& room) {
    if (room_keyframe_dict_.count(room.id)) {
      return;
    }
    auto value =
        generate_room_keyframe(room, *x_vert_planes_, *y_vert_planes_, *keyframes_);
    if (!value.has_value()) return;
    auto [centre, cloud] = value.value();
    ExtendedRooms ext_room(room);
    ext_room.cloud = cloud;
    ext_room.centre = centre;
    ext_room.global_planes =
        obtain_global_planes_from_room(room, *x_vert_planes_, *y_vert_planes_);

    std::cout << "AQUI1" << std::endl;
    std::vector<PlaneGlobalRep> local_plane_rep;
    for (auto& plane : ext_room.global_planes) {
      PlaneGlobalRep local_plane;
      auto transform_point = ext_room.centre.inverse() * plane.point;
      local_plane.point = transform_point;
      local_plane.normal = ext_room.centre.linear().inverse() * plane.normal;
      local_plane_rep.emplace_back(local_plane);
    }

    std::cout << "AQUI2" << std::endl;
    double max_dist = 0;
    for (size_t i = 0; i < local_plane_rep.size() - 2; i++) {
      auto& plane_i_1 = local_plane_rep[i];
      auto& plane_i_2 = local_plane_rep[i + 2];
      auto intersection = find_intersection(
          plane_i_1.point, plane_i_2.normal, plane_i_2.point, plane_i_1.normal);
      if (intersection.has_value()) {
        auto intersec = intersection.value();
        intersec.z() = 0;
        auto dist = intersec.norm();
        if (dist > max_dist) {
          max_dist = dist;
        }
      }
    }
    std::cout << "max_dist:" << max_dist << std::endl;

    /************/
    room_keyframe_dict_.insert({ext_room.id, ext_room});

    std::cout << "AQUI3" << std::endl;
    auto filtered = filter_room_pointcloud<s_graphs::PointT>(cloud, max_dist);
    std::string preable(
        "/home/miguel/lux_stay_ws/ros2_ws/src/multirobot_sgraphs_server/"
        "pointcloud_data/room_keyframes/");
    pcl::io::savePCDFileASCII(
        preable + "room_keyframe_" + std::to_string(room.id) + ".pcd", *cloud);
    pcl::io::savePCDFileASCII(
        preable + "room_keyframe_" + std::to_string(room.id) + "filtered.pcd",
        *filtered);
  }

  const ExtendedRooms& getExtendedRoom(int id) { return room_keyframe_dict_[id]; };
  std::optional<ExtendedRooms> tryGetExtendedRoom(int id) {
    if (!room_keyframe_dict_.count(id)) {
      return {};
    }
    return room_keyframe_dict_[id];
  };

  std::vector<ExtendedRooms> getExtendedRooms() {
    std::vector<ExtendedRooms> out;
    out.reserve(room_keyframe_dict_.size());
    for (auto& [id, room] : room_keyframe_dict_) {
      out.emplace_back(room);
    }
    return out;
  }

 private:
  std::unordered_map<int, ExtendedRooms> room_keyframe_dict_;
  const std::vector<s_graphs::VerticalPlanes>* x_vert_planes_;
  const std::vector<s_graphs::VerticalPlanes>* y_vert_planes_;
  const std::vector<s_graphs::KeyFrame::Ptr>* keyframes_;
};

#endif
