cmake_minimum_required(VERSION 3.16)
project(jetson_telemetry)

if(CMAKE_COMPILER_IS_GNUCXX OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# find dependencies
find_package(ament_cmake REQUIRED)
find_package(rclcpp REQUIRED)
find_package(geometry_msgs REQUIRED)
find_package(sensor_msgs REQUIRED)



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
install(TARGETS
  telemetry_client
  DESTINATION lib/${PROJECT_NAME}
)
ament_package()
