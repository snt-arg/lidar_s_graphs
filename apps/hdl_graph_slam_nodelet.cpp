// SPDX-License-Identifier: BSD-2-Clause

#include <ctime>
#include <mutex>
#include <atomic>
#include <memory>
#include <iomanip>
#include <iostream>
#include <unordered_map>
#include <boost/format.hpp>
#include <boost/thread.hpp>
#include <boost/filesystem.hpp>
#include <boost/algorithm/string.hpp>
#include <Eigen/Dense>
#include <pcl/io/pcd_io.h>
#include <pcl/common/distances.h>

#include <ros/ros.h>
#include <geodesy/utm.h>
#include <geodesy/wgs84.h>
#include <pcl_ros/point_cloud.h>
#include <pcl_ros/transforms.h>
#include <message_filters/subscriber.h>
#include <message_filters/time_synchronizer.h>
#include <message_filters/sync_policies/approximate_time.h>
#include <tf_conversions/tf_eigen.h>
#include <tf/transform_listener.h>

#include <std_msgs/Time.h>
#include <nav_msgs/Odometry.h>
#include <nmea_msgs/Sentence.h>
#include <sensor_msgs/Imu.h>
#include <sensor_msgs/NavSatFix.h>
#include <sensor_msgs/PointCloud2.h>
#include <geographic_msgs/GeoPointStamped.h>
#include <visualization_msgs/MarkerArray.h>
#include <hdl_graph_slam/FloorCoeffs.h>
#include <geometry_msgs/PoseStamped.h>

#include <hdl_graph_slam/SaveMap.h>
#include <hdl_graph_slam/DumpGraph.h>

#include <nodelet/nodelet.h>
#include <pluginlib/class_list_macros.h>

#include <hdl_graph_slam/ros_utils.hpp>
#include <hdl_graph_slam/ros_time_hash.hpp>
#include <hdl_graph_slam/PointClouds.h>


#include <hdl_graph_slam/graph_slam.hpp>
#include <hdl_graph_slam/keyframe.hpp>
#include <hdl_graph_slam/planes.hpp>
#include <hdl_graph_slam/corridors.hpp>
#include <hdl_graph_slam/rooms.hpp>
#include <hdl_graph_slam/keyframe_updater.hpp>
#include <hdl_graph_slam/loop_detector.hpp>
#include <hdl_graph_slam/information_matrix_calculator.hpp>
#include <hdl_graph_slam/map_cloud_generator.hpp>
#include <hdl_graph_slam/nmea_sentence_parser.hpp>

#include <g2o/vertex_room.hpp>
#include <g2o/vertex_corridor.hpp>
#include <g2o/types/slam3d/edge_se3.h>
#include <g2o/types/slam3d/vertex_se3.h>
#include <g2o/edge_se3_plane.hpp>
#include <g2o/edge_se3_priorxy.hpp>
#include <g2o/edge_se3_priorxyz.hpp>
#include <g2o/edge_se3_priorvec.hpp>
#include <g2o/edge_se3_priorquat.hpp>
#include <g2o/types/slam3d_addons/vertex_plane.h>
#include <g2o/edge_se3_point_to_plane.hpp>
#include <g2o/edge_plane_parallel.hpp>
#include <g2o/edge_corridor_plane.hpp>
#include <g2o/edge_room.hpp>

namespace hdl_graph_slam {

class HdlGraphSlamNodelet : public nodelet::Nodelet {
public:
  typedef pcl::PointXYZI PointT;
  typedef pcl::PointXYZRGBNormal PointNormal;
  typedef message_filters::sync_policies::ApproximateTime<nav_msgs::Odometry, sensor_msgs::PointCloud2> ApproxSyncPolicy;
  
  struct plane_data_list { 
    g2o::Plane3D plane_local;
    g2o::Plane3D plane;
    int plane_id;
    float plane_length;
    g2o::VertexSE3* keyframe_node;
    Eigen::Vector3d keyframe_trans;
    bool is_structural_candidate;
  };
  struct structure_data_list {
    plane_data_list plane1;
    plane_data_list plane2;
    float width;
    float length_diff;
  };

  HdlGraphSlamNodelet() {}
  virtual ~HdlGraphSlamNodelet() {}

  virtual void onInit() {
    nh = getNodeHandle();
    mt_nh = getMTNodeHandle();
    private_nh = getPrivateNodeHandle();

    // init parameters
    map_frame_id = private_nh.param<std::string>("map_frame_id", "map");
    odom_frame_id = private_nh.param<std::string>("odom_frame_id", "odom");
    map_cloud_resolution = private_nh.param<double>("map_cloud_resolution", 0.05);
    wait_trans_odom2map = private_nh.param<bool>("wait_trans_odom2map", false);
    got_trans_odom2map = false;
    trans_odom2map.setIdentity();

    max_keyframes_per_update = private_nh.param<int>("max_keyframes_per_update", 10);

    //
    anchor_node = nullptr;
    anchor_edge = nullptr;
    floor_plane_node = nullptr;
    graph_slam.reset(new GraphSLAM(private_nh.param<std::string>("g2o_solver_type", "lm_var")));
    keyframe_updater.reset(new KeyframeUpdater(private_nh));
    loop_detector.reset(new LoopDetector(private_nh));
    map_cloud_generator.reset(new MapCloudGenerator());
    inf_calclator.reset(new InformationMatrixCalculator(private_nh));
    nmea_parser.reset(new NmeaSentenceParser());

    gps_time_offset = private_nh.param<double>("gps_time_offset", 0.0);
    gps_edge_stddev_xy = private_nh.param<double>("gps_edge_stddev_xy", 10000.0);
    gps_edge_stddev_z = private_nh.param<double>("gps_edge_stddev_z", 10.0);
    floor_edge_stddev = private_nh.param<double>("floor_edge_stddev", 10.0);

    imu_time_offset = private_nh.param<double>("imu_time_offset", 0.0);
    enable_imu_orientation = private_nh.param<bool>("enable_imu_orientation", false);
    enable_imu_acceleration = private_nh.param<bool>("enable_imu_acceleration", false);
    imu_orientation_edge_stddev = private_nh.param<double>("imu_orientation_edge_stddev", 0.1);
    imu_acceleration_edge_stddev = private_nh.param<double>("imu_acceleration_edge_stddev", 3.0);
    
    plane_dist_threshold = private_nh.param<double>("plane_dist_threshold", 0.15);
    use_point_to_plane = private_nh.param<bool>("plane_dist_threshold", false);
    use_parallel_plane_constraint = private_nh.param<bool>("use_parallel_plane_constraint", true);
    use_perpendicular_plane_constraint = private_nh.param<bool>("use_perpendicular_plane_constraint", true);

    use_corridor_constraint = private_nh.param<bool>("use_corridor_constraint", false); 
    corridor_dist_threshold = private_nh.param<double>("corridor_dist_threshold", 1.0);
    corridor_min_plane_length = private_nh.param<double>("corridor_min_plane_length", 10);
    corridor_min_width  = private_nh.param<double>("corridor_min_width", 1.5);
    corridor_max_width  = private_nh.param<double>("corridor_max_width", 2.5);
    corridor_plane_length_diff_threshold = private_nh.param<double>("corridor_plane_length_diff_threshold", 0.3);
      
    use_room_constraint = private_nh.param<bool>("use_room_constraint", false); 
    room_plane_length_diff_threshold = private_nh.param<double>("room_plane_length_diff_threshold", 0.3);
    room_dist_threshold = private_nh.param<double>("room_dist_threshold", 1.0);
    room_min_plane_length = private_nh.param<double>("room_min_plane_length", 3.0);
    room_max_plane_length = private_nh.param<double>("room_max_plane_length", 6.0);
    room_min_width = private_nh.param<double>("room_min_width", 2.5);

    points_topic = private_nh.param<std::string>("points_topic", "/velodyne_points");

    init_odom2map_sub = nh.subscribe("/odom2map/initial_pose", 1, &HdlGraphSlamNodelet::init_map2odom_pose_callback, this);
    while(wait_trans_odom2map && !got_trans_odom2map) {
      ROS_WARN("Waiting for the Initial Transform between odom and map frame");
      ros::spinOnce();
      usleep(1e6);
    }
    
    // subscribers
    odom_sub.reset(new message_filters::Subscriber<nav_msgs::Odometry>(mt_nh, "/odom", 256));
    cloud_sub.reset(new message_filters::Subscriber<sensor_msgs::PointCloud2>(mt_nh, "/filtered_points", 32));
    sync.reset(new message_filters::Synchronizer<ApproxSyncPolicy>(ApproxSyncPolicy(32), *odom_sub, *cloud_sub));
    sync->registerCallback(boost::bind(&HdlGraphSlamNodelet::cloud_callback, this, _1, _2));
  
    imu_sub = nh.subscribe("/gpsimu_driver/imu_data", 1024, &HdlGraphSlamNodelet::imu_callback, this);
    floor_sub = nh.subscribe("/floor_detection/floor_coeffs", 1024, &HdlGraphSlamNodelet::floor_coeffs_callback, this);
    cloud_seg_sub = nh.subscribe("/segmented_clouds", 32, &HdlGraphSlamNodelet::cloud_seg_callback, this);

    if(private_nh.param<bool>("enable_gps", true)) {
      gps_sub = mt_nh.subscribe("/gps/geopoint", 1024, &HdlGraphSlamNodelet::gps_callback, this);
      nmea_sub = mt_nh.subscribe("/gpsimu_driver/nmea_sentence", 1024, &HdlGraphSlamNodelet::nmea_callback, this);
      navsat_sub = mt_nh.subscribe("/gps/navsat", 1024, &HdlGraphSlamNodelet::navsat_callback, this);
    }

    // publishers
    markers_pub = mt_nh.advertise<visualization_msgs::MarkerArray>("/hdl_graph_slam/markers", 16);
    odom2map_pub = mt_nh.advertise<geometry_msgs::TransformStamped>("/hdl_graph_slam/odom2map", 16);
    map_points_pub = mt_nh.advertise<sensor_msgs::PointCloud2>("/hdl_graph_slam/map_points", 1, true);
    read_until_pub = mt_nh.advertise<std_msgs::Header>("/hdl_graph_slam/read_until", 32);

    dump_service_server = mt_nh.advertiseService("/hdl_graph_slam/dump", &HdlGraphSlamNodelet::dump_service, this);
    save_map_service_server = mt_nh.advertiseService("/hdl_graph_slam/save_map", &HdlGraphSlamNodelet::save_map_service, this);

    graph_updated = false;
    double graph_update_interval = private_nh.param<double>("graph_update_interval", 3.0);
    double map_cloud_update_interval = private_nh.param<double>("map_cloud_update_interval", 10.0);
    optimization_timer = mt_nh.createWallTimer(ros::WallDuration(graph_update_interval), &HdlGraphSlamNodelet::optimization_timer_callback, this);
    map_publish_timer = mt_nh.createWallTimer(ros::WallDuration(map_cloud_update_interval), &HdlGraphSlamNodelet::map_points_publish_timer_callback, this);
  }

private:
  /**
   * @brief receive the initial transform between map and odom frame
   * @param map2odom_pose_msg
   */
  void init_map2odom_pose_callback(const geometry_msgs::PoseStamped pose_msg) {
    if(got_trans_odom2map) return;

    Eigen::Matrix3f mat3 = Eigen::Quaternionf(pose_msg.pose.orientation.w, pose_msg.pose.orientation.x, pose_msg.pose.orientation.y, pose_msg.pose.orientation.z).toRotationMatrix();

    trans_odom2map.block<3, 3>(0, 0) = mat3;
    trans_odom2map(0, 3) = pose_msg.pose.position.x;
    trans_odom2map(1, 3) = pose_msg.pose.position.y;
    trans_odom2map(2, 3) = pose_msg.pose.position.z;

    if(trans_odom2map.isIdentity())
      return;
    else {
      got_trans_odom2map = true;
    }
  }

  /**
   * @brief received point clouds are pushed to #keyframe_queue
   * @param odom_msg
   * @param cloud_msg
   */
  void cloud_callback(const nav_msgs::OdometryConstPtr& odom_msg, const sensor_msgs::PointCloud2::ConstPtr& cloud_msg) {
    const ros::Time& stamp = cloud_msg->header.stamp;
    Eigen::Isometry3d odom = odom2isometry(odom_msg);

    pcl::PointCloud<PointT>::Ptr cloud(new pcl::PointCloud<PointT>());
    pcl::fromROSMsg(*cloud_msg, *cloud);
    if(base_frame_id.empty()) {
      base_frame_id = cloud_msg->header.frame_id;
    }

    if(!keyframe_updater->update(odom)) {
      std::lock_guard<std::mutex> lock(keyframe_queue_mutex);
      if(keyframe_queue.empty()) {
        std_msgs::Header read_until;
        read_until.stamp = stamp + ros::Duration(10, 0);
        read_until.frame_id = points_topic;
        read_until_pub.publish(read_until);
        read_until.frame_id = "/filtered_points";
        read_until_pub.publish(read_until);
      }

      return;
    }

    double accum_d = keyframe_updater->get_accum_distance();
    KeyFrame::Ptr keyframe(new KeyFrame(stamp, odom, accum_d, cloud));

    std::lock_guard<std::mutex> lock(keyframe_queue_mutex);
    keyframe_queue.push_back(keyframe);
  }

  /**
   * @brief received segmented clouds pushed to be pushed #keyframe_queue
   * @param clouds_seg_msg
   */
  void cloud_seg_callback(const hdl_graph_slam::PointClouds::Ptr& clouds_seg_msg) {
      std::lock_guard<std::mutex> lock(cloud_seg_mutex);
      clouds_seg_queue.push_back(clouds_seg_msg);
  }

  /** 
  * @brief flush the accumulated cloud seg queue
  */
  bool flush_clouds_seg_queue(){
    std::lock_guard<std::mutex> lock(cloud_seg_mutex);

    if(keyframes.empty() ) {
      std::cout << "No keyframes" << std::endl;  
      return false;
    }
    else if (clouds_seg_queue.empty()) {
      std::cout << "Clouds seg queue is empty" << std::endl;  
      return false;
    }

    const auto& latest_keyframe_stamp = keyframes.back()->stamp;
   
    bool updated = false; int plane_id;
    for(const auto& clouds_seg_msg : clouds_seg_queue) {

      std::vector<plane_data_list> x_det_corridor_candidates, y_det_corridor_candidates;
      std::vector<plane_data_list> x_det_room_candidates, y_det_room_candidates;

      for(const auto& cloud_seg_msg : clouds_seg_msg->pointclouds) {

        if(cloud_seg_msg.header.stamp > latest_keyframe_stamp) {
          //std::cout << "cloud_seg time is greater than last keyframe stamp" << std::endl;
          break;
        }

        auto found = keyframe_hash.find(cloud_seg_msg.header.stamp);
        if(found == keyframe_hash.end()) {
          continue;
        }

        pcl::PointCloud<PointNormal>::Ptr cloud_seg_body(new pcl::PointCloud<PointNormal>());
        pcl::fromROSMsg(cloud_seg_msg, *cloud_seg_body);
        
        if(cloud_seg_body->points.size() < 100)
          continue;

        const auto& keyframe = found->second;
        keyframe->cloud_seg_body = cloud_seg_body;
    
        g2o::Plane3D det_plane_body_frame = Eigen::Vector4d(cloud_seg_body->back().normal_x, cloud_seg_body->back().normal_y, cloud_seg_body->back().normal_z, cloud_seg_body->back().curvature);
        g2o::Plane3D det_plane_map_frame = plane_in_map_frame(keyframe, det_plane_body_frame);

        if (fabs(det_plane_map_frame.coeffs()(0)) > 0.98) {              
          int plane_type = plane_class::X_VERT_PLANE; 
          //std::cout << "X det_plane_map_frame " << det_plane_map_frame.coeffs() << std::endl;
          plane_id = factor_planes(keyframe, det_plane_map_frame, det_plane_body_frame, plane_type);
          /* check for potential x corridor and room candidates */
          float length =  plane_length(keyframe->cloud_seg_body);       
          //std::cout << "length x: " << length << std::endl;
    
          plane_data_list x_plane_id_pair;
          x_plane_id_pair.plane = det_plane_map_frame; x_plane_id_pair.plane_local = det_plane_body_frame; 
          x_plane_id_pair.plane_length = length;
          x_plane_id_pair.plane_id = plane_id; 
          x_plane_id_pair.keyframe_node = keyframe->node;  
          x_plane_id_pair.keyframe_trans = keyframe->node->estimate().translation();
          x_plane_id_pair.is_structural_candidate = false;
          if(length >= corridor_min_plane_length) {
            //std::cout << "added x corridor candidate " << std::endl;
            x_det_corridor_candidates.push_back(x_plane_id_pair); 
          } 
          if(length >= room_min_plane_length && length <= room_max_plane_length) {
            //std::cout << "added x room candidate " << std::endl;
            x_det_room_candidates.push_back(x_plane_id_pair);
          }
          updated = true;
        }else if (fabs(det_plane_map_frame.coeffs()(1)) > 0.98) {                   
          int plane_type = plane_class::Y_VERT_PLANE;  
          //std::cout << "Y det_plane_map_frame " << det_plane_map_frame.coeffs() << std::endl;
          plane_id = factor_planes(keyframe, det_plane_map_frame, det_plane_body_frame, plane_type);

          /* check for potential y corridor and room candidates */
          float length =  plane_length(keyframe->cloud_seg_body);  
          //std::cout << "length y: " << length << std::endl;
          plane_data_list y_plane_id_pair;
          y_plane_id_pair.plane = det_plane_map_frame; y_plane_id_pair.plane_local = det_plane_body_frame; 
          y_plane_id_pair.plane_length = length;
          y_plane_id_pair.plane_id = plane_id; 
          y_plane_id_pair.keyframe_node = keyframe->node;
          y_plane_id_pair.keyframe_trans = keyframe->node->estimate().translation();         
          y_plane_id_pair.is_structural_candidate = false;

          if(length >= corridor_min_plane_length) {
            //std::cout << "added y corridor candidate " << std::endl;
            y_det_corridor_candidates.push_back(y_plane_id_pair); 
          } 
          if (length >= room_min_plane_length && length <= room_max_plane_length) {
            //std::cout << "added y room candidate " << std::endl;
            y_det_room_candidates.push_back(y_plane_id_pair);
          }
          updated = true;
        }else if (fabs(det_plane_map_frame.coeffs()(2)) > 0.98) {
          int plane_type = plane_class::HORT_PLANE;  
          plane_id = factor_planes(keyframe, det_plane_map_frame, det_plane_body_frame, plane_type);
          updated = true;
        }else 
          continue;
      }
      
      if(use_corridor_constraint) {
        std::vector<structure_data_list> x_corridor = sort_corridors(plane_class::X_VERT_PLANE, x_det_corridor_candidates);  
        std::vector<structure_data_list> y_corridor = sort_corridors(plane_class::Y_VERT_PLANE, y_det_corridor_candidates);  
        
        std::vector<plane_data_list> x_corridor_refined = refine_corridors(x_corridor);
        if(x_corridor_refined.size() == 2) 
          factor_corridors(plane_class::X_VERT_PLANE, x_corridor_refined[0], x_corridor_refined[1]);
        
        std::vector<plane_data_list> y_corridor_refined = refine_corridors(y_corridor);
        if(y_corridor_refined.size() == 2) 
          factor_corridors(plane_class::Y_VERT_PLANE, y_corridor_refined[0], y_corridor_refined[1]);

      }

      if(use_room_constraint)  {
        std::vector<structure_data_list> x_room_pair_vec = sort_rooms(plane_class::X_VERT_PLANE, x_det_room_candidates); 
        std::vector<structure_data_list> y_room_pair_vec = sort_rooms(plane_class::Y_VERT_PLANE, y_det_room_candidates);
        std::pair<std::vector<plane_data_list>,std::vector<plane_data_list>> refined_room_pair = refine_rooms(x_room_pair_vec, y_room_pair_vec);

        if(refined_room_pair.first.size() == 2 && refined_room_pair.second.size() == 2) {
          factor_rooms(refined_room_pair.first, refined_room_pair.second);
        }
      }
    }

    auto remove_loc = std::upper_bound(clouds_seg_queue.begin(), clouds_seg_queue.end(), latest_keyframe_stamp, [=](const ros::Time& stamp, const hdl_graph_slam::PointClouds::Ptr& clouds_seg) { return stamp < clouds_seg->header.stamp; });
    clouds_seg_queue.erase(clouds_seg_queue.begin(), remove_loc);

    return updated;
  }
  
  /**
   * @brief sort corridors and create their factors
  */
  std::vector<structure_data_list> sort_corridors(int plane_type, std::vector<plane_data_list> corridor_candidates) {
    std::vector<structure_data_list> corridor_pair_vec; 

    for(int i=0; i < corridor_candidates.size(); ++i) {
      for(int j=i+1; j < corridor_candidates.size(); ++j) {       
        correct_plane_d(plane_type, corridor_candidates[i].plane, corridor_candidates[j].plane);
        correct_plane_d(plane_type, corridor_candidates[i].plane_local, corridor_candidates[j].plane_local);
        float corr_width = width_between_planes(corridor_candidates[i].plane.coeffs(), corridor_candidates[j].plane.coeffs());
        std::cout << "Corr plane i coeffs of type " << plane_type << " " << corridor_candidates[i].plane.coeffs() << std::endl;
        std::cout << "Corr plane j coeffs of type " << plane_type << " " << corridor_candidates[j].plane.coeffs() << std::endl;
        std::cout << "Corr_width: " << corr_width << std::endl;
        float diff_plane_length = fabs(corridor_candidates[i].plane_length - corridor_candidates[j].plane_length); 
        std::cout << "corr diff_plane_length: " << diff_plane_length << std::endl;
        
        if (corridor_candidates[i].plane.coeffs().head(3).dot(corridor_candidates[j].plane.coeffs().head(3)) < 0 && (corr_width < corridor_max_width && corr_width > corridor_min_width)
           && diff_plane_length < corridor_plane_length_diff_threshold) {
            //corridor_candidates[i].is_structural_candidate = corridor_candidates[j].is_structural_candidate = true; 
            structure_data_list corridor_pair;
            corridor_pair.plane1 = corridor_candidates[i];
            corridor_pair.plane2 = corridor_candidates[j];
            corridor_pair.width  = corr_width;
            corridor_pair.length_diff  = diff_plane_length;
            corridor_pair_vec.push_back(corridor_pair);
        } 
      } 
    }

    return corridor_pair_vec;
  }

  std::vector<plane_data_list> refine_corridors(std::vector<structure_data_list> corr_vec) {
    float min_width_diff=corridor_min_width; float min_corr_length_diff=100;
    std::vector<plane_data_list> corr_refined; corr_refined.resize(2);
    
    for(int i=0; i<corr_vec.size(); ++i) {
      float width_diff = fabs(corridor_max_width - corr_vec[i].width); 
      if(corr_vec[i].length_diff < min_corr_length_diff) {
          min_corr_length_diff = corr_vec[i].length_diff;
          corr_refined[0] = corr_vec[i].plane1;
          corr_refined[1] = corr_vec[i].plane2;
        }
      }
    
    if(min_corr_length_diff >= 100){
      std::vector<plane_data_list> corr_empty; corr_empty.resize(0);
      return corr_empty;
    }
    else  
      return corr_refined;
  }


  std::vector<structure_data_list> sort_rooms(int plane_type, std::vector<plane_data_list> room_candidates) {
    std::vector<structure_data_list> room_pair_vec; 

    for(int i=0; i < room_candidates.size(); ++i) {
        for(int j=i+1; j < room_candidates.size(); ++j) {
          correct_plane_d(plane_type, room_candidates[i].plane, room_candidates[j].plane);
          correct_plane_d(plane_type, room_candidates[i].plane_local, room_candidates[j].plane_local);
          float room_width = width_between_planes(room_candidates[i].plane.coeffs(), room_candidates[j].plane.coeffs());
          std::cout << "Room plane i coeffs of type " << plane_type << " " << room_candidates[i].plane.coeffs() << std::endl;
          std::cout << "Room plane j coeffs of type " << plane_type << " " << room_candidates[j].plane.coeffs() << std::endl;
          std::cout << "rooom width : " << room_width << std::endl;
          float diff_plane_length = fabs(room_candidates[i].plane_length - room_candidates[j].plane_length); 
          std::cout << "room diff_plane_length: " << diff_plane_length << std::endl;
          
          if (room_candidates[i].plane.coeffs().head(3).dot(room_candidates[j].plane.coeffs().head(3)) < 0 && room_width > room_min_width && diff_plane_length < room_plane_length_diff_threshold) {
            structure_data_list room_pair;
            room_pair.plane1 = room_candidates[i];
            room_pair.plane2 = room_candidates[j];
            room_pair.width  = room_width;
            room_pair.length_diff = diff_plane_length;
            room_pair_vec.push_back(room_pair);
            std::cout << "Adding room candidates" << std::endl;
          }
       }
    }
    return room_pair_vec;
  }

  std::pair<std::vector<plane_data_list>,std::vector<plane_data_list>> refine_rooms(std::vector<structure_data_list> x_room_vec, std::vector<structure_data_list> y_room_vec) {
    float min_width_diff=2.5;
    std::vector<plane_data_list> x_room, y_room;
    x_room.resize(2); y_room.resize(2);
    
    for(int i=0; i<x_room_vec.size(); ++i) {
      for(int j=0; j<y_room_vec.size(); ++j) {
          float width_diff = fabs(x_room_vec[i].width - y_room_vec[j].width); 
          if(width_diff < min_width_diff) {              
            min_width_diff = width_diff;
            x_room[0] = x_room_vec[i].plane1;
            x_room[1] = x_room_vec[i].plane2;
            y_room[0] = y_room_vec[j].plane1;
            y_room[1] = y_room_vec[j].plane2;
        }
      }
    }
    
    if(min_width_diff >= 2.5) {
      std::vector<plane_data_list> x_room_empty, y_room_empty;
      x_room_empty.resize(0); y_room_empty.resize(0);
      return std::make_pair(x_room_empty, x_room_empty);
    }
    else 
      return std::make_pair(x_room, y_room);
  }

  /**
  * @brief convert body plane coefficients to map frame
  */
  g2o::Plane3D plane_in_map_frame(KeyFrame::Ptr keyframe, g2o::Plane3D det_plane_body_frame) {
    g2o::Plane3D det_plane_map_frame; 
    Eigen::Vector4d map_coeffs;

    Eigen::Isometry3d w2n = keyframe->node->estimate();
    map_coeffs.head<3>() =  w2n.rotation() * det_plane_body_frame.coeffs().head<3>();
    map_coeffs(3) = det_plane_body_frame.coeffs()(3) - w2n.translation().dot(map_coeffs.head<3>());
    det_plane_map_frame = map_coeffs;

    return det_plane_map_frame;
  }

  // /**  
  // * @brief Converting the cloud to map frame
  // */
  // pcl::PointCloud<PointNormal>::Ptr convert_cloud_to_map(KeyFrame::Ptr keyframe) {
  //   pcl::PointCloud<PointNormal>::Ptr cloud_seg_map(new pcl::PointCloud<PointNormal>());
  //   Eigen::Matrix4f pose = keyframe->node->estimate().matrix().cast<float>();
  //   for(const auto& src_pt : keyframe->cloud_seg_body->points) {
  //     PointNormal dst_pt;
  //     dst_pt.getVector4fMap() = pose * src_pt.getVector4fMap();
  //     cloud_seg_map->push_back(dst_pt);
  //   }
  //   return cloud_seg_map;
  // }

  /** 
  * @brief create vertical plane factors
  */
  int factor_planes(KeyFrame::Ptr keyframe, g2o::Plane3D det_plane_map_frame, g2o::Plane3D det_plane_body_frame, int plane_type) {
    g2o::VertexPlane* plane_node; 

    Eigen::Matrix4d Gij;
    Gij.setZero();  
    if(use_point_to_plane) {
      auto it = keyframe->cloud_seg_body->points.begin();
      while (it != keyframe->cloud_seg_body->points.end()) {
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
      
    // pcl::PointCloud<PointNormal>::Ptr cloud_seg_map = convert_cloud_to_map(keyframe);
    // if(cloud_seg_map->points.empty()) { 
    //   std::cout << "Could not convert the cloud to body frame";
    //   return false;
    // }
    
    std::pair<int,int> data_association; data_association.first = -1;
    bool new_plane_node_added = false;
    data_association = associate_plane(keyframe, det_plane_body_frame.coeffs(), plane_type);

    if (plane_type == plane_class::X_VERT_PLANE) {  
      if(x_vert_planes.empty() || data_association.first == -1) {
          data_association.first = graph_slam->num_vertices();
          plane_node = graph_slam->add_plane_node(det_plane_map_frame.coeffs());
          //x_vert_plane_node->setFixed(true);
          //std::cout << "Added new x vertical plane node with coeffs " <<  det_plane_map_frame.coeffs() << std::endl;
          VerticalPlanes vert_plane;
          vert_plane.id = data_association.first;
          vert_plane.plane = det_plane_map_frame.coeffs();
          vert_plane.cloud_seg_body = keyframe->cloud_seg_body;
          vert_plane.keyframe_node = keyframe->node; 
          vert_plane.plane_node = plane_node; 
          vert_plane.covariance = Eigen::Matrix3d::Identity();
          vert_plane.parallel_pair = false;
          x_vert_planes.push_back(vert_plane);
          new_plane_node_added = true;
      } else {
          //std::cout << "matched x vert plane with x vert plane of id " << std::to_string(data_association.first)  << std::endl;
          plane_node = x_vert_planes[data_association.second].plane_node;
      }
    } else if (plane_type == plane_class::Y_VERT_PLANE) {      
      if(y_vert_planes.empty() || data_association.first == -1) {
        data_association.first = graph_slam->num_vertices();
        plane_node = graph_slam->add_plane_node(det_plane_map_frame.coeffs());
        //std::cout << "Added new y vertical plane node with coeffs " <<  det_plane_map_frame.coeffs() << std::endl;
        VerticalPlanes vert_plane;
        vert_plane.id = data_association.first;
        vert_plane.plane = det_plane_map_frame.coeffs();
        vert_plane.cloud_seg_body = keyframe->cloud_seg_body;
        vert_plane.keyframe_node = keyframe->node; 
        vert_plane.plane_node = plane_node; 
        vert_plane.covariance = Eigen::Matrix3d::Identity();
        vert_plane.parallel_pair = false;
        y_vert_planes.push_back(vert_plane);
        new_plane_node_added = true; 
      } else {
          //std::cout << "matched y vert plane with y vert plane of id " << std::to_string(data_association.first)  << std::endl;
          plane_node = y_vert_planes[data_association.second].plane_node;

        } 
      } else if (plane_type == plane_class::HORT_PLANE) {       
        if(hort_planes.empty() || data_association.first == -1) {
          data_association.first = graph_slam->num_vertices();
          plane_node = graph_slam->add_plane_node(det_plane_map_frame.coeffs());
          //std::cout << "Added new horizontal plane node with coeffs " <<  det_plane_map_frame.coeffs() << std::endl;
          HorizontalPlanes hort_plane;
          hort_plane.id = data_association.first;
          hort_plane.plane = det_plane_map_frame.coeffs();
          hort_plane.cloud_seg_body = keyframe->cloud_seg_body;
          hort_plane.keyframe_node = keyframe->node; 
          hort_plane.plane_node = plane_node; 
          hort_plane.covariance = Eigen::Matrix3d::Identity();
          hort_planes.push_back(hort_plane);
          new_plane_node_added = true;
      } else {
        //std::cout << "matched hort plane with hort plane of id " << std::to_string(data_association.first)  << std::endl;
        plane_node = hort_planes[data_association.second].plane_node;
      }
    } 
    

    if(use_point_to_plane) {
      Eigen::Matrix<double, 1, 1> information(0.001);
      auto edge = graph_slam->add_se3_point_to_plane_edge(keyframe->node, plane_node, Gij, information);
      graph_slam->add_robust_kernel(edge, "Huber", 1.0);
    } else {
      Eigen::Matrix3d information = 0.1 * Eigen::Matrix3d::Identity();  
      auto edge = graph_slam->add_se3_plane_edge(keyframe->node, plane_node, det_plane_body_frame.coeffs(), information);
      graph_slam->add_robust_kernel(edge, "Huber", 1.0);
    }    

    if(use_parallel_plane_constraint && new_plane_node_added) {
      parallel_plane_constraint(plane_node, data_association.first, plane_type);
    }
    if(use_perpendicular_plane_constraint && new_plane_node_added) {
      perpendicular_plane_constraint(plane_node, data_association.first, plane_type);
    }

    return data_association.first;
  }

  /** 
  * @brief data assoction betweeen the planes
  */
  std::pair<int,int> associate_plane(KeyFrame::Ptr keyframe, g2o::Plane3D det_plane, int plane_type) {
    std::pair<int,int> data_association;
    float min_dist = 100;
    double min_maha_dist = 100;  
    Eigen::Isometry3d m2n = keyframe->estimate().inverse();

    if(plane_type == plane_class::X_VERT_PLANE) {
      for(int i=0; i< x_vert_planes.size(); ++i) { 
        float dist = fabs(det_plane.coeffs()(3) - x_vert_planes[i].plane.coeffs()(3));
        //std::cout << "distance x: " << dist << std::endl;
        if(dist < min_dist){
          min_dist = dist;
          //id = x_vert_planes[i].id;
        }
        g2o::Plane3D local_plane = m2n * x_vert_planes[i].plane;
        Eigen::Vector3d error = local_plane.ominus(det_plane);
        double maha_dist = sqrt(error.transpose() * x_vert_planes[i].covariance.inverse() * error);
        //std::cout << "cov x: " << x_vert_planes[i].covariance.inverse() << std::endl;
        //std::cout << "maha distance x: " << maha_dist << std::endl;

        if(std::isnan(maha_dist) || maha_dist < 1e-3) {
            Eigen::Matrix3d cov = Eigen::Matrix3d::Identity();
            maha_dist = sqrt(error.transpose() * cov * error);            
          } 
        if(maha_dist < min_maha_dist) {
          min_maha_dist = maha_dist;
          data_association.first = x_vert_planes[i].id;
          data_association.second = i;
          }
        }
      }

    if(plane_type == plane_class::Y_VERT_PLANE) {
        for(int i=0; i< y_vert_planes.size(); ++i) { 
          float dist = fabs(det_plane.coeffs()(3) - y_vert_planes[i].plane.coeffs()(3));
          //std::cout << "distance y: " << dist << std::endl;
          if(dist < min_dist){
            min_dist = dist;
            //id = y_vert_planes[i].id;
          }
          g2o::Plane3D local_plane = m2n * y_vert_planes[i].plane;
          Eigen::Vector3d error = local_plane.ominus(det_plane);
          double maha_dist = sqrt(error.transpose() * y_vert_planes[i].covariance.inverse() * error);
          //std::cout << "cov y: " << y_vert_planes[i].covariance.inverse() << std::endl;
          //std::cout << "maha distance y: " << maha_dist << std::endl;
          if(std::isnan(maha_dist) || maha_dist < 1e-3) {
            Eigen::Matrix3d cov = Eigen::Matrix3d::Identity();
            maha_dist = sqrt(error.transpose() * cov * error);            
          } 
          if(maha_dist < min_maha_dist) {
            min_maha_dist = maha_dist;
            data_association.first = y_vert_planes[i].id;
            data_association.second = i;
            }
          }   
      }

    if(plane_type == plane_class::HORT_PLANE) {
        for(int i=0; i< hort_planes.size(); ++i) { 
          float dist = fabs(det_plane.coeffs()(3) - hort_planes[i].plane.coeffs()(3));
          //std::cout << "distance hort: " << dist << std::endl;
          if(dist < min_dist){
            min_dist = dist;
            //id = y_vert_planes[i].id;
          }
          g2o::Plane3D local_plane = m2n * hort_planes[i].plane;
          Eigen::Vector3d error = local_plane.ominus(det_plane);
          double maha_dist = sqrt(error.transpose() * hort_planes[i].covariance.inverse() * error);
          //std::cout << "cov hor: " << hort_planes[i].covariance.inverse() << std::endl;
          //std::cout << "maha distance hort: " << maha_dist << std::endl;
          if(std::isnan(maha_dist) || maha_dist < 1e-3) {
            Eigen::Matrix3d cov = Eigen::Matrix3d::Identity();
            maha_dist = sqrt(error.transpose() * cov * error);            
          } 
          if(maha_dist < min_maha_dist) {
            min_maha_dist = maha_dist;
            data_association.first = hort_planes[i].id;
            data_association.second = i;
            }
          }   
      }

      //std::cout << "min_dist: " << min_dist << std::endl;
      //std::cout << "min_mah_dist: " << min_maha_dist << std::endl;

      // if(min_dist > 0.30)
      //   id = -1;
      if(min_maha_dist > plane_dist_threshold)
         data_association.first = -1;

    return data_association;
  }
  
  /**  
  * @brief this method add parallel constraint between the planes
  */
  void parallel_plane_constraint(g2o::VertexPlane* plane_node, int id, int plane_type) {
    Eigen::Matrix<double, 1, 1> information(0.001);
    Eigen::Vector3d meas(0,0,0);
    if(plane_type == plane_class::X_VERT_PLANE) {
      for(int i=0; i<x_vert_planes.size(); ++i){
        if(id != x_vert_planes[i].id) {
          auto edge = graph_slam->add_plane_parallel_edge(x_vert_planes[i].plane_node, plane_node, meas, information);
          graph_slam->add_robust_kernel(edge, "Huber", 1.0);
          x_vert_planes[i].parallel_pair = true;
        }
      }
    }
    if(plane_type == plane_class::Y_VERT_PLANE) {
      for(int i=0; i<y_vert_planes.size(); ++i){
        if(id != y_vert_planes[i].id) {
          auto edge = graph_slam->add_plane_parallel_edge(y_vert_planes[i].plane_node, plane_node, meas, information);
          graph_slam->add_robust_kernel(edge, "Huber", 1.0);
          y_vert_planes[i].parallel_pair = true;
        }
      }
    }
    if(plane_type == plane_class::HORT_PLANE) {
      for(int i=0; i<hort_planes.size(); ++i){
        if(id != hort_planes[i].id) {
          auto edge = graph_slam->add_plane_parallel_edge(hort_planes[i].plane_node, plane_node, meas, information);
          graph_slam->add_robust_kernel(edge, "Huber", 1.0);
          hort_planes[i].parallel_pair = true;
        }
      }
    }
  }

  /**  
  * @brief this method adds perpendicular constraint between the planes
  */
  void perpendicular_plane_constraint(g2o::VertexPlane* plane_node, int id, int plane_type) {
    Eigen::Matrix<double, 1, 1> information(0.001);
    Eigen::Vector3d meas(0,0,0);
    if(plane_type == plane_class::X_VERT_PLANE) {
      for(int i=0; i<y_vert_planes.size(); ++i){
          auto edge = graph_slam->add_plane_perpendicular_edge(y_vert_planes[i].plane_node, plane_node, meas, information);
          graph_slam->add_robust_kernel(edge, "Huber", 1.0);
      }
    }
    if(plane_type == plane_class::Y_VERT_PLANE) {
      for(int i=0; i<x_vert_planes.size(); ++i){
          auto edge = graph_slam->add_plane_perpendicular_edge(x_vert_planes[i].plane_node, plane_node, meas, information);
          graph_slam->add_robust_kernel(edge, "Huber", 1.0);
      }
    } 
    if(plane_type == plane_class::HORT_PLANE) {
      for(int i=0; i<x_vert_planes.size(); ++i){
          auto edge = graph_slam->add_plane_perpendicular_edge(x_vert_planes[i].plane_node, plane_node, meas, information);
          graph_slam->add_robust_kernel(edge, "Huber", 1.0);
      }
      for(int i=0; i<y_vert_planes.size(); ++i){
          auto edge = graph_slam->add_plane_perpendicular_edge(y_vert_planes[i].plane_node, plane_node, meas, information);
          graph_slam->add_robust_kernel(edge, "Huber", 1.0);
      }
    } 
  }

  void factor_corridors(int plane_type, plane_data_list corr_plane1_pair, plane_data_list corr_plane2_pair) {
    g2o::VertexCorridor* corr_node;  std::pair<int,int> corr_data_association;   
    Eigen::Vector3d meas_plane1, meas_plane2;
    Eigen::Matrix<double, 3, 3> information_se3_corridor = 0.01 * Eigen::Matrix3d::Identity();
    Eigen::Matrix<double, 1, 1> information_corridor_plane(0.01);
    Eigen::Vector3d pre_corr_pose = pre_corridor_pose(plane_type, corr_plane1_pair.plane.coeffs(), corr_plane2_pair.plane.coeffs());
    
    if(plane_type == plane_class::X_VERT_PLANE) { 
      Eigen::Vector3d corr_pose;
      auto found_plane1 = x_vert_planes.begin();
      auto found_plane2 = x_vert_planes.begin();
      corr_data_association = associate_corridors(plane_type, pre_corr_pose);

      if((x_corridors.empty() || corr_data_association.first == -1)) {
        
        std::cout << "found an X corridor with pre pose " << pre_corr_pose <<  " between plane id " << corr_plane1_pair.plane_id << " and plane id " << corr_plane2_pair.plane_id << std::endl;
        Eigen::Vector3d corr_pose = final_corridor_pose(plane_type, pre_corr_pose, corr_plane1_pair.keyframe_node);
        corr_data_association.first = graph_slam->num_vertices();
        corr_node = graph_slam->add_corridor_node(corr_pose);
        //corr_node->setFixed(true);
        Corridors det_corridor;
        det_corridor.id = corr_data_association.first;
        det_corridor.plane1 = corr_plane1_pair.plane; det_corridor.plane2 = corr_plane2_pair.plane; 
        det_corridor.plane1_id = corr_plane1_pair.plane_id; det_corridor.plane2_id = corr_plane2_pair.plane_id; 
        det_corridor.keyframe_trans = corr_plane1_pair.keyframe_node->estimate().translation().head(3);
        det_corridor.node = corr_node;   
        x_corridors.push_back(det_corridor);

        found_plane1 = std::find_if(x_vert_planes.begin(), x_vert_planes.end(), boost::bind(&VerticalPlanes::id, _1) == corr_plane1_pair.plane_id);
        found_plane2 = std::find_if(x_vert_planes.begin(), x_vert_planes.end(), boost::bind(&VerticalPlanes::id, _1) == corr_plane2_pair.plane_id);
        meas_plane1 =  corridor_measurement(plane_type, corr_node->estimate(), corr_plane1_pair.plane.coeffs());
        meas_plane2 =  corridor_measurement(plane_type, corr_node->estimate(), corr_plane2_pair.plane.coeffs());
      } else {
        /* add the edge between detected planes and the corridor */
        corr_node = x_corridors[corr_data_association.second].node;
        std::cout << "Matched det corridor X with pre pose " << pre_corr_pose << " to mapped corridor with id " << corr_data_association.first << " and pose " << corr_node->estimate()  << std::endl;
        
        found_plane1 = std::find_if(x_vert_planes.begin(), x_vert_planes.end(), boost::bind(&VerticalPlanes::id, _1) == corr_plane1_pair.plane_id);
        found_plane2 = std::find_if(x_vert_planes.begin(), x_vert_planes.end(), boost::bind(&VerticalPlanes::id, _1) == corr_plane2_pair.plane_id);
        meas_plane1 =  corridor_measurement(plane_type, corr_node->estimate(), corr_plane1_pair.plane.coeffs());
        meas_plane2 =  corridor_measurement(plane_type, corr_node->estimate(), corr_plane2_pair.plane.coeffs());
      }
      
      Eigen::Vector3d corr_pose_local = corridor_pose_local(corr_plane1_pair.keyframe_node, corr_node->estimate());
      std::cout << "corr pose local: " << corr_pose_local << std::endl;
      auto edge_se3_corridor = graph_slam->add_se3_corridor_edge(corr_plane1_pair.keyframe_node, corr_node, corr_pose_local, information_se3_corridor);
      graph_slam->add_robust_kernel(edge_se3_corridor, "Huber", 1.0);

      auto edge_plane1 = graph_slam->add_corridor_xplane_edge(corr_node, (*found_plane1).plane_node, meas_plane1, information_corridor_plane);
      graph_slam->add_robust_kernel(edge_plane1, "Huber", 1.0);

      auto edge_plane2 = graph_slam->add_corridor_xplane_edge(corr_node, (*found_plane2).plane_node, meas_plane2, information_corridor_plane);
      graph_slam->add_robust_kernel(edge_plane2, "Huber", 1.0);
    }

    if(plane_type == plane_class::Y_VERT_PLANE) {
      Eigen::Vector3d corr_pose;
      auto found_plane1 = y_vert_planes.begin();
      auto found_plane2 = y_vert_planes.begin();
      corr_data_association = associate_corridors(plane_type, pre_corr_pose);

      if((y_corridors.empty() || corr_data_association.first == -1)) {

        std::cout << "found an Y corridor with pre pose " << pre_corr_pose <<  " between plane id " << corr_plane1_pair.plane_id << " and plane id " << corr_plane2_pair.plane_id << std::endl;
        corr_pose = final_corridor_pose(plane_type, pre_corr_pose, corr_plane1_pair.keyframe_node);
        corr_data_association.first = graph_slam->num_vertices();
        corr_node = graph_slam->add_corridor_node(corr_pose);
        //corr_node->setFixed(true);
        Corridors det_corridor;
        det_corridor.id = corr_data_association.first;
        det_corridor.plane1 = corr_plane1_pair.plane; det_corridor.plane2 = corr_plane2_pair.plane; 
        det_corridor.plane1_id = corr_plane1_pair.plane_id; det_corridor.plane2_id = corr_plane2_pair.plane_id; 
        det_corridor.keyframe_trans = corr_plane1_pair.keyframe_node->estimate().translation().head(3);
        det_corridor.node = corr_node;      
        y_corridors.push_back(det_corridor);
        
        found_plane1 = std::find_if(y_vert_planes.begin(), y_vert_planes.end(), boost::bind(&VerticalPlanes::id, _1) == corr_plane1_pair.plane_id);
        found_plane2 = std::find_if(y_vert_planes.begin(), y_vert_planes.end(), boost::bind(&VerticalPlanes::id, _1) == corr_plane2_pair.plane_id);
        meas_plane1 =  corridor_measurement(plane_type, corr_node->estimate(), corr_plane1_pair.plane.coeffs());
        meas_plane2 =  corridor_measurement(plane_type, corr_node->estimate(), corr_plane2_pair.plane.coeffs());

       } else {
        /* add the edge between detected planes and the corridor */
        corr_node = y_corridors[corr_data_association.second].node;
        std::cout << "Matched det corridor Y with pre pose " << pre_corr_pose << " to mapped corridor with id " << corr_data_association.first << " and pose " << corr_node->estimate()  << std::endl;

        found_plane1 = std::find_if(y_vert_planes.begin(), y_vert_planes.end(), boost::bind(&VerticalPlanes::id, _1) == corr_plane1_pair.plane_id);
        found_plane2 = std::find_if(y_vert_planes.begin(), y_vert_planes.end(), boost::bind(&VerticalPlanes::id, _1) == corr_plane2_pair.plane_id);
        meas_plane1 =  corridor_measurement(plane_type, corr_node->estimate(), corr_plane1_pair.plane.coeffs());
        meas_plane2 =  corridor_measurement(plane_type, corr_node->estimate(), corr_plane2_pair.plane.coeffs());
      }
      
      Eigen::Vector3d corr_pose_local = corridor_pose_local(corr_plane1_pair.keyframe_node, corr_node->estimate());
      auto edge_se3_corridor = graph_slam->add_se3_corridor_edge(corr_plane1_pair.keyframe_node, corr_node, corr_pose_local, information_se3_corridor);
      graph_slam->add_robust_kernel(edge_se3_corridor, "Huber", 1.0);

      auto edge_plane1 = graph_slam->add_corridor_yplane_edge(corr_node, (*found_plane1).plane_node, meas_plane1, information_corridor_plane);
      graph_slam->add_robust_kernel(edge_plane1, "Huber", 1.0);

      auto edge_plane2 = graph_slam->add_corridor_yplane_edge(corr_node, (*found_plane2).plane_node, meas_plane2, information_corridor_plane);
      graph_slam->add_robust_kernel(edge_plane2, "Huber", 1.0); 
    } 

    return;
  }

  Eigen::Vector3d pre_corridor_pose(int plane_type, Eigen::Vector4d v1, Eigen::Vector4d v2) {
    Eigen::Vector3d corridor_pose(0,0,0);

    if(plane_type == plane_class::X_VERT_PLANE) {
      if(fabs(v1(3)) > fabs(v2(3))) {
        double size = v1(3) - v2(3);
        corridor_pose(0) = ((size)/2) + v2(3); 
      } else {
        double size = v2(3) - v1(3);
        corridor_pose(0) = ((size)/2) + v1(3);
      }
    }

    if(plane_type == plane_class::Y_VERT_PLANE) {
      if(fabs(v1(3)) > fabs(v2(3))) {
        double size = v1(3) - v2(3);
        corridor_pose(1) = ((size)/2) + v2(3); 
      } else {
        double size = v2(3) - v1(3);
        corridor_pose(1) = ((size)/2) + v1(3);
      }
    }
    
    return corridor_pose;
  }

  Eigen::Vector3d final_corridor_pose(int plane_type, Eigen::Vector3d pre_corr_pose, g2o::VertexSE3* keyframe_node) {
    Eigen::Vector3d corridor_pose;
    
    if(plane_type == plane_class::X_VERT_PLANE) {
      corridor_pose(0) = pre_corr_pose(0); 
      corridor_pose(1) = keyframe_node->estimate().translation()(1);
      corridor_pose(2) = keyframe_node->estimate().translation()(2);
    }    

    if(plane_type == plane_class::Y_VERT_PLANE) {
        corridor_pose(0) = keyframe_node->estimate().translation()(0); 
        corridor_pose(1) = pre_corr_pose(1); 
        corridor_pose(2) = keyframe_node->estimate().translation()(2);
    }

    return corridor_pose;
  }

  Eigen::Vector3d corridor_pose_local(g2o::VertexSE3* keyframe_node, Eigen::Vector3d corr_pose) {
    Eigen::Isometry3d corridor_pose_map;
    corridor_pose_map.matrix().block<4,4>(0,0) = Eigen::Matrix4d::Identity(); corridor_pose_map.matrix().block<3,1>(0,3) = corr_pose;  

    Eigen::Isometry3d corridor_pose_local = corridor_pose_map * keyframe_node->estimate().inverse();

    return corridor_pose_local.matrix().block<3,1>(0,3);
  }


  Eigen::Vector3d corridor_measurement(int plane_type, Eigen::Vector3d corr, Eigen::Vector4d plane) {
    Eigen::Vector3d meas(0,0,0);  

    if(plane_type == plane_class::X_VERT_PLANE) {
      if(fabs(corr(0)) > fabs(plane(3))) {
        meas(0) =  corr(0) - plane(3);
      } else {
        meas(0) =  plane(3) - corr(0);
      }
    }  

    if(plane_type == plane_class::Y_VERT_PLANE) {
      if(fabs(corr(1)) > fabs(plane(3))) {
        meas(0) =  corr(1) - plane(3);
      } else {
        meas(0) =  plane(3) - corr(1);
      }
    } 

    return meas;
  }

  std::pair<int,int> associate_corridors(int plane_type, Eigen::Vector3d corr_pose) {
    float min_dist = 100;

    std::pair<int,int> data_association; data_association.first = -1;

    if(plane_type == plane_class::X_VERT_PLANE) {
      for(int i=0; i< x_corridors.size(); ++i) { 
        float dist = fabs((corr_pose(0)) - (x_corridors[i].node->estimate()(0)));
        if(dist < min_dist) {
          min_dist = dist;
          std::cout << "dist X corr: " << dist << std::endl;
          data_association.first = x_corridors[i].id;
          data_association.second = i;
        }
      }
    }


   if(plane_type == plane_class::Y_VERT_PLANE) {
      for(int i=0; i< y_corridors.size(); ++i) { 
        float dist = fabs((corr_pose(1)) - (y_corridors[i].node->estimate()(1)));
        if(dist < min_dist) {
          min_dist = dist;
          std::cout << "dist Y corr: " << dist << std::endl;
          data_association.first = y_corridors[i].id;
          data_association.second = i;
        }
      }
    }

    std::cout << "min dist: " << min_dist << std::endl;
    if (min_dist > corridor_dist_threshold) 
      data_association.first = -1;

    return data_association;
  }

  void factor_rooms(std::vector<plane_data_list> x_room_pair_vec, std::vector<plane_data_list> y_room_pair_vec) {
    g2o::VertexRoomXYLB* room_node;  std::pair<int,int> room_data_association;   
    Eigen::Matrix<double, 2, 2> information_se3_room = 0.01 * Eigen::Matrix2d::Identity();
    Eigen::Matrix<double, 1, 1> information_room_plane(0.01);

    auto found_x_plane1 = x_vert_planes.begin();
    auto found_x_plane2 = x_vert_planes.begin();
    auto found_y_plane1 = y_vert_planes.begin();
    auto found_y_plane2 = y_vert_planes.begin();
    double x_plane1_meas, x_plane2_meas;
    double y_plane1_meas, y_plane2_meas;

    Eigen::Vector2d room_pose = compute_room_pose(x_room_pair_vec, y_room_pair_vec);
    Eigen::Vector2d room_pose_local = compute_room_pose_local(x_room_pair_vec[0].keyframe_node, room_pose);
    room_data_association = associate_rooms(room_pose);   
    if((rooms_vec.empty() || room_data_association.first == -1)) {
        std::cout << "found first room with pose " << room_pose << std::endl;
        room_data_association.first = graph_slam->num_vertices();
        room_node = graph_slam->add_room_node(room_pose);
        //room_node->setFixed(true);
        Rooms det_room;
        det_room.id = room_data_association.first;     
        det_room.plane_x1 = x_room_pair_vec[0].plane; det_room.plane_x2 = x_room_pair_vec[1].plane;
        det_room.plane_y1 = y_room_pair_vec[0].plane; det_room.plane_y2 = y_room_pair_vec[1].plane;       
        det_room.plane_x1_id = x_room_pair_vec[0].plane_id; det_room.plane_x2_id = x_room_pair_vec[1].plane_id; 
        det_room.plane_y1_id = y_room_pair_vec[0].plane_id; det_room.plane_y2_id = y_room_pair_vec[1].plane_id; 
        det_room.node = room_node;   
        rooms_vec.push_back(det_room);

    } else {
        /* add the edge between detected planes and the corridor */
        room_node = rooms_vec[room_data_association.second].node;
        std::cout << "Matched det room with pose " << room_pose << " to mapped room with id " << room_data_association.first << " and pose " << room_node->estimate()  << std::endl;
    }

      found_x_plane1 = std::find_if(x_vert_planes.begin(), x_vert_planes.end(), boost::bind(&VerticalPlanes::id, _1) == x_room_pair_vec[0].plane_id);
      found_x_plane2 = std::find_if(x_vert_planes.begin(), x_vert_planes.end(), boost::bind(&VerticalPlanes::id, _1) == x_room_pair_vec[1].plane_id);
      x_plane1_meas =  room_measurement(plane_class::X_VERT_PLANE, room_pose, x_room_pair_vec[0].plane.coeffs());
      x_plane2_meas =  room_measurement(plane_class::X_VERT_PLANE, room_pose, x_room_pair_vec[1].plane.coeffs());

      found_y_plane1 = std::find_if(y_vert_planes.begin(), y_vert_planes.end(), boost::bind(&VerticalPlanes::id, _1) == y_room_pair_vec[0].plane_id);
      found_y_plane2 = std::find_if(y_vert_planes.begin(), y_vert_planes.end(), boost::bind(&VerticalPlanes::id, _1) == y_room_pair_vec[1].plane_id);
      y_plane1_meas =  room_measurement(plane_class::Y_VERT_PLANE, room_pose, y_room_pair_vec[0].plane.coeffs());
      y_plane2_meas =  room_measurement(plane_class::Y_VERT_PLANE, room_pose, y_room_pair_vec[1].plane.coeffs());

      std::cout << "room pose local: " << room_pose_local << std::endl;
      auto edge_se3_room = graph_slam->add_se3_room_edge(x_room_pair_vec[0].keyframe_node, room_node, room_pose_local, information_se3_room);
      graph_slam->add_robust_kernel(edge_se3_room, "Huber", 1.0);

      auto edge_x_plane1 = graph_slam->add_room_xplane_edge(room_node, (*found_x_plane1).plane_node, x_plane1_meas, information_room_plane);
      graph_slam->add_robust_kernel(edge_x_plane1, "Huber", 1.0);

      auto edge_x_plane2 = graph_slam->add_room_xplane_edge(room_node, (*found_x_plane2).plane_node, x_plane2_meas, information_room_plane);
      graph_slam->add_robust_kernel(edge_x_plane2, "Huber", 1.0);

      auto edge_y_plane1 = graph_slam->add_room_yplane_edge(room_node, (*found_y_plane1).plane_node, y_plane1_meas, information_room_plane);
      graph_slam->add_robust_kernel(edge_y_plane1, "Huber", 1.0);

      auto edge_y_plane2 = graph_slam->add_room_yplane_edge(room_node, (*found_y_plane2).plane_node, y_plane2_meas, information_room_plane);
      graph_slam->add_robust_kernel(edge_y_plane2, "Huber", 1.0);

  }

  Eigen::Vector2d compute_room_pose(std::vector<plane_data_list> x_room_pair_vec, std::vector<plane_data_list> y_room_pair_vec) {
    Eigen::Vector2d room_pose(0,0);
    Eigen::Vector4d x_plane1 = x_room_pair_vec[0].plane.coeffs(),  x_plane2 = x_room_pair_vec[1].plane.coeffs(); 
    Eigen::Vector4d y_plane1 = y_room_pair_vec[0].plane.coeffs(),  y_plane2 = y_room_pair_vec[1].plane.coeffs();

    if(fabs(x_plane1(3)) > fabs(x_plane2(3))) {
        double size = x_plane1(3) - x_plane2(3);
        room_pose(0) = -1*(((size)/2) + x_plane2(3));
        //room_pose(2) = size;
    } else {
        double size = x_plane2(3) - x_plane1(3);
        room_pose(0) = -1*(((size)/2) + x_plane1(3));
        //room_pose(2) = size;
    }

    if(fabs(y_plane1(3)) > fabs(y_plane2(3))) {
        double size = y_plane1(3) - y_plane2(3);
        room_pose(1) = -1*(((size)/2) + y_plane2(3));
        //room_pose(3) = size;
    } else {
        double size = y_plane2(3) - y_plane1(3);
        room_pose(1) = -1*(((size)/2) + y_plane1(3));
        //room_pose(3) = size;
    }

    return room_pose;
  }

  Eigen::Vector2d compute_room_pose_local(g2o::VertexSE3* keyframe_node, Eigen::Vector2d room_pose) {
    Eigen::Isometry3d room_pose_map; 
    room_pose_map.matrix().block<4,4>(0,0) = Eigen::Matrix4d::Identity(); room_pose_map.matrix().block<2,1>(0,3) = room_pose;  

    Eigen::Isometry3d room_pose_local;
    room_pose_local = room_pose_map * keyframe_node->estimate().inverse();

    return room_pose_local.matrix().block<2,1>(0,3);
  }

  double room_measurement(int plane_type, Eigen::Vector2d room, Eigen::Vector4d plane) {
    double meas;  
    
    if(plane_type == plane_class::Y_VERT_PLANE) {
      if(fabs(room(1)) > fabs(plane(3))) {
        meas =  room(1) - plane(3);
      } else {
        meas =  plane(3) - room(1);
      }
    }

    if(plane_type == plane_class::X_VERT_PLANE) {
      if(fabs(room(0)) > fabs(plane(3))) {
        meas =  room(0) - plane(3);
      } else {
        meas =  plane(3) - room(0);
      }
    }

    return meas;
  }


  std::pair<int,int> associate_rooms(Eigen::Vector2d room_pose) {
    float min_dist = 100;
    std::pair<int,int> data_association; data_association.first = -1; 

    for(int i=0; i< rooms_vec.size(); ++i) {
      float diff_x = room_pose(0) - rooms_vec[i].node->estimate()(0); 
      float diff_y = room_pose(1) - rooms_vec[i].node->estimate()(1); 
      float dist = sqrt(std::pow(diff_x, 2) + std::pow(diff_y, 2));  
      std::cout << "dist room: " << dist << std::endl;

      if(dist < min_dist) {
          min_dist = dist;
          data_association.first = rooms_vec[i].id;
          data_association.second = i;
        }
    }

    std::cout << "min dist: " << min_dist << std::endl;
    if (min_dist > room_dist_threshold) 
      data_association.first = -1;

    return data_association;

  }

  float plane_length(pcl::PointCloud<PointNormal>::Ptr cloud_seg) {
    PointNormal pmin, pmax; pcl::PointXY p1, p2;
    pcl::getMaxSegment(*cloud_seg, pmin, pmax);
    p1.x = pmin.x; p1.y = pmin.y;       
    p2.x = pmax.x; p2.y = pmax.y;
    float length = pcl::euclideanDistance(p1,p2);

    return length;
  } 

  float width_between_planes(Eigen::Vector4d v1, Eigen::Vector4d v2) {
    float size = 0;
    if(fabs(v1(3)) > fabs(v2(3))) 
      size = fabs(v1(3) - v2(3));
    else if (fabs(v2(3)) > fabs(v1(3))) 
      size = fabs(v2(3) - v1(3));  

    return size;  
  }

  void correct_plane_d(int plane_type, g2o::Plane3D& plane1, g2o::Plane3D& plane2) { 
    Eigen::Vector4d coeffs1, coeffs2;
    coeffs1 = plane1.coeffs(); coeffs2 = plane2.coeffs();

    if(plane_type == plane_class::X_VERT_PLANE){
      if(coeffs1(0) < 0) {
        coeffs1(3) = -1*coeffs1(3); 
        plane1 = coeffs1;
      }
      if(coeffs2(0) < 0) {
        coeffs2(3) = -1*coeffs2(3); 
        plane2 = coeffs2;
      }
    }
    if(plane_type == plane_class::Y_VERT_PLANE){
      if(coeffs1(1) < 0) {
        coeffs1(3) = -1*coeffs1(3); 
        plane1 = coeffs1;
      }
      if(coeffs2(1) < 0) {
        coeffs2(3) = -1*coeffs2(3); 
        plane2 = coeffs2;
      }
    }
  }


  /**
   * @brief this method adds all the keyframes in #keyframe_queue to the pose graph (odometry edges)
   * @return if true, at least one keyframe was added to the pose graph
   */
  bool flush_keyframe_queue() {
    std::lock_guard<std::mutex> lock(keyframe_queue_mutex);

    if(keyframe_queue.empty()) {
      return false;
    }

    trans_odom2map_mutex.lock();
    Eigen::Isometry3d odom2map(trans_odom2map.cast<double>());
    trans_odom2map_mutex.unlock();

    int num_processed = 0;
    for(int i = 0; i < std::min<int>(keyframe_queue.size(), max_keyframes_per_update); i++) {
      num_processed = i;

      const auto& keyframe = keyframe_queue[i];
      // new_keyframes will be tested later for loop closure
      new_keyframes.push_back(keyframe);

      // add pose node
      Eigen::Isometry3d odom = odom2map * keyframe->odom;
      keyframe->node = graph_slam->add_se3_node(odom);
      keyframe_hash[keyframe->stamp] = keyframe;

      // fix the first node
      if(keyframes.empty() && new_keyframes.size() == 1) {
        if(private_nh.param<bool>("fix_first_node", false)) {
          Eigen::MatrixXd inf = Eigen::MatrixXd::Identity(6, 6);
          std::stringstream sst(private_nh.param<std::string>("fix_first_node_stddev", "1 1 1 1 1 1"));
          for(int i = 0; i < 6; i++) {
            double stddev = 1.0;
            sst >> stddev;
            inf(i, i) = 1.0 / stddev;
          }

          anchor_node = graph_slam->add_se3_node(Eigen::Isometry3d::Identity());
          anchor_node->setFixed(true);
          anchor_edge = graph_slam->add_se3_edge(anchor_node, keyframe->node, Eigen::Isometry3d::Identity(), inf);
        }
      }

      if(i == 0 && keyframes.empty()) {
        continue;
      }

      // add edge between consecutive keyframes
      const auto& prev_keyframe = i == 0 ? keyframes.back() : keyframe_queue[i - 1];

      Eigen::Isometry3d relative_pose = keyframe->odom.inverse() * prev_keyframe->odom;
      Eigen::MatrixXd information = inf_calclator->calc_information_matrix(keyframe->cloud, prev_keyframe->cloud, relative_pose);
      auto edge = graph_slam->add_se3_edge(keyframe->node, prev_keyframe->node, relative_pose, information);
      graph_slam->add_robust_kernel(edge, private_nh.param<std::string>("odometry_edge_robust_kernel", "NONE"), private_nh.param<double>("odometry_edge_robust_kernel_size", 1.0));
    }

    std_msgs::Header read_until;
    read_until.stamp = keyframe_queue[num_processed]->stamp + ros::Duration(10, 0);
    read_until.frame_id = points_topic;
    read_until_pub.publish(read_until);
    read_until.frame_id = "/filtered_points";
    read_until_pub.publish(read_until);

    keyframe_queue.erase(keyframe_queue.begin(), keyframe_queue.begin() + num_processed + 1);
    return true;
  }

  void nmea_callback(const nmea_msgs::SentenceConstPtr& nmea_msg) {
    GPRMC grmc = nmea_parser->parse(nmea_msg->sentence);

    if(grmc.status != 'A') {
      return;
    }

    geographic_msgs::GeoPointStampedPtr gps_msg(new geographic_msgs::GeoPointStamped());
    gps_msg->header = nmea_msg->header;
    gps_msg->position.latitude = grmc.latitude;
    gps_msg->position.longitude = grmc.longitude;
    gps_msg->position.altitude = NAN;

    gps_callback(gps_msg);
  }

  void navsat_callback(const sensor_msgs::NavSatFixConstPtr& navsat_msg) {
    geographic_msgs::GeoPointStampedPtr gps_msg(new geographic_msgs::GeoPointStamped());
    gps_msg->header = navsat_msg->header;
    gps_msg->position.latitude = navsat_msg->latitude;
    gps_msg->position.longitude = navsat_msg->longitude;
    gps_msg->position.altitude = navsat_msg->altitude;
    gps_callback(gps_msg);
  }

  /**
   * @brief received gps data is added to #gps_queue
   * @param gps_msg
   */
  void gps_callback(const geographic_msgs::GeoPointStampedPtr& gps_msg) {
    std::lock_guard<std::mutex> lock(gps_queue_mutex);
    gps_msg->header.stamp += ros::Duration(gps_time_offset);
    gps_queue.push_back(gps_msg);
  }

  /**
   * @brief
   * @return
   */
  bool flush_gps_queue() {
    std::lock_guard<std::mutex> lock(gps_queue_mutex);

    if(keyframes.empty() || gps_queue.empty()) {
      return false;
    }

    bool updated = false;
    auto gps_cursor = gps_queue.begin();

    for(auto& keyframe : keyframes) {
      if(keyframe->stamp > gps_queue.back()->header.stamp) {
        break;
      }

      if(keyframe->stamp < (*gps_cursor)->header.stamp || keyframe->utm_coord) {
        continue;
      }

      // find the gps data which is closest to the keyframe
      auto closest_gps = gps_cursor;
      for(auto gps = gps_cursor; gps != gps_queue.end(); gps++) {
        auto dt = ((*closest_gps)->header.stamp - keyframe->stamp).toSec();
        auto dt2 = ((*gps)->header.stamp - keyframe->stamp).toSec();
        if(std::abs(dt) < std::abs(dt2)) {
          break;
        }

        closest_gps = gps;
      }

      // if the time residual between the gps and keyframe is too large, skip it
      gps_cursor = closest_gps;
      if(0.2 < std::abs(((*closest_gps)->header.stamp - keyframe->stamp).toSec())) {
        continue;
      }

      // convert (latitude, longitude, altitude) -> (easting, northing, altitude) in UTM coordinate
      geodesy::UTMPoint utm;
      geodesy::fromMsg((*closest_gps)->position, utm);
      Eigen::Vector3d xyz(utm.easting, utm.northing, utm.altitude);

      // the first gps data position will be the origin of the map
      if(!zero_utm) {
        zero_utm = xyz;
      }
      xyz -= (*zero_utm);

      keyframe->utm_coord = xyz;

      g2o::OptimizableGraph::Edge* edge;
      if(std::isnan(xyz.z())) {
        Eigen::Matrix2d information_matrix = Eigen::Matrix2d::Identity() / gps_edge_stddev_xy;
        edge = graph_slam->add_se3_prior_xy_edge(keyframe->node, xyz.head<2>(), information_matrix);
      } else {
        Eigen::Matrix3d information_matrix = Eigen::Matrix3d::Identity();
        information_matrix.block<2, 2>(0, 0) /= gps_edge_stddev_xy;
        information_matrix(2, 2) /= gps_edge_stddev_z;
        edge = graph_slam->add_se3_prior_xyz_edge(keyframe->node, xyz, information_matrix);
      }
      graph_slam->add_robust_kernel(edge, private_nh.param<std::string>("gps_edge_robust_kernel", "NONE"), private_nh.param<double>("gps_edge_robust_kernel_size", 1.0));

      updated = true;
    }

    auto remove_loc = std::upper_bound(gps_queue.begin(), gps_queue.end(), keyframes.back()->stamp, [=](const ros::Time& stamp, const geographic_msgs::GeoPointStampedConstPtr& geopoint) { return stamp < geopoint->header.stamp; });
    gps_queue.erase(gps_queue.begin(), remove_loc);
    return updated;
  }

  void imu_callback(const sensor_msgs::ImuPtr& imu_msg) {
    if(!enable_imu_orientation && !enable_imu_acceleration) {
      return;
    }

    std::lock_guard<std::mutex> lock(imu_queue_mutex);
    imu_msg->header.stamp += ros::Duration(imu_time_offset);
    imu_queue.push_back(imu_msg);
  }

  bool flush_imu_queue() {
    std::lock_guard<std::mutex> lock(imu_queue_mutex);
    if(keyframes.empty() || imu_queue.empty() || base_frame_id.empty()) {
      return false;
    }

    bool updated = false;
    auto imu_cursor = imu_queue.begin();

    for(auto& keyframe : keyframes) {
      if(keyframe->stamp > imu_queue.back()->header.stamp) {
        break;
      }

      if(keyframe->stamp < (*imu_cursor)->header.stamp || keyframe->acceleration) {
        continue;
      }

      // find imu data which is closest to the keyframe
      auto closest_imu = imu_cursor;
      for(auto imu = imu_cursor; imu != imu_queue.end(); imu++) {
        auto dt = ((*closest_imu)->header.stamp - keyframe->stamp).toSec();
        auto dt2 = ((*imu)->header.stamp - keyframe->stamp).toSec();
        if(std::abs(dt) < std::abs(dt2)) {
          break;
        }

        closest_imu = imu;
      }

      imu_cursor = closest_imu;
      if(0.2 < std::abs(((*closest_imu)->header.stamp - keyframe->stamp).toSec())) {
        continue;
      }

      const auto& imu_ori = (*closest_imu)->orientation;
      const auto& imu_acc = (*closest_imu)->linear_acceleration;

      geometry_msgs::Vector3Stamped acc_imu;
      geometry_msgs::Vector3Stamped acc_base;
      geometry_msgs::QuaternionStamped quat_imu;
      geometry_msgs::QuaternionStamped quat_base;

      quat_imu.header.frame_id = acc_imu.header.frame_id = (*closest_imu)->header.frame_id;
      quat_imu.header.stamp = acc_imu.header.stamp = ros::Time(0);
      acc_imu.vector = (*closest_imu)->linear_acceleration;
      quat_imu.quaternion = (*closest_imu)->orientation;

      try {
        tf_listener.transformVector(base_frame_id, acc_imu, acc_base);
        tf_listener.transformQuaternion(base_frame_id, quat_imu, quat_base);
      } catch(std::exception& e) {
        std::cerr << "failed to find transform!!" << std::endl;
        return false;
      }

      keyframe->acceleration = Eigen::Vector3d(acc_base.vector.x, acc_base.vector.y, acc_base.vector.z);
      keyframe->orientation = Eigen::Quaterniond(quat_base.quaternion.w, quat_base.quaternion.x, quat_base.quaternion.y, quat_base.quaternion.z);
      keyframe->orientation = keyframe->orientation;
      if(keyframe->orientation->w() < 0.0) {
        keyframe->orientation->coeffs() = -keyframe->orientation->coeffs();
      }

      if(enable_imu_orientation) {
        Eigen::MatrixXd info = Eigen::MatrixXd::Identity(3, 3) / imu_orientation_edge_stddev;
        auto edge = graph_slam->add_se3_prior_quat_edge(keyframe->node, *keyframe->orientation, info);
        graph_slam->add_robust_kernel(edge, private_nh.param<std::string>("imu_orientation_edge_robust_kernel", "NONE"), private_nh.param<double>("imu_orientation_edge_robust_kernel_size", 1.0));
      }

      if(enable_imu_acceleration) {
        Eigen::MatrixXd info = Eigen::MatrixXd::Identity(3, 3) / imu_acceleration_edge_stddev;
        g2o::OptimizableGraph::Edge* edge = graph_slam->add_se3_prior_vec_edge(keyframe->node, -Eigen::Vector3d::UnitZ(), *keyframe->acceleration, info);
        graph_slam->add_robust_kernel(edge, private_nh.param<std::string>("imu_acceleration_edge_robust_kernel", "NONE"), private_nh.param<double>("imu_acceleration_edge_robust_kernel_size", 1.0));
      }
      updated = true;
    }

    auto remove_loc = std::upper_bound(imu_queue.begin(), imu_queue.end(), keyframes.back()->stamp, [=](const ros::Time& stamp, const sensor_msgs::ImuConstPtr& imu) { return stamp < imu->header.stamp; });
    imu_queue.erase(imu_queue.begin(), remove_loc);

    return updated;
  }

  /**
   * @brief received floor coefficients are added to #floor_coeffs_queue
   * @param floor_coeffs_msg
   */
  void floor_coeffs_callback(const hdl_graph_slam::FloorCoeffsConstPtr& floor_coeffs_msg) {
    if(floor_coeffs_msg->coeffs.empty()) {
      return;
    }

    std::lock_guard<std::mutex> lock(floor_coeffs_queue_mutex);
    floor_coeffs_queue.push_back(floor_coeffs_msg);
  }

  /**
   * @brief this methods associates floor coefficients messages with registered keyframes, and then adds the associated coeffs to the pose graph
   * @return if true, at least one floor plane edge is added to the pose graph
   */
  bool flush_floor_queue() {
    std::lock_guard<std::mutex> lock(floor_coeffs_queue_mutex);

    if(keyframes.empty()) {
      return false;
    }

    const auto& latest_keyframe_stamp = keyframes.back()->stamp;

    bool updated = false;
    for(const auto& floor_coeffs : floor_coeffs_queue) {
      if(floor_coeffs->header.stamp > latest_keyframe_stamp) {
        break;
      }

      auto found = keyframe_hash.find(floor_coeffs->header.stamp);
      if(found == keyframe_hash.end()) {
        continue;
      }

      if(!floor_plane_node) {
        floor_plane_node = graph_slam->add_plane_node(Eigen::Vector4d(0.0, 0.0, 1.0, 0.0));
        floor_plane_node->setFixed(true);
      }

      const auto& keyframe = found->second;

      Eigen::Vector4d coeffs(floor_coeffs->coeffs[0], floor_coeffs->coeffs[1], floor_coeffs->coeffs[2], floor_coeffs->coeffs[3]);
      Eigen::Matrix3d information = Eigen::Matrix3d::Identity() * (1.0 / floor_edge_stddev);
      auto edge = graph_slam->add_se3_plane_edge(keyframe->node, floor_plane_node, coeffs, information);
      graph_slam->add_robust_kernel(edge, private_nh.param<std::string>("floor_edge_robust_kernel", "NONE"), private_nh.param<double>("floor_edge_robust_kernel_size", 1.0));

      keyframe->floor_coeffs = coeffs;

      updated = true;
    }

    auto remove_loc = std::upper_bound(floor_coeffs_queue.begin(), floor_coeffs_queue.end(), latest_keyframe_stamp, [=](const ros::Time& stamp, const hdl_graph_slam::FloorCoeffsConstPtr& coeffs) { return stamp < coeffs->header.stamp; });
    floor_coeffs_queue.erase(floor_coeffs_queue.begin(), remove_loc);

    return updated;
  }

  /**
   * @brief generate map point cloud and publish it
   * @param event
   */
  void map_points_publish_timer_callback(const ros::WallTimerEvent& event) {
    if(!map_points_pub.getNumSubscribers() || !graph_updated) {
      return;
    }

    std::vector<KeyFrameSnapshot::Ptr> snapshot;

    keyframes_snapshot_mutex.lock();
    snapshot = keyframes_snapshot;
    keyframes_snapshot_mutex.unlock();

    auto cloud = map_cloud_generator->generate(snapshot, map_cloud_resolution);
    if(!cloud) {
      return;
    }

    cloud->header.frame_id = map_frame_id;
    cloud->header.stamp = snapshot.back()->cloud->header.stamp;

    sensor_msgs::PointCloud2Ptr cloud_msg(new sensor_msgs::PointCloud2());
    pcl::toROSMsg(*cloud, *cloud_msg);

    convert_plane_points_to_map();
    auto markers = create_marker_array(ros::Time::now());
    markers_pub.publish(markers);
    
    map_points_pub.publish(cloud_msg);
  }

  /**
   * @brief this methods adds all the data in the queues to the pose graph, and then optimizes the pose graph
   * @param event
   */
  void optimization_timer_callback(const ros::WallTimerEvent& event) {
    std::lock_guard<std::mutex> lock(main_thread_mutex);

    // add keyframes and floor coeffs in the queues to the pose graph
    bool keyframe_updated = flush_keyframe_queue();

    if(!keyframe_updated) {
      std_msgs::Header read_until;
      read_until.stamp = ros::Time::now() + ros::Duration(30, 0);
      read_until.frame_id = points_topic;
      read_until_pub.publish(read_until);
      read_until.frame_id = "/filtered_points";
      read_until_pub.publish(read_until);
    }

    if(!keyframe_updated & !flush_floor_queue() & !flush_gps_queue() & !flush_imu_queue() & !flush_clouds_seg_queue()) {
      return;
    }

    // loop detection
    std::vector<Loop::Ptr> loops = loop_detector->detect(keyframes, new_keyframes, *graph_slam);
    for(const auto& loop : loops) {
      Eigen::Isometry3d relpose(loop->relative_pose.cast<double>());
      Eigen::MatrixXd information_matrix = inf_calclator->calc_information_matrix(loop->key1->cloud, loop->key2->cloud, relpose);
      auto edge = graph_slam->add_se3_edge(loop->key1->node, loop->key2->node, relpose, information_matrix);
      graph_slam->add_robust_kernel(edge, private_nh.param<std::string>("loop_closure_edge_robust_kernel", "NONE"), private_nh.param<double>("loop_closure_edge_robust_kernel_size", 1.0));
    }

    std::copy(new_keyframes.begin(), new_keyframes.end(), std::back_inserter(keyframes));
    new_keyframes.clear();

    // move the first node anchor position to the current estimate of the first node pose
    // so the first node moves freely while trying to stay around the origin
    if(anchor_node && private_nh.param<bool>("fix_first_node_adaptive", true)) {
      Eigen::Isometry3d anchor_target = static_cast<g2o::VertexSE3*>(anchor_edge->vertices()[1])->estimate();
      anchor_node->setEstimate(anchor_target);
    }

    // optimize the pose graph
    int num_iterations = private_nh.param<int>("g2o_solver_num_iterations", 1024);
    if((graph_slam->optimize(num_iterations)) > 0)
      compute_plane_cov();

    // publish tf
    const auto& keyframe = keyframes.back();
    Eigen::Isometry3d trans = keyframe->node->estimate() * keyframe->odom.inverse();
    trans_odom2map_mutex.lock();
    trans_odom2map = trans.matrix().cast<float>();
    trans_odom2map_mutex.unlock();

    std::vector<KeyFrameSnapshot::Ptr> snapshot(keyframes.size());
    std::transform(keyframes.begin(), keyframes.end(), snapshot.begin(), [=](const KeyFrame::Ptr& k) { return std::make_shared<KeyFrameSnapshot>(k); });

    keyframes_snapshot_mutex.lock();
    keyframes_snapshot.swap(snapshot);
    keyframes_snapshot_mutex.unlock();
    graph_updated = true;

    geometry_msgs::TransformStamped ts = matrix2transform(keyframe->stamp, trans.matrix().cast<float>(), map_frame_id, odom_frame_id);
    odom2map_pub.publish(ts);

  }

  /**  
  * @brief compute the plane covariances
  */
  void compute_plane_cov() {
    g2o::SparseBlockMatrix<Eigen::MatrixXd> plane_spinv_vec;
    std::vector<std::pair<int, int>> plane_pairs_vec;
    for (int i = 0; i < x_vert_planes.size(); ++i) {
      x_vert_planes[i].plane_node->unlockQuadraticForm();
      plane_pairs_vec.push_back(std::make_pair(x_vert_planes[i].plane_node->hessianIndex(), x_vert_planes[i].plane_node->hessianIndex()));
    }
    for (int i = 0; i < y_vert_planes.size(); ++i) {
      y_vert_planes[i].plane_node->unlockQuadraticForm();
      plane_pairs_vec.push_back(std::make_pair(y_vert_planes[i].plane_node->hessianIndex(), y_vert_planes[i].plane_node->hessianIndex()));
    }
    for (int i = 0; i < hort_planes.size(); ++i) {
      hort_planes[i].plane_node->unlockQuadraticForm();
      plane_pairs_vec.push_back(std::make_pair(hort_planes[i].plane_node->hessianIndex(), hort_planes[i].plane_node->hessianIndex()));
    }
    

    if(!plane_pairs_vec.empty()){
      if (graph_slam->compute_landmark_marginals(plane_spinv_vec, plane_pairs_vec)) {
        int i=0;
        while (i < x_vert_planes.size()) {
          //std::cout << "covariance of x plane " << i << " " << y_vert_planes[i].covariance << std::endl;
          x_vert_planes[i].covariance = plane_spinv_vec.block(x_vert_planes[i].plane_node->hessianIndex(), x_vert_planes[i].plane_node->hessianIndex())->eval().cast<double>();
          Eigen::LLT<Eigen::MatrixXd> lltOfCov(x_vert_planes[i].covariance);
          if(lltOfCov.info() == Eigen::NumericalIssue) {
              //std::cout << "covariance of x plane not PSD" << i << " " << x_vert_planes[i].covariance << std::endl;
              x_vert_planes[i].covariance = Eigen::Matrix3d::Identity();
          }
          i++; 
        }
        i=0;
        while (i < y_vert_planes.size()) {
          y_vert_planes[i].covariance = plane_spinv_vec.block(y_vert_planes[i].plane_node->hessianIndex(), y_vert_planes[i].plane_node->hessianIndex())->eval().cast<double>();
          //std::cout << "covariance of y plane " << i << " " << y_vert_planes[i].covariance << std::endl;
          Eigen::LLT<Eigen::MatrixXd> lltOfCov(y_vert_planes[i].covariance);
          if(lltOfCov.info() == Eigen::NumericalIssue) {
              //std::cout << "covariance of y plane not PSD " << i << " " << y_vert_planes[i].covariance << std::endl;
              y_vert_planes[i].covariance = Eigen::Matrix3d::Identity();
          }
          i++;
       }
       i=0;
       while (i < hort_planes.size()) {
          hort_planes[i].covariance = plane_spinv_vec.block(hort_planes[i].plane_node->hessianIndex(), hort_planes[i].plane_node->hessianIndex())->eval().cast<double>();
          //std::cout << "covariance of y plane " << i << " " << hort_planes[i].covariance << std::endl;
          Eigen::LLT<Eigen::MatrixXd> lltOfCov(hort_planes[i].covariance);
          if(lltOfCov.info() == Eigen::NumericalIssue) {
              //std::cout << "covariance of y plane not PSD " << i << " " << hort_planes[i].covariance << std::endl;
              hort_planes[i].covariance = Eigen::Matrix3d::Identity();
          }
          i++;
       }

      }
    }
  }

  /** 
  * @brief convert the body points of planes to map frame for mapping 
  */ 
  void convert_plane_points_to_map() {

    for(int i = 0; i < x_vert_planes.size(); ++i) {
      Eigen::Matrix4f pose = x_vert_planes[i].keyframe_node->estimate().matrix().cast<float>();
      pcl::PointCloud<PointNormal>::Ptr cloud_seg_map(new pcl::PointCloud<PointNormal>());
      
      for(size_t j=0; j < x_vert_planes[i].cloud_seg_body->points.size(); ++j) {
        PointNormal dst_pt;
        dst_pt.getVector4fMap() = pose * x_vert_planes[i].cloud_seg_body->points[j].getVector4fMap();
        cloud_seg_map->points.push_back(dst_pt);
        x_vert_planes[i].cloud_seg_map = cloud_seg_map;
      }
    }

    for(int i = 0; i < y_vert_planes.size(); ++i) {
      Eigen::Matrix4f pose = y_vert_planes[i].keyframe_node->estimate().matrix().cast<float>();
      pcl::PointCloud<PointNormal>::Ptr cloud_seg_map(new pcl::PointCloud<PointNormal>());
      
      for(size_t j=0; j < y_vert_planes[i].cloud_seg_body->points.size(); ++j) {
        PointNormal dst_pt;
        dst_pt.getVector4fMap() = pose * y_vert_planes[i].cloud_seg_body->points[j].getVector4fMap();
        cloud_seg_map->points.push_back(dst_pt);
        y_vert_planes[i].cloud_seg_map = cloud_seg_map;
      }
    }

    for(int i = 0; i < hort_planes.size(); ++i) {
      Eigen::Matrix4f pose = hort_planes[i].keyframe_node->estimate().matrix().cast<float>();
      pcl::PointCloud<PointNormal>::Ptr cloud_seg_map(new pcl::PointCloud<PointNormal>());
      
      for(size_t j=0; j < hort_planes[i].cloud_seg_body->points.size(); ++j) {
        PointNormal dst_pt;
        dst_pt.getVector4fMap() = pose * hort_planes[i].cloud_seg_body->points[j].getVector4fMap();
        cloud_seg_map->points.push_back(dst_pt);
        hort_planes[i].cloud_seg_map = cloud_seg_map;
      }
    }
  }

  /**
   * @brief create visualization marker
   * @param stamp
   * @return
   */
  visualization_msgs::MarkerArray create_marker_array(const ros::Time& stamp) const {
    visualization_msgs::MarkerArray markers;
    //markers.markers.resize(11);

    // node markers
    visualization_msgs::Marker traj_marker;
    traj_marker.header.frame_id = map_frame_id;
    traj_marker.header.stamp = stamp;
    traj_marker.ns = "nodes";
    traj_marker.id = markers.markers.size();
    traj_marker.type = visualization_msgs::Marker::SPHERE_LIST;

    traj_marker.pose.orientation.w = 1.0;
    traj_marker.scale.x = traj_marker.scale.y = traj_marker.scale.z = 0.5;

    visualization_msgs::Marker imu_marker;
    imu_marker.header = traj_marker.header;
    imu_marker.ns = "imu";
    imu_marker.id = markers.markers.size()+1;
    imu_marker.type = visualization_msgs::Marker::SPHERE_LIST;

    imu_marker.pose.orientation.w = 1.0;
    imu_marker.scale.x = imu_marker.scale.y = imu_marker.scale.z = 0.75;

    traj_marker.points.resize(keyframes.size());
    traj_marker.colors.resize(keyframes.size());
    for(int i = 0; i < keyframes.size(); i++) {
      Eigen::Vector3d pos = keyframes[i]->node->estimate().translation();
      traj_marker.points[i].x = pos.x();
      traj_marker.points[i].y = pos.y();
      traj_marker.points[i].z = pos.z();

      double p = static_cast<double>(i) / keyframes.size();
      traj_marker.colors[i].r = 1.0 - p;
      traj_marker.colors[i].g = p;
      traj_marker.colors[i].b = 0.0;
      traj_marker.colors[i].a = 1.0;

      if(keyframes[i]->acceleration) {
        Eigen::Vector3d pos = keyframes[i]->node->estimate().translation();
        geometry_msgs::Point point;
        point.x = pos.x();
        point.y = pos.y();
        point.z = pos.z();

        std_msgs::ColorRGBA color;
        color.r = 0.0;
        color.g = 0.0;
        color.b = 1.0;
        color.a = 0.1;

        imu_marker.points.push_back(point);
        imu_marker.colors.push_back(color);
      }
    }
    markers.markers.push_back(traj_marker); 
    markers.markers.push_back(imu_marker); 

    // edge markers
    visualization_msgs::Marker edge_marker;
    edge_marker.header.frame_id = map_frame_id;
    edge_marker.header.stamp = stamp;
    edge_marker.ns = "edges";
    edge_marker.id = markers.markers.size();
    edge_marker.type = visualization_msgs::Marker::LINE_LIST;

    edge_marker.pose.orientation.w = 1.0;
    edge_marker.scale.x = 0.05;

    edge_marker.points.resize(graph_slam->graph->edges().size() * 4);
    edge_marker.colors.resize(graph_slam->graph->edges().size() * 4);

    auto edge_itr = graph_slam->graph->edges().begin();
    for(int i = 0; edge_itr != graph_slam->graph->edges().end(); edge_itr++, i++) {
      g2o::HyperGraph::Edge* edge = *edge_itr;
      g2o::EdgeSE3* edge_se3 = dynamic_cast<g2o::EdgeSE3*>(edge);
      if(edge_se3) {
        g2o::VertexSE3* v1 = dynamic_cast<g2o::VertexSE3*>(edge_se3->vertices()[0]);
        g2o::VertexSE3* v2 = dynamic_cast<g2o::VertexSE3*>(edge_se3->vertices()[1]);
        Eigen::Vector3d pt1 = v1->estimate().translation();
        Eigen::Vector3d pt2 = v2->estimate().translation();

        edge_marker.points[i * 2].x = pt1.x();
        edge_marker.points[i * 2].y = pt1.y();
        edge_marker.points[i * 2].z = pt1.z();
        edge_marker.points[i * 2 + 1].x = pt2.x();
        edge_marker.points[i * 2 + 1].y = pt2.y();
        edge_marker.points[i * 2 + 1].z = pt2.z();

        double p1 = static_cast<double>(v1->id()) / graph_slam->graph->vertices().size();
        double p2 = static_cast<double>(v2->id()) / graph_slam->graph->vertices().size();
        edge_marker.colors[i * 2].r = 1.0 - p1;
        edge_marker.colors[i * 2].g = p1;
        edge_marker.colors[i * 2].a = 1.0;
        edge_marker.colors[i * 2 + 1].r = 1.0 - p2;
        edge_marker.colors[i * 2 + 1].g = p2;
        edge_marker.colors[i * 2 + 1].a = 1.0;

        if(std::abs(v1->id() - v2->id()) > 2) {
          edge_marker.points[i * 2].z += 0.5;
          edge_marker.points[i * 2 + 1].z += 0.5;
        }

        continue;
      }

      g2o::EdgeSE3Plane* edge_plane = dynamic_cast<g2o::EdgeSE3Plane*>(edge);
      if(edge_plane) {
        g2o::VertexSE3* v1 = dynamic_cast<g2o::VertexSE3*>(edge_plane->vertices()[0]);
        g2o::VertexPlane* v2 = dynamic_cast<g2o::VertexPlane*>(edge_plane->vertices()[1]);
        Eigen::Vector3d pt1 = v1->estimate().translation();
        Eigen::Vector3d pt2, pt3;

        float r=0, g=0, b=0.0;
        double x=0, y=0;
        if (fabs(v2->estimate().normal()(0)) > 0.95) {
          for(auto x_plane : x_vert_planes) {
            if (x_plane.id == v2->id()) {
              x = x_plane.cloud_seg_map->points[(x_plane.cloud_seg_map->points.size()/2)].x;
              y = x_plane.cloud_seg_map->points[(x_plane.cloud_seg_map->points.size()/2)].y;
            } 
          }
          pt2 = Eigen::Vector3d(pt1.x(), pt1.y(), 3.0);
          pt3 = Eigen::Vector3d(x, y, 5.0);
          r=1.0;
        } 
        else if (fabs(v2->estimate().normal()(1)) > 0.95) {
           for(auto y_plane : y_vert_planes) {
            if (y_plane.id == v2->id()) {
              x = y_plane.cloud_seg_map->points[(y_plane.cloud_seg_map->points.size()/2)].x;
              y = y_plane.cloud_seg_map->points[(y_plane.cloud_seg_map->points.size()/2)].y;
            } 
          }
          pt2 = Eigen::Vector3d(pt1.x(), pt1.y(), 3.0);
          pt3 = Eigen::Vector3d(x, y, 5.0);
          b=1.0;
        }
        else if (fabs(v2->estimate().normal()(2)) > 0.95) {
           for(auto h_plane : hort_planes) {
            if (h_plane.id == v2->id()) {
              x = h_plane.cloud_seg_map->points[(h_plane.cloud_seg_map->points.size()/2)].x;
              y = h_plane.cloud_seg_map->points[(h_plane.cloud_seg_map->points.size()/2)].y;
            }
          }   
          pt2 = Eigen::Vector3d(pt1.x(), pt1.y(), 3.0); 
          pt3 = Eigen::Vector3d(x, y, 5.0); 
          r=1; g=0.65;
        }

        edge_marker.points[i * 2].x = pt1.x();
        edge_marker.points[i * 2].y = pt1.y();
        edge_marker.points[i * 2].z = pt1.z();
        edge_marker.points[i * 2 + 1].x = pt2.x();
        edge_marker.points[i * 2 + 1].y = pt2.y();
        edge_marker.points[i * 2 + 1].z = pt2.z();

        edge_marker.colors[i * 2].r = 0;
        edge_marker.colors[i * 2].g = 0;
        edge_marker.colors[i * 2].b = 0.0;
        edge_marker.colors[i * 2].a = 1.0;
        edge_marker.colors[i * 2 + 1].r = 0;
        edge_marker.colors[i * 2 + 1].g = 0;
        edge_marker.colors[i * 2 + 1].b = 0;
        edge_marker.colors[i * 2 + 1].a = 1;

        edge_marker.points[i * 2+2].x = pt2.x();
        edge_marker.points[i * 2+2].y = pt2.y();
        edge_marker.points[i * 2+2].z = pt2.z();
        edge_marker.points[i * 2 + 3].x = pt3.x();
        edge_marker.points[i * 2 + 3].y = pt3.y();
        edge_marker.points[i * 2 + 3].z = pt3.z();

        edge_marker.colors[i * 2+2].r = r;
        edge_marker.colors[i * 2+2].g = g;
        edge_marker.colors[i * 2+2].b = b;
        edge_marker.colors[i * 2+2].a = 1.0;
        edge_marker.colors[i * 2 + 3].r = r;
        edge_marker.colors[i * 2 + 3].g = g;
        edge_marker.colors[i * 2 + 3].b = b;
        edge_marker.colors[i * 2 + 3].a = 0.5;

        continue;
      }

      g2o::EdgeSE3PointToPlane* edge_point_to_plane = dynamic_cast<g2o::EdgeSE3PointToPlane*>(edge);
      if(edge_point_to_plane) {
        g2o::VertexSE3* v1 = dynamic_cast<g2o::VertexSE3*>(edge_point_to_plane->vertices()[0]);
        g2o::VertexPlane* v2 = dynamic_cast<g2o::VertexPlane*>(edge_point_to_plane->vertices()[1]);
        Eigen::Vector3d pt1 = v1->estimate().translation();
        Eigen::Vector3d pt2;
        float r=0, g=0, b=0.0;
        double x=0, y=0;
        if (fabs(v2->estimate().normal()(0)) > 0.95) {
          for(auto x_plane : x_vert_planes) {
            if (x_plane.id == v2->id()) {
              x = x_plane.cloud_seg_map->points[(x_plane.cloud_seg_map->points.size()/2)].x;
              y = x_plane.cloud_seg_map->points[(x_plane.cloud_seg_map->points.size()/2)].y;
            } 
          }
          pt2 = Eigen::Vector3d(x, y, 5.0);
          r=1.0;
        } 
        else if (fabs(v2->estimate().normal()(1)) > 0.95) {
           for(auto y_plane : y_vert_planes) {
            if (y_plane.id == v2->id()) {
              x = y_plane.cloud_seg_map->points[(y_plane.cloud_seg_map->points.size()/2)].x;
              y = y_plane.cloud_seg_map->points[(y_plane.cloud_seg_map->points.size()/2)].y;
            } 
          }
          pt2 = Eigen::Vector3d(x, y, 5.0);
          b=1.0;
        }
        else if (fabs(v2->estimate().normal()(2)) > 0.95) {
           for(auto h_plane : hort_planes) {
            if (h_plane.id == v2->id()) {
              x = h_plane.cloud_seg_map->points[(h_plane.cloud_seg_map->points.size()/2)].x;
              y = h_plane.cloud_seg_map->points[(h_plane.cloud_seg_map->points.size()/2)].y;
            }
          } 
          pt2 = Eigen::Vector3d(x, y, 5.0);  
          r=1; g=0.65;
        }

        edge_marker.points[i * 2].x = pt1.x();
        edge_marker.points[i * 2].y = pt1.y();
        edge_marker.points[i * 2].z = pt1.z();
        edge_marker.points[i * 2 + 1].x = pt2.x();
        edge_marker.points[i * 2 + 1].y = pt2.y();
        edge_marker.points[i * 2 + 1].z = pt2.z();

        edge_marker.colors[i * 2].r = r;
        edge_marker.colors[i * 2].g = g;
        edge_marker.colors[i * 2].b = b;
        edge_marker.colors[i * 2].a = 1.0;
        edge_marker.colors[i * 2 + 1].r = r;
        edge_marker.colors[i * 2 + 1].g = g;
        edge_marker.colors[i * 2 + 1].b = b;
        edge_marker.colors[i * 2 + 1].a = 1.0;

        continue;
      }

      // g2o::EdgePlaneParallel* edge_parallel_plane = dynamic_cast<g2o::EdgePlaneParallel*>(edge);  
      // if(edge_parallel_plane) {
      //   g2o::VertexPlane* v1 = dynamic_cast<g2o::VertexPlane*>(edge_parallel_plane->vertices()[0]);
      //   g2o::VertexPlane* v2 = dynamic_cast<g2o::VertexPlane*>(edge_parallel_plane->vertices()[1]);
      //   Eigen::Vector3d pt1(0,0,0), pt2(0,0,0);
      //   float r=0, g=0, b=0.0;
      //   double x1=0, y1=0, x2 =0, y2=0;
      //   if (fabs(v2->estimate().normal()(0)) > 0.95) {
      //     for(auto x_plane : x_vert_planes) {
      //       if (x_plane.id == v1->id()) {
      //         x1 = x_plane.cloud_seg_map->points[(x_plane.cloud_seg_map->points.size()/2)].x;
      //         y1 = x_plane.cloud_seg_map->points[(x_plane.cloud_seg_map->points.size()/2)].y;
      //       } else if(x_plane.id == v2->id()) {
      //         x2 = x_plane.cloud_seg_map->points[(x_plane.cloud_seg_map->points.size()/2)].x;
      //         y2 = x_plane.cloud_seg_map->points[(x_plane.cloud_seg_map->points.size()/2)].y;
      //       } 
      //     }
      //     pt1 = Eigen::Vector3d(x1, y1, 10.0);
      //     pt2 = Eigen::Vector3d(x2, y2, 10.0);
      //   }
      //   if (fabs(v2->estimate().normal()(1)) > 0.95) {
      //     for(auto y_plane : y_vert_planes) {
      //       if (y_plane.id == v1->id()) {
      //         x1 = y_plane.cloud_seg_map->points[(y_plane.cloud_seg_map->points.size()/2)].x;
      //         y1 = y_plane.cloud_seg_map->points[(y_plane.cloud_seg_map->points.size()/2)].y;
      //       } else if(y_plane.id == v2->id()) {
      //         x2 = y_plane.cloud_seg_map->points[(y_plane.cloud_seg_map->points.size()/2)].x;
      //         y2 = y_plane.cloud_seg_map->points[(y_plane.cloud_seg_map->points.size()/2)].y;
      //       } 
      //     }
      //     pt1 = Eigen::Vector3d(x1, y1, 10.0);
      //     pt2 = Eigen::Vector3d(x2, y2, 10.0);
      //   }
        
      //   edge_marker.points[i * 2].x = pt1.x();
      //   edge_marker.points[i * 2].y = pt1.y();
      //   edge_marker.points[i * 2].z = pt1.z();
      //   edge_marker.points[i * 2 + 1].x = pt2.x();
      //   edge_marker.points[i * 2 + 1].y = pt2.y();
      //   edge_marker.points[i * 2 + 1].z = pt2.z();

      //   edge_marker.colors[i * 2].r = r;
      //   edge_marker.colors[i * 2].g = g;
      //   edge_marker.colors[i * 2].b = b;
      //   edge_marker.colors[i * 2].a = 1.0;
      //   edge_marker.colors[i * 2 + 1].r = r;
      //   edge_marker.colors[i * 2 + 1].g = g;
      //   edge_marker.colors[i * 2 + 1].b = b;
      //   edge_marker.colors[i * 2 + 1].a = 1.0;

      //   continue;
      // }

      g2o::EdgeSE3PriorXY* edge_priori_xy = dynamic_cast<g2o::EdgeSE3PriorXY*>(edge);
      if(edge_priori_xy) {
        g2o::VertexSE3* v1 = dynamic_cast<g2o::VertexSE3*>(edge_priori_xy->vertices()[0]);
        Eigen::Vector3d pt1 = v1->estimate().translation();
        Eigen::Vector3d pt2 = Eigen::Vector3d::Zero();
        pt2.head<2>() = edge_priori_xy->measurement();

        edge_marker.points[i * 2].x = pt1.x();
        edge_marker.points[i * 2].y = pt1.y();
        edge_marker.points[i * 2].z = pt1.z() + 0.5;
        edge_marker.points[i * 2 + 1].x = pt2.x();
        edge_marker.points[i * 2 + 1].y = pt2.y();
        edge_marker.points[i * 2 + 1].z = pt2.z() + 0.5;

        edge_marker.colors[i * 2].r = 1.0;
        edge_marker.colors[i * 2].a = 1.0;
        edge_marker.colors[i * 2 + 1].r = 1.0;
        edge_marker.colors[i * 2 + 1].a = 1.0;

        continue;
      }
      g2o::EdgeSE3PriorXYZ* edge_priori_xyz = dynamic_cast<g2o::EdgeSE3PriorXYZ*>(edge);
      if(edge_priori_xyz) {
        g2o::VertexSE3* v1 = dynamic_cast<g2o::VertexSE3*>(edge_priori_xyz->vertices()[0]);
        Eigen::Vector3d pt1 = v1->estimate().translation();
        Eigen::Vector3d pt2 = edge_priori_xyz->measurement();

        edge_marker.points[i * 2].x = pt1.x();
        edge_marker.points[i * 2].y = pt1.y();
        edge_marker.points[i * 2].z = pt1.z() + 0.5;
        edge_marker.points[i * 2 + 1].x = pt2.x();
        edge_marker.points[i * 2 + 1].y = pt2.y();
        edge_marker.points[i * 2 + 1].z = pt2.z();

        edge_marker.colors[i * 2].r = 1.0;
        edge_marker.colors[i * 2].a = 1.0;
        edge_marker.colors[i * 2 + 1].r = 1.0;
        edge_marker.colors[i * 2 + 1].a = 1.0;

        continue;
      }
    }
    markers.markers.push_back(edge_marker); 

    // sphere
    visualization_msgs::Marker sphere_marker;
    sphere_marker.header.frame_id = map_frame_id;
    sphere_marker.header.stamp = stamp;
    sphere_marker.ns = "loop_close_radius";
    sphere_marker.id = markers.markers.size();
    sphere_marker.type = visualization_msgs::Marker::SPHERE;

    if(!keyframes.empty()) {
      Eigen::Vector3d pos = keyframes.back()->node->estimate().translation();
      sphere_marker.pose.position.x = pos.x();
      sphere_marker.pose.position.y = pos.y();
      sphere_marker.pose.position.z = pos.z();
    }
    sphere_marker.pose.orientation.w = 1.0;
    sphere_marker.scale.x = sphere_marker.scale.y = sphere_marker.scale.z = loop_detector->get_distance_thresh() * 2.0;

    sphere_marker.color.r = 1.0;
    sphere_marker.color.a = 0.3;
    markers.markers.push_back(sphere_marker); 

    //x vertical plane markers 
    visualization_msgs::Marker x_vert_plane_marker;
    x_vert_plane_marker.pose.orientation.w = 1.0;
    x_vert_plane_marker.scale.x = 0.05;
    x_vert_plane_marker.scale.y = 0.05;
    x_vert_plane_marker.scale.z = 0.05;
    //plane_marker.points.resize(vert_planes.size());    
    x_vert_plane_marker.header.frame_id = map_frame_id;
    x_vert_plane_marker.header.stamp = stamp;
    x_vert_plane_marker.ns = "x_vert_planes";
    x_vert_plane_marker.id = markers.markers.size();
    x_vert_plane_marker.type = visualization_msgs::Marker::CUBE_LIST;

    for(int i = 0; i < x_vert_planes.size(); ++i) {
      for(size_t j=0; j < x_vert_planes[i].cloud_seg_map->size(); ++j) {
        geometry_msgs::Point point;
        point.x = x_vert_planes[i].cloud_seg_map->points[j].x;
        point.y = x_vert_planes[i].cloud_seg_map->points[j].y;
        point.z = x_vert_planes[i].cloud_seg_map->points[j].z + 5.0;
        x_vert_plane_marker.points.push_back(point);
      }
      x_vert_plane_marker.color.r = 1;
      x_vert_plane_marker.color.a = 1;
    }
    markers.markers.push_back(x_vert_plane_marker); 


    //x parallel plane markers 
    // visualization_msgs::Marker x_parallel_plane_marker;
    // x_parallel_plane_marker.pose.orientation.w = 1.0;
    // x_parallel_plane_marker.scale.x = 0.05;
    // x_parallel_plane_marker.scale.y = 2;
    // x_parallel_plane_marker.scale.z = 1.0;
    // //plane_marker.points.resize(vert_planes.size());    
    // x_parallel_plane_marker.header.frame_id = map_frame_id;
    // x_parallel_plane_marker.header.stamp = stamp;
    // x_parallel_plane_marker.ns = "x_parallel_planes";
    // x_parallel_plane_marker.id = markers.markers.size();
    // x_parallel_plane_marker.type = visualization_msgs::Marker::CUBE_LIST;
    // for(int i = 0; i < x_vert_planes.size(); ++i) {
    //   if (x_vert_planes[i].parallel_pair) {
    //     geometry_msgs::Point parallel_plane_point;
    //     parallel_plane_point.x = x_vert_planes[i].cloud_seg_map->points[(x_vert_planes[i].cloud_seg_map->points.size()/2)].x;
    //     parallel_plane_point.y = x_vert_planes[i].cloud_seg_map->points[(x_vert_planes[i].cloud_seg_map->points.size()/2)].y;
    //     parallel_plane_point.z = 8.0;
    //     x_parallel_plane_marker.points.push_back(parallel_plane_point);
    //   }
    // }
    // x_parallel_plane_marker.color.r = 1;
    // x_parallel_plane_marker.color.a = 1;
    // markers.markers.push_back(x_parallel_plane_marker); 


    //y vertical plane markers 
    visualization_msgs::Marker y_vert_plane_marker;
    y_vert_plane_marker.pose.orientation.w = 1.0;
    y_vert_plane_marker.scale.x = 0.05;
    y_vert_plane_marker.scale.y = 0.05;
    y_vert_plane_marker.scale.z = 0.05;
    //plane_marker.points.resize(vert_planes.size());    
    y_vert_plane_marker.header.frame_id = map_frame_id;
    y_vert_plane_marker.header.stamp = stamp;
    y_vert_plane_marker.ns = "y_vert_planes";
    y_vert_plane_marker.id = markers.markers.size();
    y_vert_plane_marker.type = visualization_msgs::Marker::CUBE_LIST;
   
    for(int i = 0; i < y_vert_planes.size(); ++i) {
      for(size_t j=0; j < y_vert_planes[i].cloud_seg_map->size(); ++j) { 
        geometry_msgs::Point point;
        point.x = y_vert_planes[i].cloud_seg_map->points[j].x;
        point.y = y_vert_planes[i].cloud_seg_map->points[j].y;
        point.z = y_vert_planes[i].cloud_seg_map->points[j].z + 5.0;
        y_vert_plane_marker.points.push_back(point);
      }
      y_vert_plane_marker.color.b = 1;
      y_vert_plane_marker.color.a = 1;
    }
    markers.markers.push_back(y_vert_plane_marker); 


    //y parallel plane markers 
    // visualization_msgs::Marker y_parallel_plane_marker;
    // y_parallel_plane_marker.pose.orientation.w = 1.0;
    // y_parallel_plane_marker.scale.x = 2.0;
    // y_parallel_plane_marker.scale.y = 0.05;
    // y_parallel_plane_marker.scale.z = 1.0;
    // //plane_marker.points.resize(vert_planes.size());    
    // y_parallel_plane_marker.header.frame_id = map_frame_id;
    // y_parallel_plane_marker.header.stamp = stamp;
    // y_parallel_plane_marker.ns = "x_parallel_planes";
    // y_parallel_plane_marker.id = markers.markers.size();
    // y_parallel_plane_marker.type = visualization_msgs::Marker::CUBE_LIST;
    // for(int i = 0; i < y_vert_planes.size(); ++i) {
    //   if (y_vert_planes[i].parallel_pair) {
    //     geometry_msgs::Point parallel_plane_point;
    //     parallel_plane_point.x = y_vert_planes[i].cloud_seg_map->points[(y_vert_planes[i].cloud_seg_map->points.size()/2)].x;
    //     parallel_plane_point.y = y_vert_planes[i].cloud_seg_map->points[(y_vert_planes[i].cloud_seg_map->points.size()/2)].y;
    //     parallel_plane_point.z = 8.0;
    //     y_parallel_plane_marker.points.push_back(parallel_plane_point);
    //   }
    // }
    // y_parallel_plane_marker.color.b = 1;
    // y_parallel_plane_marker.color.a = 1;
    // markers.markers.push_back(y_parallel_plane_marker); 


    //horizontal plane markers 
    visualization_msgs::Marker hort_plane_marker;
    hort_plane_marker.pose.orientation.w = 1.0;
    hort_plane_marker.scale.x = 0.05;
    hort_plane_marker.scale.y = 0.05;
    hort_plane_marker.scale.z = 0.05;
    //plane_marker.points.resize(vert_planes.size());    
    hort_plane_marker.header.frame_id = map_frame_id;
    hort_plane_marker.header.stamp = stamp;
    hort_plane_marker.ns = "hort_planes";
    hort_plane_marker.id = 8;
    hort_plane_marker.type = visualization_msgs::Marker::CUBE_LIST;
   
    for(int i = 0; i < hort_planes.size(); ++i) {
      for(size_t j=0; j < hort_planes[i].cloud_seg_map->size(); ++j) { 
        geometry_msgs::Point point;
        point.x = hort_planes[i].cloud_seg_map->points[j].x;
        point.y = hort_planes[i].cloud_seg_map->points[j].y;
        point.z = hort_planes[i].cloud_seg_map->points[j].z + 5.0;
        hort_plane_marker.points.push_back(point);
      }
      hort_plane_marker.color.r = 1;
      hort_plane_marker.color.g = 0.65;
      hort_plane_marker.color.a = 1;
    }
    markers.markers.push_back(hort_plane_marker); 

    //x corridor markers
    visualization_msgs::Marker corridor_marker;
    corridor_marker.pose.orientation.w = 1.0;
    corridor_marker.scale.x = 0.5;
    corridor_marker.scale.y = 0.5;
    corridor_marker.scale.z = 0.5;
    //plane_marker.points.resize(vert_planes.size());    
    corridor_marker.header.frame_id = map_frame_id;
    corridor_marker.header.stamp = stamp;
    corridor_marker.ns = "corridors";
    corridor_marker.id = markers.markers.size();
    corridor_marker.type = visualization_msgs::Marker::CUBE_LIST;
    corridor_marker.color.r = 0;
    corridor_marker.color.g = 1;
    corridor_marker.color.a = 1; 

    for(int i = 0; i < x_corridors.size(); ++i) {
      geometry_msgs::Point point;
      point.x = -x_corridors[i].node->estimate()(0);
      point.y =  x_corridors[i].node->estimate()(1);
      point.z = 12;
      corridor_marker.points.push_back(point);

      //fill in the text marker
      visualization_msgs::Marker corr_x_text_marker;
      corr_x_text_marker.scale.z = 0.5;
      corr_x_text_marker.ns = "corridor_x_text";
      corr_x_text_marker.header.frame_id = map_frame_id;
      corr_x_text_marker.header.stamp = stamp;
      corr_x_text_marker.id = markers.markers.size()+1;
      corr_x_text_marker.type = visualization_msgs::Marker::TEXT_VIEW_FACING;
      corr_x_text_marker.pose.position.x = -x_corridors[i].node->estimate()(0);
      corr_x_text_marker.pose.position.y = x_corridors[i].node->estimate()(1);
      corr_x_text_marker.pose.position.z = 11.5;
      corr_x_text_marker.color.r = 1;
      corr_x_text_marker.color.g = 1;
      corr_x_text_marker.color.b = 1;
      corr_x_text_marker.color.a = 1; 
      corr_x_text_marker.pose.orientation.w = 1.0;
      corr_x_text_marker.text = "Corridor X" + std::to_string(i+1);
      markers.markers.push_back(corr_x_text_marker);

      //fill in the line marker
      visualization_msgs::Marker corr_x_line_marker;
      corr_x_line_marker.scale.x = 0.05;
      corr_x_line_marker.pose.orientation.w = 1.0;
      corr_x_line_marker.ns = "corridor_x_lines";
      corr_x_line_marker.header.frame_id = map_frame_id;
      corr_x_line_marker.header.stamp = stamp;
      corr_x_line_marker.id = markers.markers.size()+1;
      corr_x_line_marker.type = visualization_msgs::Marker::LINE_LIST;
      corr_x_line_marker.color.r =  corr_x_line_marker.color.g = corr_x_line_marker.color.b = 1;  
      corr_x_line_marker.color.a = 1.0;
      geometry_msgs::Point p1,p2,p3;
      p1.x = -x_corridors[i].node->estimate()(0);
      p1.y =  x_corridors[i].node->estimate()(1);
      p1.z =  11.5;
      p2.x = -x_corridors[i].node->estimate()(0) - 0.5;
      p2.y =  x_corridors[i].node->estimate()(1);
      p2.z =  8;
      corr_x_line_marker.points.push_back(p1);
      corr_x_line_marker.points.push_back(p2);
      p3.x = -x_corridors[i].node->estimate()(0) + 0.5;
      p3.y =  x_corridors[i].node->estimate()(1);
      p3.z =  8;
      corr_x_line_marker.points.push_back(p1);
      corr_x_line_marker.points.push_back(p3);
      markers.markers.push_back(corr_x_line_marker); 
    }
    
    for(int i = 0; i < y_corridors.size(); ++i) {
      geometry_msgs::Point point;
      point.x =  y_corridors[i].node->estimate()(0);
      point.y = -y_corridors[i].node->estimate()(1);
      point.z = 12;
      corridor_marker.points.push_back(point);

      //fill in the text marker
      visualization_msgs::Marker corr_y_text_marker;
      corr_y_text_marker.scale.z = 0.5;
      corr_y_text_marker.ns = "corridor_y_text";
      corr_y_text_marker.header.frame_id = map_frame_id;
      corr_y_text_marker.header.stamp = stamp;
      corr_y_text_marker.id = markers.markers.size()+1;
      corr_y_text_marker.type = visualization_msgs::Marker::TEXT_VIEW_FACING;
      corr_y_text_marker.pose.position.x = y_corridors[i].node->estimate()(0);
      corr_y_text_marker.pose.position.y = -y_corridors[i].node->estimate()(1);
      corr_y_text_marker.pose.position.z = 11.5;
      corr_y_text_marker.color.r = 1;
      corr_y_text_marker.color.g = 1;
      corr_y_text_marker.color.b = 1;
      corr_y_text_marker.color.a = 1; 
      corr_y_text_marker.pose.orientation.w = 1.0;
      corr_y_text_marker.text = "Corridor Y" + std::to_string(i+1);
      markers.markers.push_back(corr_y_text_marker);

      //fill in the line marker
      visualization_msgs::Marker corr_y_line_marker;
      corr_y_line_marker.scale.x = 0.05;
      corr_y_line_marker.pose.orientation.w = 1.0;
      corr_y_line_marker.ns = "corridor_y_lines";
      corr_y_line_marker.header.frame_id = map_frame_id;
      corr_y_line_marker.header.stamp = stamp;
      corr_y_line_marker.id = markers.markers.size()+1;
      corr_y_line_marker.type = visualization_msgs::Marker::LINE_LIST;
      corr_y_line_marker.color.r =  corr_y_line_marker.color.g = corr_y_line_marker.color.b = 1;  
      corr_y_line_marker.color.a = 1.0;
      geometry_msgs::Point p1,p2,p3;
      p1.x =   y_corridors[i].node->estimate()(0);
      p1.y =  -y_corridors[i].node->estimate()(1);
      p1.z =  11.5;
      p2.x =   y_corridors[i].node->estimate()(0);
      p2.y =  -y_corridors[i].node->estimate()(1) - 0.5;
      p2.z =   8;
      corr_y_line_marker.points.push_back(p1);
      corr_y_line_marker.points.push_back(p2);
      p3.x =  y_corridors[i].node->estimate()(0);
      p3.y = -y_corridors[i].node->estimate()(1) + 0.5;
      p3.z =   8;
      corr_y_line_marker.points.push_back(p1);
      corr_y_line_marker.points.push_back(p3);
      markers.markers.push_back(corr_y_line_marker); 
    }
    markers.markers.push_back(corridor_marker); 


    //room markers
    visualization_msgs::Marker room_marker;
    room_marker.pose.orientation.w = 1.0;
    room_marker.scale.x = 0.5;
    room_marker.scale.y = 0.5;
    room_marker.scale.z = 0.5;
    //plane_marker.points.resize(vert_planes.size());    
    room_marker.header.frame_id = map_frame_id;
    room_marker.header.stamp = stamp;
    room_marker.ns = "rooms";
    room_marker.id = markers.markers.size();
    room_marker.type = visualization_msgs::Marker::CUBE_LIST;
    room_marker.color.r = 1;
    room_marker.color.g = 0.07;
    room_marker.color.b = 0.57;
    room_marker.color.a = 1; 

    for(int i = 0; i < rooms_vec.size(); ++i) {
      geometry_msgs::Point point;
      point.x = rooms_vec[i].node->estimate()(0);
      point.y = rooms_vec[i].node->estimate()(1);
      point.z = 14;
      room_marker.points.push_back(point);

      //fill in the text marker
      visualization_msgs::Marker room_text_marker;
      room_text_marker.scale.z = 0.5;
      room_text_marker.ns = "rooms_text";
      room_text_marker.header.frame_id = map_frame_id;
      room_text_marker.header.stamp = stamp;
      room_text_marker.id = markers.markers.size()+1;
      room_text_marker.type = visualization_msgs::Marker::TEXT_VIEW_FACING;
      room_text_marker.pose.position.x = rooms_vec[i].node->estimate()(0);
      room_text_marker.pose.position.y = rooms_vec[i].node->estimate()(1);
      room_text_marker.pose.position.z = 13.5;
      room_text_marker.color.r = 1;
      room_text_marker.color.g = 1;
      room_text_marker.color.b = 1;
      room_text_marker.color.a = 1; 
      room_text_marker.pose.orientation.w = 1.0;
      room_text_marker.text = "Room" + std::to_string(i+1);
      markers.markers.push_back(room_text_marker);

      //fill in the line marker
      visualization_msgs::Marker room_line_marker;
      room_line_marker.scale.x = 0.05;
      room_line_marker.pose.orientation.w = 1.0;
      room_line_marker.ns = "rooms_lines";
      room_line_marker.header.frame_id = map_frame_id;
      room_line_marker.header.stamp = stamp;
      room_line_marker.id = markers.markers.size()+1;
      room_line_marker.type = visualization_msgs::Marker::LINE_LIST;
      room_line_marker.color.r =  room_line_marker.color.g = room_line_marker.color.b = 1;  
      room_line_marker.color.a = 1.0;
      geometry_msgs::Point p1,p2,p3,p4,p5;
      p1.x = rooms_vec[i].node->estimate()(0);
      p1.y = rooms_vec[i].node->estimate()(1);
      p1.z = 13;
      p2.x = rooms_vec[i].node->estimate()(0) - 1;
      p2.y = rooms_vec[i].node->estimate()(1) - 1;
      p2.z = 10;
      room_line_marker.points.push_back(p1);
      room_line_marker.points.push_back(p2);
      p3.x = rooms_vec[i].node->estimate()(0) + 1;
      p3.y = rooms_vec[i].node->estimate()(1) - 1;
      p3.z = 10;
      room_line_marker.points.push_back(p1);
      room_line_marker.points.push_back(p3);
      p4.x = rooms_vec[i].node->estimate()(0) - 1;
      p4.y = rooms_vec[i].node->estimate()(1) + 1;
      p4.z = 10;
      room_line_marker.points.push_back(p1);
      room_line_marker.points.push_back(p4);
      p5.x = rooms_vec[i].node->estimate()(0) + 1;
      p5.y = rooms_vec[i].node->estimate()(1) + 1;
      p5.z = 10;
      room_line_marker.points.push_back(p1);
      room_line_marker.points.push_back(p5);
      markers.markers.push_back(room_line_marker); 
    }
    markers.markers.push_back(room_marker); 

    // final line markers for printing different layers for abstraction
    visualization_msgs::Marker robot_layer_marker;
    robot_layer_marker.scale.z = 1.5;
    robot_layer_marker.ns = "layer_marker";
    robot_layer_marker.header.frame_id = map_frame_id;
    robot_layer_marker.header.stamp = stamp;
    robot_layer_marker.id = markers.markers.size();
    robot_layer_marker.type = visualization_msgs::Marker::TEXT_VIEW_FACING;
    robot_layer_marker.pose.position.x = 0.0;
    robot_layer_marker.pose.position.y = 30.0;
    robot_layer_marker.pose.position.z = 0.0;
    robot_layer_marker.color.a = 1; 
    robot_layer_marker.pose.orientation.w = 1.0;
    robot_layer_marker.color.r =  robot_layer_marker.color.g = robot_layer_marker.color.b = 1;  
    robot_layer_marker.text = "Robot Tracking Layer";
    markers.markers.push_back(robot_layer_marker);

    if(!y_vert_planes.empty() || !x_vert_planes.empty()) {
      visualization_msgs::Marker semantic_layer_marker;
      semantic_layer_marker.scale.z = 1.5;
      semantic_layer_marker.ns = "layer_marker";
      semantic_layer_marker.header.frame_id = map_frame_id;
      semantic_layer_marker.header.stamp = stamp;
      semantic_layer_marker.id = markers.markers.size();
      semantic_layer_marker.type = visualization_msgs::Marker::TEXT_VIEW_FACING;
      semantic_layer_marker.pose.position.x = 0.0;
      semantic_layer_marker.pose.position.y = 30.0;
      semantic_layer_marker.pose.position.z = 5.0;
      semantic_layer_marker.color.r =  semantic_layer_marker.color.g = semantic_layer_marker.color.b = 1;  
      semantic_layer_marker.color.a = 1; 
      semantic_layer_marker.pose.orientation.w = 1.0;
      semantic_layer_marker.text = "Metric-Semantic Layer";
      markers.markers.push_back(semantic_layer_marker);
    }

    if(!x_corridors.empty() || !y_corridors.empty() || !rooms_vec.empty()) {
      visualization_msgs::Marker topological_layer_marker;
      topological_layer_marker.scale.z = 1.5;
      topological_layer_marker.ns = "layer_marker";
      topological_layer_marker.header.frame_id = map_frame_id;
      topological_layer_marker.header.stamp = stamp;
      topological_layer_marker.id = markers.markers.size();
      topological_layer_marker.type = visualization_msgs::Marker::TEXT_VIEW_FACING;
      topological_layer_marker.pose.position.x = 0.0;
      topological_layer_marker.pose.position.y = 30.0;
      topological_layer_marker.pose.position.z = 12.0;
      topological_layer_marker.color.r =  topological_layer_marker.color.g = topological_layer_marker.color.b = 1;  
      topological_layer_marker.color.a = 1; 
      topological_layer_marker.pose.orientation.w = 1.0;
      topological_layer_marker.text = "Topological Layer";
      markers.markers.push_back(topological_layer_marker);
    }

    return markers;
  }

  /**
   * @brief dump all data to the current directory
   * @param req
   * @param res
   * @return
   */
  bool dump_service(hdl_graph_slam::DumpGraphRequest& req, hdl_graph_slam::DumpGraphResponse& res) {
    std::lock_guard<std::mutex> lock(main_thread_mutex);

    std::string directory = req.destination;

    if(directory.empty()) {
      std::array<char, 64> buffer;
      buffer.fill(0);
      time_t rawtime;
      time(&rawtime);
      const auto timeinfo = localtime(&rawtime);
      strftime(buffer.data(), sizeof(buffer), "%d-%m-%Y %H:%M:%S", timeinfo);
    }

    if(!boost::filesystem::is_directory(directory)) {
      boost::filesystem::create_directory(directory);
    }

    std::cout << "all data dumped to:" << directory << std::endl;

    graph_slam->save(directory + "/graph.g2o");
    for(int i = 0; i < keyframes.size(); i++) {
      std::stringstream sst;
      sst << boost::format("%s/%06d") % directory % i;

      keyframes[i]->save(sst.str());
    }

    if(zero_utm) {
      std::ofstream zero_utm_ofs(directory + "/zero_utm");
      zero_utm_ofs << boost::format("%.6f %.6f %.6f") % zero_utm->x() % zero_utm->y() % zero_utm->z() << std::endl;
    }

    std::ofstream ofs(directory + "/special_nodes.csv");
    ofs << "anchor_node " << (anchor_node == nullptr ? -1 : anchor_node->id()) << std::endl;
    ofs << "anchor_edge " << (anchor_edge == nullptr ? -1 : anchor_edge->id()) << std::endl;
    ofs << "floor_node " << (floor_plane_node == nullptr ? -1 : floor_plane_node->id()) << std::endl;

    res.success = true;
    return true;
  }

  /**
   * @brief save map data as pcd
   * @param req
   * @param res
   * @return
   */
  bool save_map_service(hdl_graph_slam::SaveMapRequest& req, hdl_graph_slam::SaveMapResponse& res) {
    std::vector<KeyFrameSnapshot::Ptr> snapshot;

    keyframes_snapshot_mutex.lock();
    snapshot = keyframes_snapshot;
    keyframes_snapshot_mutex.unlock();

    auto cloud = map_cloud_generator->generate(snapshot, req.resolution);
    if(!cloud) {
      res.success = false;
      return true;
    }

    if(zero_utm && req.utm) {
      for(auto& pt : cloud->points) {
        pt.getVector3fMap() += (*zero_utm).cast<float>();
      }
    }

    cloud->header.frame_id = map_frame_id;
    cloud->header.stamp = snapshot.back()->cloud->header.stamp;

    if(zero_utm) {
      std::ofstream ofs(req.destination + ".utm");
      ofs << boost::format("%.6f %.6f %.6f") % zero_utm->x() % zero_utm->y() % zero_utm->z() << std::endl;
    }

    int ret = pcl::io::savePCDFileBinary(req.destination, *cloud);
    res.success = ret == 0;

    return true;
  }

private:
  // ROS
  ros::NodeHandle nh;
  ros::NodeHandle mt_nh;
  ros::NodeHandle private_nh;
  ros::WallTimer optimization_timer;
  ros::WallTimer map_publish_timer;

  std::unique_ptr<message_filters::Subscriber<nav_msgs::Odometry>> odom_sub;
  std::unique_ptr<message_filters::Subscriber<sensor_msgs::PointCloud2>> cloud_sub;
  std::unique_ptr<message_filters::Synchronizer<ApproxSyncPolicy>> sync;

  ros::Subscriber cloud_seg_sub;
  ros::Subscriber gps_sub;
  ros::Subscriber nmea_sub;
  ros::Subscriber navsat_sub;

  ros::Subscriber imu_sub;
  ros::Subscriber floor_sub;

  ros::Publisher markers_pub;

  std::string map_frame_id;
  std::string odom_frame_id;

  bool wait_trans_odom2map, got_trans_odom2map;
  std::mutex trans_odom2map_mutex;
  Eigen::Matrix4f trans_odom2map;
  ros::Publisher odom2map_pub;
  ros::Subscriber init_odom2map_sub;

  std::string points_topic;
  ros::Publisher read_until_pub;
  ros::Publisher map_points_pub;

  tf::TransformListener tf_listener;

  ros::ServiceServer dump_service_server;
  ros::ServiceServer save_map_service_server;

  // keyframe queue
  std::string base_frame_id;
  std::mutex keyframe_queue_mutex;
  std::deque<KeyFrame::Ptr> keyframe_queue;

  // gps queue
  double gps_time_offset;
  double gps_edge_stddev_xy;
  double gps_edge_stddev_z;
  boost::optional<Eigen::Vector3d> zero_utm;
  std::mutex gps_queue_mutex;
  std::deque<geographic_msgs::GeoPointStampedConstPtr> gps_queue;

  // imu queue
  double imu_time_offset;
  bool enable_imu_orientation;
  double imu_orientation_edge_stddev;
  bool enable_imu_acceleration;
  double imu_acceleration_edge_stddev;
  std::mutex imu_queue_mutex;
  std::deque<sensor_msgs::ImuConstPtr> imu_queue;

  // floor_coeffs queue
  double floor_edge_stddev;
  std::mutex floor_coeffs_queue_mutex;
  std::deque<hdl_graph_slam::FloorCoeffsConstPtr> floor_coeffs_queue;

  //vertical and horizontal planes
  double plane_dist_threshold;
  bool use_point_to_plane;
  bool use_parallel_plane_constraint, use_perpendicular_plane_constraint;
  bool use_corridor_constraint, use_room_constraint;
  double corridor_dist_threshold, corridor_min_plane_length, corridor_min_width, corridor_max_width;
  double corridor_plane_length_diff_threshold;
  double room_plane_length_diff_threshold;
  double room_dist_threshold, room_min_plane_length, room_max_plane_length, room_min_width;
  std::vector<VerticalPlanes> x_vert_planes, y_vert_planes;         // vertically segmented planes
  std::vector<HorizontalPlanes> hort_planes;                        // horizontally segmented planes
  std::vector<Corridors> x_corridors, y_corridors;  // corridors segmented from planes
  std::vector<Rooms> rooms_vec;                    // rooms segmented from planes
  enum plane_class : uint8_t{
    X_VERT_PLANE = 0,
    Y_VERT_PLANE = 1,
    HORT_PLANE = 2,
  };

  // Seg map queue
  std::mutex cloud_seg_mutex;
  std::deque<hdl_graph_slam::PointClouds::Ptr> clouds_seg_queue; 

  // for map cloud generation
  std::atomic_bool graph_updated;
  double map_cloud_resolution;
  std::mutex keyframes_snapshot_mutex;
  std::vector<KeyFrameSnapshot::Ptr> keyframes_snapshot;
  std::unique_ptr<MapCloudGenerator> map_cloud_generator;
  
  
  // graph slam
  // all the below members must be accessed after locking main_thread_mutex
  std::mutex main_thread_mutex;

  int max_keyframes_per_update;
  std::deque<KeyFrame::Ptr> new_keyframes;

  g2o::VertexSE3* anchor_node;
  g2o::EdgeSE3* anchor_edge;
  g2o::VertexPlane* floor_plane_node;
  std::vector<KeyFrame::Ptr> keyframes;
  std::unordered_map<ros::Time, KeyFrame::Ptr, RosTimeHash> keyframe_hash;
  
  std::unique_ptr<GraphSLAM> graph_slam;
  std::unique_ptr<LoopDetector> loop_detector;
  std::unique_ptr<KeyframeUpdater> keyframe_updater;
  std::unique_ptr<NmeaSentenceParser> nmea_parser;

  std::unique_ptr<InformationMatrixCalculator> inf_calclator;
};

}  // namespace hdl_graph_slam

PLUGINLIB_EXPORT_CLASS(hdl_graph_slam::HdlGraphSlamNodelet, nodelet::Nodelet)
