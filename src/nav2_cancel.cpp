#include <rclcpp/rclcpp.hpp>
#include <rclcpp_action/rclcpp_action.hpp>
#include <nav2_msgs/action/navigate_to_pose.hpp>

using NavigateToPose = nav2_msgs::action::NavigateToPose;

int main(int argc, char ** argv)
{
  rclcpp::init(argc, argv);
  auto node = rclcpp::Node::make_shared("nav2_cancel");

  auto client = rclcpp_action::create_client<NavigateToPose>(node, "navigate_to_pose");

  RCLCPP_INFO(node->get_logger(), "Waiting for navigate_to_pose action server...");

  if (!client->wait_for_action_server(std::chrono::seconds(1))) {
    RCLCPP_WARN(node->get_logger(), "navigate_to_pose not available — Nav2 not running");
    rclcpp::shutdown();
    return 1;
  }

  RCLCPP_INFO(node->get_logger(), "Sending cancel request for all goals...");

  auto future = client->async_cancel_all_goals();

  if (rclcpp::spin_until_future_complete(node, future) != rclcpp::FutureReturnCode::SUCCESS) {
    RCLCPP_ERROR(node->get_logger(), "Cancel request failed");
    rclcpp::shutdown();
    return 1;
  }

  auto result = future.get();
  RCLCPP_INFO(node->get_logger(), "✅ Cancel accepted — %zu goal(s) cancelled",
    result->goals_canceling.size());

  rclcpp::shutdown();
  return 0;
}
