cmake_minimum_required(VERSION 3.24)
project(quickshare LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Qt6 6.10 REQUIRED COMPONENTS Core Gui Widgets Quick Qml DBus)
qt_standard_project_setup(REQUIRES 6.10)
set(CMAKE_AUTORCC ON)

include(GNUInstallDirs)

# The Nearby side is built by Bazel: bazelisk build //sharing/linux/lib:quickshare
get_filename_component(NEARBY_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
set(QUICKSHARE_LIB_DIR "${NEARBY_ROOT}/bazel-bin/sharing/linux/lib"
    CACHE PATH "Directory containing libquickshare.so")
set(QUICKSHARE_INCLUDE_DIR "${NEARBY_ROOT}/sharing/linux/lib"
    CACHE PATH "Directory containing quickshare.h")

find_package(GTest QUIET)
if(NOT GTest_FOUND)
  include(FetchContent)
  FetchContent_Declare(googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.17.0.tar.gz)
  FetchContent_MakeAvailable(googletest)
endif()

add_library(quickshare_lib SHARED IMPORTED)
set_target_properties(quickshare_lib PROPERTIES
  IMPORTED_LOCATION "${QUICKSHARE_LIB_DIR}/libquickshare.so"
  IMPORTED_SONAME "libquickshare.so"
  INTERFACE_INCLUDE_DIRECTORIES "${QUICKSHARE_INCLUDE_DIR}")

# Everything except main(); linked against either libquickshare.so or, in the
# tests, a fake implementation of the C API.
add_library(quickshare_core STATIC
  application_controller.cc
  application_controller.h
  backend.cc
  backend.h
  file_dialogs.cc
  file_dialogs.h)
target_include_directories(quickshare_core PUBLIC
  "${NEARBY_ROOT}"
  "${QUICKSHARE_INCLUDE_DIR}")
target_link_libraries(quickshare_core PUBLIC
  Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Quick Qt6::Qml Qt6::DBus)

if(NOT EXISTS "${QUICKSHARE_LIB_DIR}/libquickshare.so")
  message(FATAL_ERROR
    "libquickshare.so not found in ${QUICKSHARE_LIB_DIR}. Run "
    "'bazelisk build //sharing/linux/lib:quickshare' or set QUICKSHARE_LIB_DIR.")
endif()

qt_add_executable(quickshare main.cc resources.qrc)
# Default for NEARBY_QML_HOT_RELOAD when NEARBY_QML_SOURCE_DIR is unset.
target_compile_definitions(quickshare PRIVATE
  QUICKSHARE_QML_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(quickshare PRIVATE quickshare_core quickshare_lib)
set_target_properties(quickshare PROPERTIES
  INSTALL_RPATH "$ORIGIN/../lib"
  WIN32_EXECUTABLE OFF)

install(TARGETS quickshare RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
# Fixed to "lib" so that $ORIGIN/../lib above finds it.
install(IMPORTED_RUNTIME_ARTIFACTS quickshare_lib LIBRARY DESTINATION lib)
install(FILES packaging/io.github.quickshare_linux.QuickShare.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES packaging/io.github.quickshare_linux.QuickShare.metainfo.xml
        DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)
# Desktop file, metainfo and icon are all named after the app id: Flatpak's
# build-export drops anything in /app/share that is not.
install(FILES icons/quickshare.svg
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps
        RENAME io.github.quickshare_linux.QuickShare.svg)

enable_testing()

function(quickshare_add_test name)
  qt_add_executable(${name} ${ARGN})
  target_link_libraries(${name} PRIVATE quickshare_core GTest::gtest_main)
  add_test(NAME ${name} COMMAND ${name})
  set_tests_properties(${name} PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endfunction()

quickshare_add_test(backend_test backend_test.cc fake_quickshare.cc)
quickshare_add_test(application_controller_test
  application_controller_test.cc fake_quickshare.cc)
