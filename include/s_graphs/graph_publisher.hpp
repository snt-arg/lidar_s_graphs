/*
Copyright (c) 2023, University of Luxembourg
All rights reserved.

Redistributions and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

3. Neither the name of the copyright holder nor the names of its
   contributors may be used to endorse or promote products derived from
   this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS 'AS IS'
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
*/

// SPDX-License-Identifier: BSD-2-Clause

#ifndef GRAPH_PUBLISHER_HPP
#define GRAPH_PUBLISHER_HPP

#include <g2o/types/slam3d/edge_se3.h>
#include <g2o/types/slam3d/vertex_se3.h>
#include <g2o/types/slam3d_addons/vertex_plane.h>
#include <math.h>
#include <pcl/common/common.h>
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
#include <s_graphs/graph_publisher.hpp>
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
#include "graph_manager_msgs/msg/node.hpp"
#include "pcl_ros/transforms.hpp"
#include "rclcpp/rclcpp.hpp"
#include "s_graphs/msg/plane_data.hpp"
#include "s_graphs/msg/planes_data.hpp"
#include "s_graphs/msg/room_data.hpp"
#include "s_graphs/msg/rooms_data.hpp"
#include "sensor_msgs/msg/point_cloud2.hpp"
#include "visualization_msgs/msg/marker_array.hpp"
class GraphPublisher {
 public:
  GraphPublisher();
  ~GraphPublisher();

 public:
  graph_manager_msgs::msg::Graph publish_graph(
      const g2o::SparseOptimizer* local_graph,
      std::string graph_type,
      const std::vector<s_graphs::VerticalPlanes>& x_vert_planes_prior,
      const std::vector<s_graphs::VerticalPlanes>& y_vert_planes_prior,
      const std::vector<s_graphs::Rooms>& rooms_vec_prior,
      const std::vector<s_graphs::VerticalPlanes>& x_vert_planes,
      const std::vector<s_graphs::VerticalPlanes>& y_vert_planes,
      const std::vector<s_graphs::Rooms>& rooms_vec,
      const std::vector<s_graphs::InfiniteRooms>& x_infinite_rooms,
      const std::vector<s_graphs::InfiniteRooms>& y_infinite_rooms);

 private:
};

#endif  // GRAPH_PUBLISHER_HPP