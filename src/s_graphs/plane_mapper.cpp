// SPDX-License-Identifier: BSD-2-Clause

#include <s_graphs/plane_mapper.hpp>

namespace s_graphs {

PlaneMapper::PlaneMapper(const ros::NodeHandle& private_nh) {
  use_point_to_plane = private_nh.param<bool>("use_point_to_plane", false);
  plane_information = private_nh.param<double>("plane_information", 0.01);
  plane_dist_threshold = private_nh.param<double>("plane_dist_threshold", 0.15);
  plane_points_dist = private_nh.param<double>("plane_points_dist", 0.5);
  corridor_min_plane_length = private_nh.param<double>("corridor_min_plane_length", 10);
  room_min_plane_length = private_nh.param<double>("room_min_plane_length", 3.0);
  room_max_plane_length = private_nh.param<double>("room_max_plane_length", 6.0);
  min_plane_points = private_nh.param<double>("min_plane_points", 100);
  use_corridor_constraint = private_nh.param<bool>("use_corridor_constraint", false);
  use_room_constraint = private_nh.param<bool>("use_room_constraint", false);
}

PlaneMapper::~PlaneMapper() {}

void PlaneMapper::map_extracted_planes(std::unique_ptr<GraphSLAM>& graph_slam, KeyFrame::Ptr keyframe, const std::vector<sensor_msgs::PointCloud2>& extracted_cloud_vec, std::vector<VerticalPlanes>& x_vert_planes, std::vector<VerticalPlanes>& y_vert_planes, std::vector<HorizontalPlanes>& hort_planes) {
  std::vector<plane_data_list> x_det_corridor_candidates, y_det_corridor_candidates;
  std::vector<plane_data_list> x_det_room_candidates, y_det_room_candidates;

  for(const auto& cloud_seg_msg : extracted_cloud_vec) {
    pcl::PointCloud<PointNormal>::Ptr cloud_seg_body(new pcl::PointCloud<PointNormal>());
    pcl::fromROSMsg(cloud_seg_msg, *cloud_seg_body);

    if(cloud_seg_body->points.size() < min_plane_points) continue;
    keyframe->cloud_seg_body = cloud_seg_body;

    g2o::Plane3D det_plane_body_frame = Eigen::Vector4d(cloud_seg_body->back().normal_x, cloud_seg_body->back().normal_y, cloud_seg_body->back().normal_z, cloud_seg_body->back().curvature);
    int plane_type = add_planes_to_graph(graph_slam, keyframe, det_plane_body_frame, x_vert_planes, y_vert_planes, hort_planes);
  }
}

/**
 * @brief detected plane mapping
 *
 */
int PlaneMapper::add_planes_to_graph(std::unique_ptr<GraphSLAM>& graph_slam, KeyFrame::Ptr& keyframe, const g2o::Plane3D& det_plane_body_frame, std::vector<VerticalPlanes>& x_vert_planes, std::vector<VerticalPlanes>& y_vert_planes, std::vector<HorizontalPlanes>& hort_planes) {
  int plane_id;
  int plane_type = -1;

  g2o::Plane3D det_plane_map_frame = convert_plane_to_map_frame(keyframe, det_plane_body_frame);

  /* Get the plane type based on the largest value of the normal orientation x,y and z */
  if(fabs(det_plane_map_frame.coeffs()(0)) > fabs(det_plane_map_frame.coeffs()(1)) && fabs(det_plane_map_frame.coeffs()(0)) > fabs(det_plane_map_frame.coeffs()(2)))
    plane_type = PlaneUtils::plane_class::X_VERT_PLANE;
  else if(fabs(det_plane_map_frame.coeffs()(1)) > fabs(det_plane_map_frame.coeffs()(0)) && fabs(det_plane_map_frame.coeffs()(1)) > fabs(det_plane_map_frame.coeffs()(2)))
    plane_type = PlaneUtils::plane_class::Y_VERT_PLANE;
  else if(fabs(det_plane_map_frame.coeffs()(2)) > fabs(det_plane_map_frame.coeffs()(0)) && fabs(det_plane_map_frame.coeffs()(2)) > fabs(det_plane_map_frame.coeffs()(1)))
    plane_type = PlaneUtils::plane_class::HORT_PLANE;

  plane_id = sort_planes(graph_slam, plane_type, keyframe, det_plane_map_frame, det_plane_body_frame, x_vert_planes, y_vert_planes, hort_planes);

  return plane_type;
}

/**
 * @brief convert body plane coefficients to map frame
 */
g2o::Plane3D PlaneMapper::convert_plane_to_map_frame(const KeyFrame::Ptr& keyframe, const g2o::Plane3D& det_plane_body_frame) {
  g2o::Plane3D det_plane_map_frame;
  Eigen::Vector4d map_coeffs;

  Eigen::Isometry3d w2n = keyframe->node->estimate();
  map_coeffs.head<3>() = w2n.rotation() * det_plane_body_frame.coeffs().head<3>();
  map_coeffs(3) = det_plane_body_frame.coeffs()(3) - w2n.translation().dot(map_coeffs.head<3>());
  det_plane_map_frame = map_coeffs;

  return det_plane_map_frame;
}

/**
 * @brief sort and factor the detected planes
 *
 */
int PlaneMapper::sort_planes(std::unique_ptr<GraphSLAM>& graph_slam, const int& plane_type, KeyFrame::Ptr& keyframe, const g2o::Plane3D& det_plane_map_frame, const g2o::Plane3D& det_plane_body_frame, std::vector<VerticalPlanes>& x_vert_planes, std::vector<VerticalPlanes>& y_vert_planes, std::vector<HorizontalPlanes>& hort_planes) {
  int plane_id = factor_planes(graph_slam, plane_type, keyframe, det_plane_map_frame, det_plane_body_frame, x_vert_planes, y_vert_planes, hort_planes);

  return plane_id;
}

/**
 * @brief create vertical plane factors
 */
int PlaneMapper::factor_planes(std::unique_ptr<GraphSLAM>& graph_slam, const int& plane_type, KeyFrame::Ptr& keyframe, const g2o::Plane3D& det_plane_map_frame, const g2o::Plane3D& det_plane_body_frame, std::vector<VerticalPlanes>& x_vert_planes, std::vector<VerticalPlanes>& y_vert_planes, std::vector<HorizontalPlanes>& hort_planes) {
  g2o::VertexPlane* plane_node;
  int plane_id = -1;

  Eigen::Matrix4d Gij;
  Gij.setZero();
  if(use_point_to_plane) {
    auto it = keyframe->cloud_seg_body->points.begin();
    while(it != keyframe->cloud_seg_body->points.end()) {
      PointNormal point_tmp;
      point_tmp = *it;
      Eigen::Vector4d point(point_tmp.x, point_tmp.y, point_tmp.z, 1);
      double point_to_plane_d = det_plane_map_frame.coeffs().transpose() * keyframe->node->estimate().matrix() * point;

      if(abs(point_to_plane_d) < 0.1) {
        Gij += point * point.transpose();
        ++it;
      } else {
        it = keyframe->cloud_seg_body->points.erase(it);
      }
    }
  }

  std::pair<int, int> data_association;
  data_association.first = -1;
  data_association = associate_plane(plane_type, keyframe, det_plane_body_frame.coeffs(), keyframe->cloud_seg_body, x_vert_planes, y_vert_planes, hort_planes);

  switch(plane_type) {
    case PlaneUtils::plane_class::X_VERT_PLANE: {
      if(x_vert_planes.empty() || data_association.first == -1) {
        data_association.first = graph_slam->num_vertices_local();
        plane_node = graph_slam->add_plane_node(det_plane_map_frame.coeffs());
        VerticalPlanes vert_plane;
        vert_plane.id = data_association.first;
        vert_plane.plane = det_plane_map_frame.coeffs();
        vert_plane.cloud_seg_body = keyframe->cloud_seg_body;
        vert_plane.cloud_seg_body_vec.push_back(keyframe->cloud_seg_body);
        vert_plane.keyframe_node_vec.push_back(keyframe->node);
        vert_plane.keyframe_node = keyframe->node;
        vert_plane.plane_node = plane_node;
        vert_plane.cloud_seg_map = nullptr;
        vert_plane.covariance = Eigen::Matrix3d::Identity();
        std::vector<double> color;
        color.push_back(keyframe->cloud_seg_body->points.back().r);  // red
        color.push_back(keyframe->cloud_seg_body->points.back().g);  // green
        color.push_back(keyframe->cloud_seg_body->points.back().b);  // blue
        // color.push_back(255);  // red
        // color.push_back(0.0);  // green
        // color.push_back(0.0);  // blue
        vert_plane.color = color;
        x_vert_planes.push_back(vert_plane);
        keyframe->x_plane_ids.push_back(vert_plane.id);

        ROS_DEBUG_NAMED("xplane association", "Added new x vertical plane node with coeffs %f %f %f %f", det_plane_map_frame.coeffs()(0), det_plane_map_frame.coeffs()(1), det_plane_map_frame.coeffs()(2), det_plane_map_frame.coeffs()(3));
      } else {
        plane_node = x_vert_planes[data_association.second].plane_node;
        x_vert_planes[data_association.second].cloud_seg_body_vec.push_back(keyframe->cloud_seg_body);
        x_vert_planes[data_association.second].keyframe_node_vec.push_back(keyframe->node);
        keyframe->x_plane_ids.push_back(x_vert_planes[data_association.second].id);

        ROS_DEBUG_NAMED("xplane association", "matched x vert plane with coeffs %f %f %f %f to mapped x vert plane of coeffs %f %f %f %f", det_plane_map_frame.coeffs()(0), det_plane_map_frame.coeffs()(1), det_plane_map_frame.coeffs()(2),
                        det_plane_map_frame.coeffs()(3), x_vert_planes[data_association.second].plane_node->estimate().coeffs()(0), x_vert_planes[data_association.second].plane_node->estimate().coeffs()(1),
                        x_vert_planes[data_association.second].plane_node->estimate().coeffs()(2), x_vert_planes[data_association.second].plane_node->estimate().coeffs()(3));
        std::cout << "xplane association : matched x vert plane with coeffs " << det_plane_map_frame.coeffs()(0) << " " << det_plane_map_frame.coeffs()(1) << " " << det_plane_map_frame.coeffs()(2) << " " << det_plane_map_frame.coeffs()(3) << "  to mapped x vert plane of coeffs "
                  << x_vert_planes[data_association.second].plane_node->estimate().coeffs()(0) << " " << x_vert_planes[data_association.second].plane_node->estimate().coeffs()(1) << " " << x_vert_planes[data_association.second].plane_node->estimate().coeffs()(2) << " "
                  << x_vert_planes[data_association.second].plane_node->estimate().coeffs()(3) << " and Id. : " << x_vert_planes[data_association.second].id << std::endl;
      }
      break;
    }
    case PlaneUtils::plane_class::Y_VERT_PLANE: {
      if(y_vert_planes.empty() || data_association.first == -1) {
        data_association.first = graph_slam->num_vertices_local();
        plane_node = graph_slam->add_plane_node(det_plane_map_frame.coeffs());
        VerticalPlanes vert_plane;
        vert_plane.id = data_association.first;
        vert_plane.plane = det_plane_map_frame.coeffs();
        vert_plane.cloud_seg_body = keyframe->cloud_seg_body;
        vert_plane.cloud_seg_body_vec.push_back(keyframe->cloud_seg_body);
        vert_plane.keyframe_node_vec.push_back(keyframe->node);
        vert_plane.keyframe_node = keyframe->node;
        vert_plane.plane_node = plane_node;
        vert_plane.cloud_seg_map = nullptr;
        vert_plane.covariance = Eigen::Matrix3d::Identity();
        std::vector<double> color;
        color.push_back(keyframe->cloud_seg_body->points.back().r);  // red
        color.push_back(keyframe->cloud_seg_body->points.back().g);  // green
        color.push_back(keyframe->cloud_seg_body->points.back().b);  // blue
        // color.push_back(0.0);  // red
        // color.push_back(0.0);  // green
        // color.push_back(255);  // blue
        vert_plane.color = color;
        y_vert_planes.push_back(vert_plane);
        keyframe->y_plane_ids.push_back(vert_plane.id);
        ROS_DEBUG_NAMED("yplane association", "Added new y vertical plane node with coeffs %f %f %f %f", det_plane_map_frame.coeffs()(0), det_plane_map_frame.coeffs()(1), det_plane_map_frame.coeffs()(2), det_plane_map_frame.coeffs()(3));
      } else {
        plane_node = y_vert_planes[data_association.second].plane_node;
        y_vert_planes[data_association.second].cloud_seg_body_vec.push_back(keyframe->cloud_seg_body);
        y_vert_planes[data_association.second].keyframe_node_vec.push_back(keyframe->node);
        keyframe->y_plane_ids.push_back(y_vert_planes[data_association.second].id);

        ROS_DEBUG_NAMED("yplane association", "matched y vert plane with coeffs %f %f %f %f to mapped y vert plane of coeffs %f %f %f %f", det_plane_map_frame.coeffs()(0), det_plane_map_frame.coeffs()(1), det_plane_map_frame.coeffs()(2),
                        det_plane_map_frame.coeffs()(3), y_vert_planes[data_association.second].plane_node->estimate().coeffs()(0), y_vert_planes[data_association.second].plane_node->estimate().coeffs()(1),
                        y_vert_planes[data_association.second].plane_node->estimate().coeffs()(2), y_vert_planes[data_association.second].plane_node->estimate().coeffs()(3));
        std::cout << "yplane association : matched y vert plane with coeffs " << det_plane_map_frame.coeffs()(0) << " " << det_plane_map_frame.coeffs()(1) << " " << det_plane_map_frame.coeffs()(2) << " " << det_plane_map_frame.coeffs()(3) << "  to mapped y vert plane of coeffs "
                  << y_vert_planes[data_association.second].plane_node->estimate().coeffs()(0) << " " << y_vert_planes[data_association.second].plane_node->estimate().coeffs()(1) << " " << y_vert_planes[data_association.second].plane_node->estimate().coeffs()(2) << " "
                  << y_vert_planes[data_association.second].plane_node->estimate().coeffs()(3) << " and Id. : " << y_vert_planes[data_association.second].id << std::endl;
      }
      break;
    }
    case PlaneUtils::plane_class::HORT_PLANE: {
      if(hort_planes.empty() || data_association.first == -1) {
        data_association.first = graph_slam->num_vertices_local();
        plane_node = graph_slam->add_plane_node(det_plane_map_frame.coeffs());
        HorizontalPlanes hort_plane;
        hort_plane.id = data_association.first;
        hort_plane.plane = det_plane_map_frame.coeffs();
        hort_plane.cloud_seg_body = keyframe->cloud_seg_body;
        hort_plane.cloud_seg_body_vec.push_back(keyframe->cloud_seg_body);
        hort_plane.keyframe_node_vec.push_back(keyframe->node);
        hort_plane.keyframe_node = keyframe->node;
        hort_plane.plane_node = plane_node;
        hort_plane.cloud_seg_map = nullptr;
        hort_plane.covariance = Eigen::Matrix3d::Identity();
        std::vector<double> color;
        color.push_back(255);  // red
        color.push_back(0.0);  // green
        color.push_back(100);  // blue
        hort_plane.color = color;
        hort_planes.push_back(hort_plane);
        keyframe->hort_plane_ids.push_back(hort_plane.id);

        ROS_DEBUG_NAMED("hort plane association", "Added new horizontal plane node with coeffs %f %f %f %f", det_plane_map_frame.coeffs()(0), det_plane_map_frame.coeffs()(1), det_plane_map_frame.coeffs()(2), det_plane_map_frame.coeffs()(3));
      } else {
        plane_node = hort_planes[data_association.second].plane_node;
        hort_planes[data_association.second].cloud_seg_body_vec.push_back(keyframe->cloud_seg_body);
        hort_planes[data_association.second].keyframe_node_vec.push_back(keyframe->node);
        keyframe->hort_plane_ids.push_back(hort_planes[data_association.second].id);

        ROS_DEBUG_NAMED("hort plane association", "matched hort plane with coeffs %f %f %f %f to mapped hort plane of coeffs %f %f %f %f", det_plane_map_frame.coeffs()(0), det_plane_map_frame.coeffs()(1), det_plane_map_frame.coeffs()(2),
                        det_plane_map_frame.coeffs()(3), hort_planes[data_association.second].plane_node->estimate().coeffs()(0), hort_planes[data_association.second].plane_node->estimate().coeffs()(1),
                        hort_planes[data_association.second].plane_node->estimate().coeffs()(2), hort_planes[data_association.second].plane_node->estimate().coeffs()(3));
      }
      break;
    }
    default:
      std::cout << "factoring planes function had a weird error " << std::endl;
      break;
  }

  if(use_point_to_plane) {
    Eigen::Matrix<double, 1, 1> information(0.001);
    auto edge = graph_slam->add_se3_point_to_plane_edge(keyframe->node, plane_node, Gij, information);
    graph_slam->add_robust_kernel(edge, "Huber", 1.0);
  } else {
    Eigen::Matrix3d information = Eigen::Matrix3d::Identity() * plane_information;
    auto edge = graph_slam->add_se3_plane_edge(keyframe->node, plane_node, det_plane_body_frame.coeffs(), information);
    graph_slam->add_robust_kernel(edge, "Huber", 1.0);
  }

  convert_plane_points_to_map(x_vert_planes, y_vert_planes, hort_planes);

  return data_association.first;
}

/**
 * @brief data assoction betweeen the planes
 */
std::pair<int, int> PlaneMapper::associate_plane(const int& plane_type, const KeyFrame::Ptr& keyframe, const g2o::Plane3D& det_plane, const pcl::PointCloud<PointNormal>::Ptr& cloud_seg_body, const std::vector<VerticalPlanes>& x_vert_planes, const std::vector<VerticalPlanes>& y_vert_planes, const std::vector<HorizontalPlanes>& hort_planes) {
  std::pair<int, int> data_association;
  double vert_min_maha_dist = 100;
  double hort_min_maha_dist = 100;
  Eigen::Isometry3d m2n = keyframe->estimate().inverse();

  switch(plane_type) {
    case PlaneUtils::plane_class::X_VERT_PLANE: {
      for(int i = 0; i < x_vert_planes.size(); ++i) {
        g2o::Plane3D local_plane = m2n * x_vert_planes[i].plane;
        Eigen::Vector3d error = local_plane.ominus(det_plane);
        double maha_dist = sqrt(error.transpose() * x_vert_planes[i].covariance.inverse() * error);
        ROS_DEBUG_NAMED("xplane plane association", "maha distance xplane: %f", maha_dist);

        // printf("\n maha distance x: %f", maha_dist);

        if(std::isnan(maha_dist) || maha_dist < 1e-3) {
          Eigen::Matrix3d cov = Eigen::Matrix3d::Identity();
          maha_dist = sqrt(error.transpose() * cov * error);
        }
        if(maha_dist < vert_min_maha_dist) {
          vert_min_maha_dist = maha_dist;
          data_association.first = x_vert_planes[i].id;
          data_association.second = i;
        }
      }
      if(vert_min_maha_dist < plane_dist_threshold && !x_vert_planes[data_association.second].cloud_seg_map->empty()) {
        float min_segment = std::numeric_limits<float>::max();
        pcl::PointCloud<PointNormal>::Ptr cloud_seg_detected(new pcl::PointCloud<PointNormal>());
        Eigen::Matrix4f current_keyframe_pose = keyframe->estimate().matrix().cast<float>();
        for(size_t j = 0; j < cloud_seg_body->points.size(); ++j) {
          PointNormal dst_pt;
          dst_pt.getVector4fMap() = current_keyframe_pose * cloud_seg_body->points[j].getVector4fMap();
          cloud_seg_detected->points.push_back(dst_pt);
        }
        bool valid_neighbour = plane_utils->check_point_neighbours(x_vert_planes[data_association.second].cloud_seg_map, cloud_seg_detected);

        if(!valid_neighbour) {
          data_association.first = -1;
        }
      }
      break;
    }
    case PlaneUtils::plane_class::Y_VERT_PLANE: {
      for(int i = 0; i < y_vert_planes.size(); ++i) {
        float dist = fabs(det_plane.coeffs()(3) - y_vert_planes[i].plane.coeffs()(3));
        g2o::Plane3D local_plane = m2n * y_vert_planes[i].plane;
        Eigen::Vector3d error = local_plane.ominus(det_plane);
        double maha_dist = sqrt(error.transpose() * y_vert_planes[i].covariance.inverse() * error);
        ROS_DEBUG_NAMED("yplane plane association", "maha distance yplane: %f", maha_dist);
        // printf("\n maha distance y: %f", maha_dist);

        if(std::isnan(maha_dist) || maha_dist < 1e-3) {
          Eigen::Matrix3d cov = Eigen::Matrix3d::Identity();
          maha_dist = sqrt(error.transpose() * cov * error);
        }
        if(maha_dist < vert_min_maha_dist) {
          vert_min_maha_dist = maha_dist;
          data_association.first = y_vert_planes[i].id;
          data_association.second = i;
        }
      }
      if(vert_min_maha_dist < plane_dist_threshold && !y_vert_planes[data_association.second].cloud_seg_map->empty()) {
        float min_segment = std::numeric_limits<float>::max();
        pcl::PointCloud<PointNormal>::Ptr cloud_seg_detected(new pcl::PointCloud<PointNormal>());
        Eigen::Matrix4f current_keyframe_pose = keyframe->estimate().matrix().cast<float>();
        for(size_t j = 0; j < cloud_seg_body->points.size(); ++j) {
          PointNormal dst_pt;
          dst_pt.getVector4fMap() = current_keyframe_pose * cloud_seg_body->points[j].getVector4fMap();
          cloud_seg_detected->points.push_back(dst_pt);
        }
        bool valid_neighbour = plane_utils->check_point_neighbours(y_vert_planes[data_association.second].cloud_seg_map, cloud_seg_detected);

        if(!valid_neighbour) {
          data_association.first = -1;
        }
      }
      break;
    }
    case PlaneUtils::plane_class::HORT_PLANE: {
      for(int i = 0; i < hort_planes.size(); ++i) {
        g2o::Plane3D local_plane = m2n * hort_planes[i].plane;
        Eigen::Vector3d error = local_plane.ominus(det_plane);
        double maha_dist = sqrt(error.transpose() * hort_planes[i].covariance.inverse() * error);
        // std::cout << "cov hor: " << hort_planes[i].covariance.inverse() << std::endl;
        ROS_DEBUG_NAMED("hort plane association", "maha distance hort: %f", maha_dist);

        if(std::isnan(maha_dist) || maha_dist < 1e-3) {
          Eigen::Matrix3d cov = Eigen::Matrix3d::Identity();
          maha_dist = sqrt(error.transpose() * cov * error);
        }
        if(maha_dist < hort_min_maha_dist) {
          vert_min_maha_dist = maha_dist;
          data_association.first = hort_planes[i].id;
          data_association.second = i;
        }
      }

      if(hort_min_maha_dist > plane_dist_threshold) data_association.first = -1;

      break;
    }
    default:
      std::cout << "associating planes had an error " << std::endl;
      break;
  }

  // printf("\n vert min maha dist: %f", vert_min_maha_dist);
  // printf("\n hort min maha dist: %f", hort_min_maha_dist);

  return data_association;
}

/**
 * @brief convert the body points of planes to map frame for mapping
 */
void PlaneMapper::convert_plane_points_to_map(std::vector<VerticalPlanes>& x_vert_planes, std::vector<VerticalPlanes>& y_vert_planes, std::vector<HorizontalPlanes>& hort_planes) {
  for(int i = 0; i < x_vert_planes.size(); ++i) {
    pcl::PointCloud<PointNormal>::Ptr cloud_seg_map(new pcl::PointCloud<PointNormal>());

    for(int k = 0; k < x_vert_planes[i].keyframe_node_vec.size(); ++k) {
      Eigen::Matrix4f pose = x_vert_planes[i].keyframe_node_vec[k]->estimate().matrix().cast<float>();
      for(size_t j = 0; j < x_vert_planes[i].cloud_seg_body_vec[k]->points.size(); ++j) {
        PointNormal dst_pt;
        dst_pt.getVector4fMap() = pose * x_vert_planes[i].cloud_seg_body_vec[k]->points[j].getVector4fMap();
        cloud_seg_map->points.push_back(dst_pt);
      }
    }
    x_vert_planes[i].cloud_seg_map = cloud_seg_map;
  }

  for(int i = 0; i < y_vert_planes.size(); ++i) {
    pcl::PointCloud<PointNormal>::Ptr cloud_seg_map(new pcl::PointCloud<PointNormal>());

    for(int k = 0; k < y_vert_planes[i].keyframe_node_vec.size(); ++k) {
      Eigen::Matrix4f pose = y_vert_planes[i].keyframe_node_vec[k]->estimate().matrix().cast<float>();

      for(size_t j = 0; j < y_vert_planes[i].cloud_seg_body_vec[k]->points.size(); ++j) {
        PointNormal dst_pt;
        dst_pt.getVector4fMap() = pose * y_vert_planes[i].cloud_seg_body_vec[k]->points[j].getVector4fMap();
        cloud_seg_map->points.push_back(dst_pt);
      }
    }
    y_vert_planes[i].cloud_seg_map = cloud_seg_map;
  }

  for(int i = 0; i < hort_planes.size(); ++i) {
    pcl::PointCloud<PointNormal>::Ptr cloud_seg_map(new pcl::PointCloud<PointNormal>());

    for(int k = 0; k < hort_planes[i].keyframe_node_vec.size(); ++k) {
      Eigen::Matrix4f pose = hort_planes[i].keyframe_node_vec[k]->estimate().matrix().cast<float>();

      for(size_t j = 0; j < hort_planes[i].cloud_seg_body_vec[k]->points.size(); ++j) {
        PointNormal dst_pt;
        dst_pt.getVector4fMap() = pose * hort_planes[i].cloud_seg_body_vec[k]->points[j].getVector4fMap();
        cloud_seg_map->points.push_back(dst_pt);
      }
    }
    hort_planes[i].cloud_seg_map = cloud_seg_map;
  }
}

}  // namespace s_graphs