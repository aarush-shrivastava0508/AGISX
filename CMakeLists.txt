cmake_minimum_required(VERSION 3.20)

project(AGISX VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Optimization flags for Apple Silicon M4
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O3 -march=native -ffast-math")
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Include directories (explicitly adding the Homebrew eigen3 path)
include_directories(
    src/common
    /opt/homebrew/include
    /opt/homebrew/include/eigen3
    /usr/local/include
    /usr/local/include/eigen3
    /usr/include/eigen3
)

link_directories(
    /opt/homebrew/lib
    /usr/local/lib
)

# Stage 1 Target
add_executable(stage1_threat_gen src/stage1_threat_gen/threat_generator_node.cpp)
target_link_libraries(stage1_threat_gen PRIVATE zmq pthread)

# Stage 2 Target
add_executable(stage2_ekf_tracker src/stage2_ekf_tracker/ekf_tracker_node.cpp)
target_link_libraries(stage2_ekf_tracker PRIVATE zmq pthread)

# Add Stage 3 C2 Brain Target
add_executable(stage3_c2_wta_brain src/stage3_c2_wta_brain/c2_wta_node.cpp)
target_link_libraries(stage3_c2_wta_brain PRIVATE zmq pthread)

# Add Stage 4 Kinetic Guidance Target
add_executable(stage4_kinetic_ppn src/stage4_kinetic_ppn/interceptor_guidance_node.cpp)
target_link_libraries(stage4_kinetic_ppn PRIVATE zmq pthread)