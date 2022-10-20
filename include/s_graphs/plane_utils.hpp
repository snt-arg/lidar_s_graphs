// SPDX-License-Identifier: BSD-2-Clause

#ifndef PLANE_UTILS_HPP
#define PLANE_UTILS_HPP

#include <Eigen/Dense>

#include <s_graphs/PlanesData.h>

#include <g2o/types/slam3d/vertex_se3.h>
#include <g2o/edge_se3_plane.hpp>

#include <pcl/common/common.h>
#include <pcl/common/angles.h>
#include <pcl/common/distances.h>
namespace s_graphs {

struct plane_data_list {
  plane_data_list() : plane_centroid(0, 0, 0), connected_id(-1) {
    connected_neighbour_ids.clear();
  }
  // g2o::Plane3D plane;
  g2o::Plane3D plane_unflipped;
  int plane_id;
  int connected_id;
  std::vector<int> connected_neighbour_ids;
  pcl::PointXY start_point, end_point;
  float plane_length;
  g2o::VertexSE3* keyframe_node;
  Eigen::Vector3d plane_centroid;
  Eigen::Vector2d cluster_center;
};

struct structure_data_list {
  plane_data_list plane1;
  plane_data_list plane2;
  float width;
  float length_diff;
  float avg_point_diff;
};

class PlaneUtils {
  typedef pcl::PointXYZRGBNormal PointNormal;

public:
  PlaneUtils() {}
  ~PlaneUtils() {}

public:
  enum plane_class : uint8_t {
    X_VERT_PLANE = 0,
    Y_VERT_PLANE = 1,
    HORT_PLANE = 2,
  };

  inline float width_between_planes(Eigen::Vector4d v1, Eigen::Vector4d v2) {
    Eigen::Vector3d vec;
    float size = 0;

    if(fabs(v1(3)) > fabs(v2(3)))
      vec = fabs(v1(3)) * v1.head(3) - fabs(v2(3)) * v2.head(3);
    else if(fabs(v2(3)) > fabs(v1(3)))
      vec = fabs(v2(3)) * v2.head(3) - fabs(v1(3)) * v1.head(3);

    size = fabs(vec(0) + vec(1));

    return size;
  }

  inline float width_between_planes(s_graphs::PlaneData& plane1, s_graphs::PlaneData& plane2) {
    Eigen::Vector3d vec;
    Eigen::Vector3d plane1_eigen, plane2_eigen;
    plane1_eigen << plane1.nx, plane1.ny, plane1.nz;
    plane2_eigen << plane2.nx, plane2.ny, plane2.nz;
    float size = 0;

    if(fabs(plane1.d) > fabs(plane2.d))
      vec = fabs(plane1.d) * plane1_eigen - fabs(plane2.d) * plane2_eigen;
    else if(fabs(plane2.d) > fabs(plane1.d))
      vec = fabs(plane2.d) * plane2_eigen - fabs(plane1.d) * plane1_eigen;

    size = fabs(vec(0) + vec(1));

    return size;
  }

  void correct_plane_d(int plane_type, s_graphs::PlaneData& plane) {
    if(plane.d > 0) {
      plane.nx = -1 * plane.nx;
      plane.ny = -1 * plane.ny;
      plane.nz = -1 * plane.nz;
      plane.d = -1 * plane.d;
    }
    return;
  }

  void correct_plane_d(int plane_type, Eigen::Vector4d& plane, double px, double py) {
    if(plane(3) > 0) {
      plane(0) = -1 * plane(0);
      plane(1) = -1 * plane(1);
      plane(2) = -1 * plane(2);
      plane(3) = -1 * plane(3);
    }
    return;
  }

  float plane_length(pcl::PointCloud<PointNormal>::Ptr cloud_seg, pcl::PointXY& p1, pcl::PointXY& p2, g2o::VertexSE3* keyframe_node) {
    PointNormal pmin, pmax;
    pcl::getMaxSegment(*cloud_seg, pmin, pmax);
    p1.x = pmin.x;
    p1.y = pmin.y;
    p2.x = pmax.x;
    p2.y = pmax.y;
    float length = pcl::euclideanDistance(p1, p2);

    pcl::PointXY p1_map, p2_map;
    p1_map = convert_point_to_map(p1, keyframe_node->estimate().matrix());
    p2_map = convert_point_to_map(p2, keyframe_node->estimate().matrix());
    p1 = p1_map;
    p2 = p2_map;

    return length;
  }

  float plane_length(pcl::PointCloud<PointNormal>::Ptr cloud_seg, pcl::PointXY& p1, pcl::PointXY& p2) {
    PointNormal pmin, pmax;
    pcl::getMaxSegment(*cloud_seg, pmin, pmax);
    p1.x = pmin.x;
    p1.y = pmin.y;
    p2.x = pmax.x;
    p2.y = pmax.y;
    float length = pcl::euclideanDistance(p1, p2);

    return length;
  }

  pcl::PointXY convert_point_to_map(pcl::PointXY point_local, Eigen::Matrix4d keyframe_pose) {
    pcl::PointXY point_map;

    Eigen::Vector4d point_map_eigen, point_local_eigen;
    point_local_eigen = point_map_eigen.setZero();
    point_local_eigen(3) = point_map_eigen(3) = 1;
    point_local_eigen(0) = point_local.x;
    point_local_eigen(1) = point_local.y;
    point_map_eigen = keyframe_pose * point_local_eigen;

    point_map.x = point_map_eigen(0);
    point_map.y = point_map_eigen(1);
    return point_map;
  }

  float get_min_segment(const pcl::PointCloud<PointNormal>::Ptr& cloud_1, const pcl::PointCloud<PointNormal>::Ptr& cloud_2) {
    float min_dist = std::numeric_limits<float>::max();
    const auto token = std::numeric_limits<std::size_t>::max();
    std::size_t i_min = token, i_max = token;

    for(std::size_t i = 0; i < cloud_1->points.size(); ++i) {
      for(std::size_t j = 0; j < cloud_2->points.size(); ++j) {
        // Compute the distance
        float dist = (cloud_1->points[i].getVector4fMap() - cloud_2->points[j].getVector4fMap()).squaredNorm();
        if(dist >= min_dist) continue;

        min_dist = dist;
        i_min = i;
        i_max = j;
      }
    }

    return (std::sqrt(min_dist));
  }

  bool check_point_neighbours(const pcl::PointCloud<PointNormal>::Ptr& cloud_1, const pcl::PointCloud<PointNormal>::Ptr& cloud_2) {
    bool valid_neighbour = false;
    int point_count = 0;
    float min_dist = 0.5;

    for(std::size_t i = 0; i < cloud_1->points.size(); ++i) {
      for(std::size_t j = 0; j < cloud_2->points.size(); ++j) {
        // Compute the distance
        float dist = (cloud_1->points[i].getVector4fMap() - cloud_2->points[j].getVector4fMap()).squaredNorm();
        if(dist < min_dist) {
          point_count++;
          break;
        }
      }
      if(point_count > 100) {
        valid_neighbour = true;
        break;
      }
    }
    return valid_neighbour;
  }

  bool compute_point_difference(const double plane1_point, const double plane2_point) {
    if((plane1_point - plane2_point) > 0) return false;

    return true;
  }

  float plane_dot_product(const s_graphs::PlaneData& plane1, const s_graphs::PlaneData& plane2) {
    float dot_product = plane1.nx * plane2.nx + plane1.ny * plane2.ny + plane1.nz * plane2.nz;
    return dot_product;
  }
};
}  // namespace s_graphs
#endif  // PLANE_UTILS_HPP