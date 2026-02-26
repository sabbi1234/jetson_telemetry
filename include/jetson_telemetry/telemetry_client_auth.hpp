#ifndef JETSON_TELEMETRY__TELEMETRY_CLIENT_AUTH_HPP_
#define JETSON_TELEMETRY__TELEMETRY_CLIENT_AUTH_HPP_

#include <rclcpp/rclcpp.hpp>
#include <geometry_msgs/msg/twist.hpp>
#include <geometry_msgs/msg/pose_with_covariance_stamped.hpp>
#include <sensor_msgs/msg/battery_state.hpp>
#include <websocketpp/config/asio_no_tls_client.hpp>
#include <websocketpp/client.hpp>
#include <nlohmann/json.hpp>
#include <mutex>
#include <thread>
#include <string>
#include <utility>
#include <atomic>

typedef websocketpp::client<websocketpp::config::asio_client> ws_client;
using json = nlohmann::json;

namespace jetson_telemetry
{

class TelemetryClientAuth : public rclcpp::Node
{
public:
  TelemetryClientAuth();
  ~TelemetryClientAuth();

private:
  // -------- ROS callbacks --------
  void cmd_cb(const geometry_msgs::msg::Twist::SharedPtr msg);
  void amcl_cb(const geometry_msgs::msg::PoseWithCovarianceStamped::SharedPtr msg);
  void battery_cb(const sensor_msgs::msg::BatteryState::SharedPtr msg);

  // -------- WebSocket --------
  void try_connect();
  void publish();
  void send_message(const std::string& message);
  void handle_server_message(const std::string& payload);

  // -------- Members --------
  std::mutex m_;
  geometry_msgs::msg::Twist cmd_;
  geometry_msgs::msg::PoseWithCovarianceStamped amcl_;
  double battery_{-1.0};

  rclcpp::Subscription<geometry_msgs::msg::Twist>::SharedPtr cmd_sub_;
  rclcpp::Subscription<geometry_msgs::msg::PoseWithCovarianceStamped>::SharedPtr amcl_sub_;
  rclcpp::Subscription<sensor_msgs::msg::BatteryState>::SharedPtr battery_sub_;
  rclcpp::Publisher<geometry_msgs::msg::Twist>::SharedPtr cmd_pub_;   // ← publishes UI commands to /cmd_vel
  rclcpp::TimerBase::SharedPtr publish_timer_;
  rclcpp::TimerBase::SharedPtr reconnect_timer_;

  ws_client ws_;
  websocketpp::connection_hdl hdl_;
  std::thread ws_thread_;

  std::atomic<bool> connected_{false};
  std::atomic<bool> connecting_{false};

  std::string ws_uri_;
  std::string rover_id_;
  std::string rover_secret_;    // ← NEW: pre-shared secret for server auth
  int assigned_rover_id_{-1};
  int reconnect_sec_{3};
  int publish_sec_{1};

  double cpu_usage_ = 0.0;
  long long prev_idle_ = 0;
  long long prev_total_ = 0;
  bool cpu_initialized_ = false;
  double get_cpu_usage();
};

}  // namespace jetson_telemetry

#endif  // JETSON_TELEMETRY__TELEMETRY_CLIENT_AUTH_HPP_
