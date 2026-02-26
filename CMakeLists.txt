cmake_minimum_required(VERSION 3.16)
project(jetson_telemetry)

if(CMAKE_COMPILER_IS_GNUCXX OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# find dependencies
find_package(ament_cmake REQUIRED)
find_package(rclcpp REQUIRED)
find_package(rclcpp_action REQUIRED)
find_package(geometry_msgs REQUIRED)
find_package(sensor_msgs REQUIRED)
find_package(nav2_msgs REQUIRED)



add_executable(telemetry_client src/telemetry_client.cpp)

target_include_directories(telemetry_client PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)
ament_target_dependencies(
  telemetry_client
  rclcpp
  geometry_msgs
  sensor_msgs
)
target_link_libraries(telemetry_client pthread)

# ---- Second executable: authenticated version ----
add_executable(telemetry_client_auth src/telemetry_client_auth.cpp)

target_include_directories(telemetry_client_auth PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)
ament_target_dependencies(
  telemetry_client_auth
  rclcpp
  geometry_msgs
  sensor_msgs
)
target_link_libraries(telemetry_client_auth pthread)

# ---- Third executable: cancel Nav2 NavigateToPose goal ----
add_executable(nav2_cancel src/nav2_cancel.cpp)

target_include_directories(nav2_cancel PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)
ament_target_dependencies(
  nav2_cancel
  rclcpp
  rclcpp_action
  nav2_msgs
)

install(TARGETS
  telemetry_client
  telemetry_client_auth
  nav2_cancel
  DESTINATION lib/${PROJECT_NAME}
)
ament_package()
