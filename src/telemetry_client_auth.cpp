#include "jetson_telemetry/telemetry_client_auth.hpp"

#include <fstream>
#include <sstream>
#include <iomanip>
#include <cmath>
#include <atomic>
#include <cstdlib>

namespace jetson_telemetry
{

TelemetryClientAuth::TelemetryClientAuth()
: Node("telemetry_client_auth")
{
  // ---------------- Parameters ----------------
  declare_parameter("ws_uri", "ws://localhost:5000/ws");
  declare_parameter("rover_id", "R_001");
  declare_parameter("rover_secret", "");   // ← NEW: must match ROVER_SECRET in server .env
  declare_parameter("reconnect_interval", 3);
  declare_parameter("publish_interval", 1);

  get_parameter("ws_uri", ws_uri_);
  get_parameter("rover_id", rover_id_);
  get_parameter("rover_secret", rover_secret_);
  get_parameter("reconnect_interval", reconnect_sec_);
  get_parameter("publish_interval", publish_sec_);

  RCLCPP_INFO(get_logger(), "====================================");
  RCLCPP_INFO(get_logger(), "TelemetryClientAuth started");
  RCLCPP_INFO(get_logger(), "WS URI   : %s", ws_uri_.c_str());
  RCLCPP_INFO(get_logger(), "Rover ID : %s", rover_id_.c_str());
  RCLCPP_INFO(get_logger(), "Auth     : %s", rover_secret_.empty() ? "NONE (will be rejected)" : "OK");
  RCLCPP_INFO(get_logger(), "====================================");

  // ---------------- ROS Publishers ----------------
  cmd_pub_ = create_publisher<geometry_msgs::msg::Twist>("/cmd_vel", 10);

  // ---------------- ROS Subscriptions ----------------
  cmd_sub_ = create_subscription<geometry_msgs::msg::Twist>(
    "/cmd_vel", 10,
    std::bind(&TelemetryClientAuth::cmd_cb, this, std::placeholders::_1));

  amcl_sub_ = create_subscription<geometry_msgs::msg::PoseWithCovarianceStamped>(
    "/amcl_pose", 10,
    std::bind(&TelemetryClientAuth::amcl_cb, this, std::placeholders::_1));

  battery_sub_ = create_subscription<sensor_msgs::msg::BatteryState>(
    "/battery_state", 10,
    std::bind(&TelemetryClientAuth::battery_cb, this, std::placeholders::_1));

  // ---------------- WebSocket Setup ----------------
  ws_.init_asio();

  ws_.clear_access_channels(websocketpp::log::alevel::all);
  ws_.clear_error_channels(websocketpp::log::elevel::all);

  ws_.set_user_agent("websocketpp/0.8.2");
  ws_.start_perpetual();

  ws_.set_open_handler([this](websocketpp::connection_hdl hdl) {
    connecting_ = false;
    connected_ = true;
    hdl_ = hdl;

    RCLCPP_INFO(get_logger(), "✅ Connected to server");

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
    std::bind(&TelemetryClientAuth::try_connect, this));

  publish_timer_ = create_wall_timer(
    std::chrono::seconds(publish_sec_),
    std::bind(&TelemetryClientAuth::publish, this));

  try_connect();
}

TelemetryClientAuth::~TelemetryClientAuth()
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
void TelemetryClientAuth::cmd_cb(const geometry_msgs::msg::Twist::SharedPtr msg)
{
  std::lock_guard<std::mutex> l(m_);
  cmd_ = *msg;
}

void TelemetryClientAuth::amcl_cb(
  const geometry_msgs::msg::PoseWithCovarianceStamped::SharedPtr msg)
{
  std::lock_guard<std::mutex> l(m_);
  amcl_ = *msg;
}

void TelemetryClientAuth::battery_cb(
  const sensor_msgs::msg::BatteryState::SharedPtr msg)
{
  std::lock_guard<std::mutex> l(m_);
  battery_ = msg->percentage * 100.0;
}

// ---------------- WebSocket Connect ----------------
void TelemetryClientAuth::try_connect()
{
  if (connected_ || connecting_) {
    return;
  }

  connecting_ = true;

  try {
    // ← KEY CHANGE: append ?token=<rover_secret> to authenticate with the server
    std::string uri = ws_uri_;
    if (!rover_secret_.empty()) {
      uri += "?token=" + rover_secret_;
    } else {
      RCLCPP_WARN(get_logger(), "⚠️ rover_secret is empty — server will reject the connection");
    }

    websocketpp::lib::error_code ec;
    auto con = ws_.get_connection(uri, ec);

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
void TelemetryClientAuth::send_message(const std::string& msg)
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

void TelemetryClientAuth::handle_server_message(const std::string& payload)
{
  RCLCPP_INFO(get_logger(), "📥 RX: %s", payload.c_str());

  try {
    json msg = json::parse(payload);

    if (msg["type"] == "CONNECT" && msg["payload"]["success"] == true) {
      if (msg["payload"].contains("roverId")) {
        assigned_rover_id_ = msg["payload"]["roverId"];
        RCLCPP_INFO(get_logger(), "✅ Rover ID: %d", assigned_rover_id_);
      } else {
        assigned_rover_id_ = 999;
      }
    }

    if (msg["type"] == "ERROR") {
      RCLCPP_ERROR(get_logger(), "🚨 Server error");
    }

    if (msg.contains("roverId") && assigned_rover_id_ < 0) {
      assigned_rover_id_ = msg["roverId"];
      RCLCPP_INFO(get_logger(), "✅ Rover ID: %d", assigned_rover_id_);
    }

    if (msg["type"] == "COMMAND") {
      std::string command = msg["payload"].value("command", "");
      int cmd_id          = msg["payload"].value("commandId", -1);

      RCLCPP_INFO(get_logger(), "📋 Command: %s (id=%d)", command.c_str(), cmd_id);

      geometry_msgs::msg::Twist twist;   // zero by default
      bool valid = true;
      bool already_published = false;    // set true when stop branch publishes early
      std::string response = "ok";

      std::istringstream ss(command);
      std::string action;
      ss >> action;

      if (action == "move") {
        std::string dir;
        double speed = 0.5;
        ss >> dir >> speed;

        if      (dir == "forward")  { twist.linear.x  =  speed; }
        else if (dir == "backward") { twist.linear.x  = -speed; }
        else if (dir == "left")     { twist.angular.z =  speed; }
        else if (dir == "right")    { twist.angular.z = -speed; }
        else {
          valid    = false;
          response = "unknown direction: " + dir;
        }

      } else if (action == "stop") {
        // Publish zero Twist immediately — motors stop regardless of Nav2
        cmd_pub_->publish(twist);

        // Run nav2_cancel synchronously so COMMAND_RESPONSE reflects the real outcome.
        // nav2_cancel now has a 1-second action-server timeout, so this blocks at most ~1s.
        int nav2_ret = std::system("ros2 run jetson_telemetry nav2_cancel 2>/dev/null");

        if (nav2_ret == 0) {
          response = "stopped + nav2 goal cancelled";
          RCLCPP_INFO(get_logger(), "🛑 Motors stopped, Nav2 goal cancelled");
        } else {
          response = "motors stopped (nav2 not running)";
          RCLCPP_WARN(get_logger(), "🛑 Motors stopped, Nav2 cancel skipped — not running");
        }

        already_published = true;

      } else if (action == "lights") {
        std::string state;
        ss >> state;
        response = "lights " + state;   // physical GPIO can be added here later

      } else {
        valid    = false;
        response = "unknown command: " + action;
      }

      if (valid && !already_published) {
        cmd_pub_->publish(twist);
        RCLCPP_INFO(get_logger(), "✅ Published /cmd_vel  linear.x=%.2f  angular.z=%.2f",
          twist.linear.x, twist.angular.z);
      } else {
        RCLCPP_WARN(get_logger(), "⚠️  Invalid command: %s", response.c_str());
      }

      // Send COMMAND_RESPONSE so the server marks the log as success/failed
      if (cmd_id >= 0) {
        json resp = {
          {"type", "COMMAND_RESPONSE"},
          {"payload", {
            {"commandId", cmd_id},
            {"status",    valid ? "success" : "failed"},
            {"response",  response}
          }}
        };
        send_message(resp.dump());
      }
    }

  } catch (const json::exception& e) {
    RCLCPP_ERROR(get_logger(), "JSON parse error: %s", e.what());
  }
}

// ---------------- Telemetry ----------------
void TelemetryClientAuth::publish()
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

  // Convert quaternion → yaw (radians)
  const auto& q = amcl.pose.pose.orientation;
  double yaw = std::atan2(
    2.0 * (q.w * q.z + q.x * q.y),
    1.0 - 2.0 * (q.y * q.y + q.z * q.z)
  );

  json telemetry = {
    {"type", "TELEMETRY"},
    {"roverId", assigned_rover_id_},
    {"payload", {
      {"sensorData", {
        {"cpuUsage", cpu},
        {"memoryUsage", 0},
        {"batteryLevel", batt},
        {"speed", std::abs(cmd.linear.x)},
        {"angle", yaw},
        {"currentPosition", {
          {"x", amcl.pose.pose.position.x},
          {"y", amcl.pose.pose.position.y},
          {"z", amcl.pose.pose.position.z}
        }}
      }}
    }}
  };

  send_message(telemetry.dump());

  json status = {
    {"type", "STATUS_UPDATE"},
    {"roverId", assigned_rover_id_},
    {"payload", {
      {"status", "active"}
    }}
  };

  send_message(status.dump());
}

// ---------------- CPU Usage ----------------
double TelemetryClientAuth::get_cpu_usage()
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

  if (total_delta == 0) return 0.0;

  return 100.0 * (1.0 - (double)idle_delta / total_delta);
}

}  // namespace jetson_telemetry

// ---------------- main ----------------
int main(int argc, char **argv)
{
  rclcpp::init(argc, argv);

  try {
    auto node = std::make_shared<jetson_telemetry::TelemetryClientAuth>();
    rclcpp::spin(node);
  } catch (const std::exception& e) {
    std::cerr << "Exception in main: " << e.what() << std::endl;
    return 1;
  }

  rclcpp::shutdown();
  return 0;
}
