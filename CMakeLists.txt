cmake_minimum_required(VERSION 3.21)

project(
    robotweax_srt_telemetry
    VERSION 0.1.0
    DESCRIPTION "Implementation-neutral telemetry foundation for Robotweax SRT and Haivision SRT"
    LANGUAGES C CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

option(ROBOTWEAX_SRT_TELEMETRY_BUILD_TESTS "Build telemetry tests" ON)

add_library(robotweax_srt_telemetry
    sdk/src/delta_engine.cpp
    sdk/src/version.cpp)
add_library(
    RobotweaxSRTTelemetry::core ALIAS robotweax_srt_telemetry)

target_compile_features(robotweax_srt_telemetry PUBLIC cxx_std_20)
target_include_directories(robotweax_srt_telemetry
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/sdk/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_compile_definitions(robotweax_srt_telemetry
    PRIVATE ROBOTWEAX_SRT_TELEMETRY_BUILDING)

set_target_properties(robotweax_srt_telemetry PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    EXPORT_NAME core
    VISIBILITY_INLINES_HIDDEN YES
    VERSION ${PROJECT_VERSION}
    SOVERSION 0)

if(BUILD_SHARED_LIBS)
    target_compile_definitions(robotweax_srt_telemetry
        PUBLIC ROBOTWEAX_SRT_TELEMETRY_SHARED)
endif()

if(MSVC)
    target_compile_options(robotweax_srt_telemetry PRIVATE /W4 /permissive-)
else()
    target_compile_options(robotweax_srt_telemetry PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
endif()

if(ROBOTWEAX_SRT_TELEMETRY_BUILD_TESTS)
    enable_testing()

    add_executable(robotweax_srt_telemetry_tests
        sdk/tests/test_delta_engine.cpp)
    target_link_libraries(robotweax_srt_telemetry_tests
        PRIVATE RobotweaxSRTTelemetry::core)
    target_compile_features(robotweax_srt_telemetry_tests PRIVATE cxx_std_20)
    add_test(NAME robotweax_srt_telemetry_tests
        COMMAND robotweax_srt_telemetry_tests)

    add_executable(robotweax_srt_telemetry_c_api_test
        sdk/tests/test_c_api.c)
    target_link_libraries(robotweax_srt_telemetry_c_api_test
        PRIVATE RobotweaxSRTTelemetry::core)
    add_test(NAME robotweax_srt_telemetry_c_api_test
        COMMAND robotweax_srt_telemetry_c_api_test)
endif()

install(TARGETS robotweax_srt_telemetry
    EXPORT RobotweaxSRTTelemetryTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY sdk/include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

configure_package_config_file(
    cmake/RobotweaxSRTTelemetryConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/RobotweaxSRTTelemetryConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/RobotweaxSRTTelemetry)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/RobotweaxSRTTelemetryConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)

install(EXPORT RobotweaxSRTTelemetryTargets
    NAMESPACE RobotweaxSRTTelemetry::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/RobotweaxSRTTelemetry)
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/RobotweaxSRTTelemetryConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/RobotweaxSRTTelemetryConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/RobotweaxSRTTelemetry)
