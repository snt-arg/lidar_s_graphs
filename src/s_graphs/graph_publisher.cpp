#include <s_graphs/graph_publisher.hpp>

GraphPublisher::GraphPublisher(const ros::NodeHandle& private_nh) {}

GraphPublisher::~GraphPublisher() {}
ros1_graph_manager_interface::Graph GraphPublisher::publish_graph(std::unique_ptr<s_graphs::GraphSLAM>& graph_slam, std::string graph_type) {
  g2o::SparseOptimizer* local_graph = graph_slam->graph.get();
  std::vector<ros1_graph_manager_interface::Edge> edges_vec;
  std::vector<ros1_graph_manager_interface::Node> nodes_vec;
  ros1_graph_manager_interface::Graph graph_msg;
  std::vector<ros1_graph_manager_interface::Attribute> att_vec;
  auto edge_itr = local_graph->edges().begin();
  for(int i = 0; edge_itr != local_graph->edges().end(); edge_itr++, i++) {
    g2o::HyperGraph::Edge* edge = *edge_itr;
    g2o::EdgeRoom4Planes* edge_r4p = dynamic_cast<g2o::EdgeRoom4Planes*>(edge);
    if(edge_r4p) {
      g2o::VertexRoomXYLB* v_room = dynamic_cast<g2o::VertexRoomXYLB*>(edge_r4p->vertices()[0]);
      g2o::VertexPlane* v_xplane1 = dynamic_cast<g2o::VertexPlane*>(edge_r4p->vertices()[1]);
      g2o::VertexPlane* v_xplane2 = dynamic_cast<g2o::VertexPlane*>(edge_r4p->vertices()[2]);
      g2o::VertexPlane* v_yplane1 = dynamic_cast<g2o::VertexPlane*>(edge_r4p->vertices()[3]);
      g2o::VertexPlane* v_yplane2 = dynamic_cast<g2o::VertexPlane*>(edge_r4p->vertices()[4]);
      ros1_graph_manager_interface::Edge edge_nodes;
      edge_nodes.origin_node = v_room->id();
      edge_nodes.target_node = v_xplane1->id();
      ros1_graph_manager_interface::Attribute edge_attributes;
      edge_attributes.name = "EdgeRoom4Planes";
      att_vec.push_back(edge_attributes);
      edge_nodes.attributes = att_vec;
      edges_vec.push_back(edge_nodes);
      att_vec.clear();
      // std::cout << "v_room : " << v_room->id() << std::endl;
      // std::cout << "v_xplane1 : " << v_xplane1->id() << '\n';
      // std::cout << "v_xplane2 : " << v_xplane2->id() << '\n';
      // std::cout << "v_yplane1 : " << v_yplane1->id() << '\n';
      // std::cout << "v_yplane2 : " << v_yplane2->id() << '\n';
    } else {
      continue;
    }

    // std::cout << " edges_vec size : " << edges_vec.size() << std::endl;
  }
  graph_msg.edges = edges_vec;
  if(graph_type == "BIM") {
    graph_msg.name = "BIM";
  }
  return graph_msg;
}