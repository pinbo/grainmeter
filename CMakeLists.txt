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

# --- Optional Qt GUI front-end ---
# Builds grainmeter-gui alongside the CLI whenever Qt6 Widgets can be
# found; entirely skipped (with a note, not an error) otherwise, so the
# CLI always builds regardless of whether Qt is installed.
option(GRAINMETER_BUILD_GUI "Build the Qt GUI front-end (grainmeter-gui)" ON)
if(GRAINMETER_BUILD_GUI)
  find_package(Qt6 QUIET COMPONENTS Widgets)
  if(Qt6_FOUND)
    message(STATUS "Qt6 found (${Qt6_VERSION}) -- building grainmeter-gui")
    add_subdirectory(gui)
  else()
    message(STATUS "Qt6 Widgets not found -- skipping grainmeter-gui "
                    "(install qt6-base-dev / brew install qt to enable it)")
  endif()
endif()

if(APPLE)
  # `cmake --build build --target standalone` as a shortcut for
  # scripts/package_macos_standalone.sh, which bundles OpenCV's dylibs
  # next to the built binary (and rewrites its load commands to use them)
  # so the result runs on a Mac without OpenCV installed. See the
  # README's "Distributing on macOS without OpenCV/Qt installed" section.
  # CLI-only -- use this if you don't need the GUI in what you're sharing.
  add_custom_target(standalone
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/scripts/package_macos_standalone.sh ${CMAKE_BINARY_DIR}
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    DEPENDS grainmeter
    COMMENT "Bundling OpenCV dependencies for standalone distribution"
    VERBATIM
  )

  # `cmake --build build --target dist` packages the GUI *and* CLI
  # together into a plain dist/ folder (scripts/package_macos_dist.sh) --
  # use this to hand someone a single folder that gives them both, with
  # no .dmg/hdiutil step involved. Only registered when the GUI actually
  # got built, since it has nothing to bundle otherwise.
  if(TARGET grainmeter-gui)
    add_custom_target(dist
      COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/scripts/package_macos_dist.sh ${CMAKE_BINARY_DIR}
      WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
      DEPENDS grainmeter grainmeter-gui
      COMMENT "Packaging GUI + CLI together into dist/"
      VERBATIM
    )
  endif()
endif()

if(WIN32 AND TARGET grainmeter-gui)
  # `cmake --build build --target dist` on Windows: the PowerShell
  # equivalent of the macOS dist/ packaging above -- copies both .exe
  # files and every DLL they need (OpenCV via vcpkg's applocal
  # deployment, Qt via windeployqt) into one flat dist/ folder. See the
  # README's "Distributing on Windows without OpenCV/Qt installed"
  # section.
  add_custom_target(dist
    COMMAND powershell -NoProfile -ExecutionPolicy Bypass
            -File ${CMAKE_CURRENT_SOURCE_DIR}/scripts/package_windows_dist.ps1
            -BuildDir ${CMAKE_BINARY_DIR} -Config $<CONFIG>
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    DEPENDS grainmeter grainmeter-gui
    COMMENT "Packaging GUI + CLI together into dist/"
    VERBATIM
  )
endif()
