cmake_minimum_required(VERSION 3.10)
project(grainmeter CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# First, an unrestricted find just to learn the OpenCV major version.
find_package(OpenCV REQUIRED)

message(STATUS "OpenCV version:      ${OpenCV_VERSION}")
message(STATUS "OpenCV include dirs: ${OpenCV_INCLUDE_DIRS}")

find_package(Threads REQUIRED)

add_executable(grainmeter src/grainmeter.cpp)
# -isystem so these headers are searched, and searched first, ahead of any
# other opencv2/ headers that might be reachable via a conda/pip
# opencv-python install elsewhere on the include path.
target_include_directories(grainmeter SYSTEM BEFORE PRIVATE ${OpenCV_INCLUDE_DIRS})

# Link directly against the individual module targets we actually use
# (opencv_core, opencv_imgproc, opencv_imgcodecs, + opencv_geometry on
# OpenCV 5) instead of the aggregate ${OpenCV_LIBS}. This matters a lot for
# startup time: on some OpenCV packagings (e.g. Homebrew's "opencv"
# formula), a plain find_package(OpenCV REQUIRED) with no COMPONENTS
# resolves ${OpenCV_LIBS} to *every* module the local build has -- 50-60
# shared libraries (dnn, stitching, sfm, gapi, ...) -- all of which the
# dynamic linker loads at process startup before main() even runs. That's
# most of what makes even `--help` feel slow. (Some OpenCV packagings, e.g.
# Debian/Ubuntu's, don't actually shrink ${OpenCV_LIBS} in response to
# find_package COMPONENTS either, which is why we link the module targets
# by name here rather than relying on that variable.)
set(GRAINMETER_OPENCV_LIBS opencv_core opencv_imgproc opencv_imgcodecs)
if(OpenCV_VERSION_MAJOR GREATER_EQUAL 5)
  list(APPEND GRAINMETER_OPENCV_LIBS opencv_geometry)
endif()
message(STATUS "OpenCV libs (linked, restricted to used modules): ${GRAINMETER_OPENCV_LIBS}")

target_link_libraries(grainmeter PRIVATE ${GRAINMETER_OPENCV_LIBS} Threads::Threads)
