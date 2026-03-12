cmake_minimum_required(VERSION 3.13)
project(runner LANGUAGES C CXX)

# Define the application target. To change its name, change BINARY_NAME in the
# top-level CMakeLists.txt, not the value here, or `flutter run` will no longer
# work.
#
# Any new source files that you add to the application should be added here.
# Screen recorder native deps (must come before add_executable so IMPORTED targets exist)
find_package(PkgConfig REQUIRED)
pkg_check_modules(X11     REQUIRED IMPORTED_TARGET x11)
pkg_check_modules(XEXT    REQUIRED IMPORTED_TARGET xext)
pkg_check_modules(PULSE   REQUIRED IMPORTED_TARGET libpulse-simple)
pkg_check_modules(GST     REQUIRED IMPORTED_TARGET gstreamer-1.0)
pkg_check_modules(GST_APP REQUIRED IMPORTED_TARGET gstreamer-app-1.0)
pkg_check_modules(CAIRO   REQUIRED IMPORTED_TARGET cairo)
pkg_check_modules(CAIRO_XLIB REQUIRED IMPORTED_TARGET cairo-xlib)

# Build the C plugin as a separate static library so it gets C linkage
add_library(screen_recorder_native STATIC "screen_recorder_plugin.c")
target_include_directories(screen_recorder_native PRIVATE
  "${CMAKE_SOURCE_DIR}"
  "${CMAKE_CURRENT_SOURCE_DIR}"
)
target_link_libraries(screen_recorder_native PRIVATE
  PkgConfig::GTK
  PkgConfig::X11
  PkgConfig::XEXT
  PkgConfig::PULSE
  PkgConfig::GST
  PkgConfig::GST_APP
  PkgConfig::CAIRO
  PkgConfig::CAIRO_XLIB
)
# Flutter headers for the plugin
target_link_libraries(screen_recorder_native PRIVATE flutter)
target_compile_options(screen_recorder_native PRIVATE -Wall)

add_executable(${BINARY_NAME}
  "main.cc"
  "my_application.cc"
  "${FLUTTER_MANAGED_DIR}/generated_plugin_registrant.cc"
)

# Apply the standard set of build settings. This can be removed for applications
# that need different build settings.
apply_standard_settings(${BINARY_NAME})

# Add preprocessor definitions for the application ID.
add_definitions(-DAPPLICATION_ID="${APPLICATION_ID}")

# Add dependency libraries. Add any application-specific dependencies here.
target_link_libraries(${BINARY_NAME} PRIVATE flutter)
target_link_libraries(${BINARY_NAME} PRIVATE PkgConfig::GTK)

# Link our C plugin static lib + its transitive deps
target_link_libraries(${BINARY_NAME} PRIVATE
  screen_recorder_native
  PkgConfig::X11
  PkgConfig::XEXT
  PkgConfig::PULSE
  PkgConfig::GST
  PkgConfig::GST_APP
  PkgConfig::CAIRO
  PkgConfig::CAIRO_XLIB
)

target_include_directories(${BINARY_NAME} PRIVATE
  "${CMAKE_SOURCE_DIR}"
  "${CMAKE_CURRENT_SOURCE_DIR}"
)
