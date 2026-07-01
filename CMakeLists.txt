cmake_minimum_required(VERSION 3.10)
project(AegisFace)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find OpenCV with specific components only
find_package(OpenCV REQUIRED COMPONENTS core imgproc highgui videoio objdetect dnn)

include_directories(${OpenCV_INCLUDE_DIRS})

# Main executable (GUI version)
add_executable(live_test src/live_test.cpp)
target_link_libraries(live_test ${OpenCV_LIBS})

# Benchmark executable (CLI version)
add_executable(cli_benchmark src/cli_benchmark.cpp)
target_link_libraries(cli_benchmark ${OpenCV_LIBS})

# FaceAuth background service (Daemon)
add_executable(faceauth_daemon src/daemon.cpp)
target_link_libraries(faceauth_daemon ${OpenCV_LIBS})

# Pluggable Authentication Module (PAM Modülü - .so)
# We compile this SHARED library. No OpenCV linking needed, keeping it ultra-lightweight!
# We compile it as pure C to prevent C++ runtime/libstdc++ dynamic loading segfaults on dlclose.
add_library(pam_faceauth SHARED src/pam_faceauth.c)
target_link_libraries(pam_faceauth pam)


