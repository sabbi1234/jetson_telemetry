#include "jetson_telemetry/telemetry_client.hpp"

#include <fstream>
#include <sstream>
#include <iomanip>
#include <cmath>
#include <atomic>

namespace jetson_telemetry
{

TelemetryClient::TelemetryClient()
: Node("telemetry_client")
{
  // ---------------- Parameters ----------------
  declare_parameter("ws_uri", "ws://16.170.164.175:5000/ws");
  declare_parameter("rover_id", "R_005");
  declare_parameter("reconnect_interval", 3);
  declare_parameter("publish_interval", 30);

  get_parameter("ws_uri", ws_uri_);
  get_parameter("rover_id", rover_id_);
  get_parameter("reconnect_interval", reconnect_sec_);
  get_parameter("publish_interval", publish_sec_);

  RCLCPP_INFO(get_logger(), "====================================");
  RCLCPP_INFO(get_logger(), "TelemetryClient started");
  RCLCPP_INFO(get_logger(), "WS URI   : %s", ws_uri_.c_str());
  RCLCPP_INFO(get_logger(), "Rover ID : %s", rover_id_.c_str());
  RCLCPP_INFO(get_logger(), "====================================");

  // ---------------- ROS Subscriptions ----------------
  cmd_sub_ = create_subscription<geometry_msgs::msg::Twist>(
    "/cmd_vel", 10,
    std::bind(&TelemetryClient::cmd_cb, this, std::placeholders::_1));

  amcl_sub_ = create_subscription<geometry_msgs::msg::PoseWithCovarianceStamped>(
    "/amcl_pose", 10,
    std::bind(&TelemetryClient::amcl_cb, this, std::placeholders::_1));

  battery_sub_ = create_subscription<sensor_msgs::msg::BatteryState>(
    "/battery_state", 10,
    std::bind(&TelemetryClient::battery_cb, this, std::placeholders::_1));

  // ---------------- WebSocket Setup ----------------
  ws_.init_asio();
  
  // Disable verbose logging - only show errors
  ws_.clear_access_channels(websocketpp::log::alevel::all);
  ws_.clear_error_channels(websocketpp::log::elevel::all);
  
  // Set user agent
  ws_.set_user_agent("websocketpp/0.8.2");
  
  // IMPORTANT: Set to perpetual mode to keep the io_service running
  ws_.start_perpetual();

  ws_.set_open_handler([this](websocketpp::connection_hdl hdl) {
    connecting_ = false;
    connected_ = true;
    hdl_ = hdl;

    RCLCPP_INFO(get_logger(), "✅ Connected to server");

    // Send CONNECT message using JSON library
    json msg = {
      {"type", "CONNECT"},
      {"payload", {
        {"type", "rover"},
        {"identifier", rover_id_}
      }}
    };

    std::string msg_str = msg.dump();
    RCLCPP_INFO(get_logger(), "📤 TX: %s", msg_str.c_str());

    websocketpp::lib::error_code ec;
    ws_.send(hdl, msg_str, websocketpp::frame::opcode::text, ec);

    if (ec) {
      RCLCPP_ERROR(get_logger(), "❌ Send failed: %s", ec.message().c_str());
    }
  });

  ws_.set_fail_handler([this](websocketpp::connection_hdl hdl) {
    connecting_ = false;
    connected_ = false;

    auto con = ws_.get_con_from_hdl(hdl);
    auto ec = con->get_ec();
    
    RCLCPP_ERROR(get_logger(), "❌ Connection failed: %s", ec.message().c_str());
  });

  ws_.set_close_handler([this](websocketpp::connection_hdl) {
    connected_ = false;
    RCLCPP_WARN(get_logger(), "⚠️ Disconnected");
  });

  ws_.set_message_handler([this](websocketpp::connection_hdl,
                                 ws_client::message_ptr msg) {
    handle_server_message(msg->get_payload());
  });

  ws_thread_ = std::thread([this]() {
    try {
      ws_.run();
    } catch (const std::exception& e) {
      RCLCPP_ERROR(get_logger(), "WebSocket thread error: %s", e.what());
    }
  });

  // ---------------- Timers ----------------
  reconnect_timer_ = create_wall_timer(
    std::chrono::seconds(reconnect_sec_),
    std::bind(&TelemetryClient::try_connect, this));

  publish_timer_ = create_wall_timer(
    std::chrono::seconds(publish_sec_),
    std::bind(&TelemetryClient::publish, this));

  try_connect();
}

TelemetryClient::~TelemetryClient()
{
  connected_ = false;
  ws_.stop_perpetual();
  
  if (connected_ && !hdl_.expired()) {
    websocketpp::lib::error_code ec;
    ws_.close(hdl_, websocketpp::close::status::going_away, "Shutdown", ec);
  }
  
  ws_.stop();
  
  if (ws_thread_.joinable()) {
    ws_thread_.join();
  }
}

// ---------------- ROS Callbacks ----------------
void TelemetryClient::cmd_cb(const geometry_msgs::msg::Twist::SharedPtr msg)
{
  std::lock_guard<std::mutex> l(m_);
  cmd_ = *msg;
}

void TelemetryClient::amcl_cb(
  const geometry_msgs::msg::PoseWithCovarianceStamped::SharedPtr msg)
{
  std::lock_guard<std::mutex> l(m_);
  amcl_ = *msg;
}

void TelemetryClient::battery_cb(
  const sensor_msgs::msg::BatteryState::SharedPtr msg)
{
  std::lock_guard<std::mutex> l(m_);
  battery_ = msg->percentage * 100.0;
}

// ---------------- WebSocket Connect ----------------
void TelemetryClient::try_connect()
{
  // Check if already connected or connecting
  if (connected_ || connecting_) {
    return;
  }
  
  connecting_ = true;

  try {
    websocketpp::lib::error_code ec;
    auto con = ws_.get_connection(ws_uri_, ec);

    if (ec) {
      connecting_ = false;
      RCLCPP_ERROR(get_logger(), "❌ Connection error: %s", ec.message().c_str());
      return;
    }
    
    ws_.connect(con);
    
  } catch (const std::exception& e) {
    connecting_ = false;
    RCLCPP_ERROR(get_logger(), "❌ Exception: %s", e.what());
  }
}

// ---------------- Messaging ----------------
void TelemetryClient::send_message(const std::string& msg)
{
  if (!connected_ || hdl_.expired()) {
    return;
  }

  RCLCPP_INFO(get_logger(), "📤 TX: %s", msg.c_str());

  websocketpp::lib::error_code ec;
  ws_.send(hdl_, msg, websocketpp::frame::opcode::text, ec);

  if (ec) {
    connected_ = false;
    RCLCPP_ERROR(get_logger(), "❌ Send failed: %s", ec.message().c_str());
  }
}

void TelemetryClient::handle_server_message(const std::string& payload)
{
  RCLCPP_INFO(get_logger(), "📥 RX: %s", payload.c_str());

  try {
    json msg = json::parse(payload);
    
    // Handle CONNECT response
    if (msg["type"] == "CONNECT" && msg["payload"]["success"] == true) {
      if (msg["payload"].contains("roverId")) {
        assigned_rover_id_ = msg["payload"]["roverId"];
        RCLCPP_INFO(get_logger(), "✅ Rover ID: %d", assigned_rover_id_);
      } else {
        assigned_rover_id_ = 999;
      }
    }
    
    // Handle ERROR messages
    if (msg["type"] == "ERROR") {
      RCLCPP_ERROR(get_logger(), "🚨 Server error");
    }
    
    // Handle rover ID assignment in other messages
    if (msg.contains("roverId") && assigned_rover_id_ < 0) {
      assigned_rover_id_ = msg["roverId"];
      RCLCPP_INFO(get_logger(), "✅ Rover ID: %d", assigned_rover_id_);
    }
    
    // Handle command messages
    if (msg["type"] == "COMMAND") {
      RCLCPP_INFO(get_logger(), "📋 Command received");
      // Add your command handling logic here
    }
    
  } catch (const json::exception& e) {
    RCLCPP_ERROR(get_logger(), "JSON parse error: %s", e.what());
  }
}

// ---------------- Telemetry ----------------
void TelemetryClient::publish()
{
  if (!connected_ || assigned_rover_id_ < 0) {
    return;
  }

  geometry_msgs::msg::Twist cmd;
  geometry_msgs::msg::PoseWithCovarianceStamped amcl;
  double batt;

  {
    std::lock_guard<std::mutex> l(m_);
    cmd = cmd_;
    amcl = amcl_;
    batt = battery_;
  }
  double cpu = get_cpu_usage();
  // Build telemetry JSON message
  json telemetry = {
    {"type", "TELEMETRY"},
    {"roverId", assigned_rover_id_},
    {"payload", {
      {"sensorData", {
        {"cpuUsage", cpu},
        {"memoryUsage", 0},
        {"batteryLevel", batt},
        {"speed", std::abs(cmd.linear.x)},
        {"currentPosition", {
          {"x", amcl.pose.pose.position.x},
          {"y", amcl.pose.pose.position.y},
          {"z", amcl.pose.pose.position.z}
        }}
      }}
    }}
  };

  send_message(telemetry.dump());
  
  // Send status update
  json status = {
    {"type", "STATUS_UPDATE"},
    {"roverId", assigned_rover_id_},
    {"payload", {
      {"status", "active"}
    }}
  };
  
  send_message(status.dump());
}

// cpu usage retrieval

double TelemetryClient::get_cpu_usage()
{
  std::ifstream file("/proc/stat");
  std::string line;
  std::getline(file, line);
  file.close();

  std::istringstream ss(line);

  std::string cpu;
  long long user, nice, system, idle, iowait, irq, softirq, steal;

  ss >> cpu >> user >> nice >> system >> idle >> iowait >> irq >> softirq >> steal;

  long long idle_time = idle + iowait;
  long long total_time = user + nice + system + idle + iowait + irq + softirq + steal;

  if (!cpu_initialized_) {
    prev_idle_ = idle_time;
    prev_total_ = total_time;
    cpu_initialized_ = true;
    return 0.0;
  }

  long long idle_delta = idle_time - prev_idle_;
  long long total_delta = total_time - prev_total_;

  prev_idle_ = idle_time;
  prev_total_ = total_time;

  if (total_delta == 0)
    return 0.0;

  double usage = 100.0 * (1.0 - (double)idle_delta / total_delta);

  return usage;
}

}  // namespace jetson_telemetry

// ---------------- main ----------------
int main(int argc, char **argv)
{
  rclcpp::init(argc, argv);
  
  try {
    auto node = std::make_shared<jetson_telemetry::TelemetryClient>();
    rclcpp::spin(node);
  } catch (const std::exception& e) {
    std::cerr << "Exception in main: " << e.what() << std::endl;
    return 1;
  }
  
  rclcpp::shutdown();
  return 0;
}