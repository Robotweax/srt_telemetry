cmake_minimum_required(VERSION 3.21)

project(
    robotweax_srt_telemetry
    VERSION 0.1.0
    DESCRIPTION "Implementation-neutral telemetry foundation for Robotweax SRT and Haivision SRT"
    LANGUAGES C CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

option(ROBOTWEAX_SRT_TELEMETRY_BUILD_TESTS "Build telemetry tests" ON)
option(ROBOTWEAX_SRT_TELEMETRY_BUILD_PUBLIC_SRT_ADAPTER
    "Build the injected public SRT API adapter" OFF)
set(ROBOTWEAX_SRT_TELEMETRY_SRT_INCLUDE_DIR "" CACHE PATH
    "Directory containing srt/srt.h or the flat srt.h header")
set(ROBOTWEAX_SRT_TELEMETRY_SRT_LIBRARY "" CACHE FILEPATH
    "Optional SRT library used only by live adapter tests")
set(ROBOTWEAX_SRT_TELEMETRY_RUNTIME_TEST_PROVIDER "" CACHE STRING
    "Provider for live tests: robotweax-srt or haivision-srt")

if(ROBOTWEAX_SRT_TELEMETRY_BUILD_TESTS)
    enable_testing()
endif()

add_library(robotweax_srt_telemetry
    sdk/src/delta_engine.cpp
    sdk/src/recorder.cpp
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

if(ROBOTWEAX_SRT_TELEMETRY_BUILD_PUBLIC_SRT_ADAPTER)
    if(NOT ROBOTWEAX_SRT_TELEMETRY_SRT_INCLUDE_DIR)
        find_path(ROBOTWEAX_SRT_TELEMETRY_SRT_INCLUDE_DIR
            NAMES srt/srt.h srt.h)
    endif()
    if(NOT ROBOTWEAX_SRT_TELEMETRY_SRT_INCLUDE_DIR)
        message(FATAL_ERROR
            "Set ROBOTWEAX_SRT_TELEMETRY_SRT_INCLUDE_DIR to an SRT public header directory")
    endif()

    add_library(robotweax_srt_telemetry_public_srt_adapter
        adapters/public_srt/src/public_srt_adapter.cpp)
    add_library(RobotweaxSRTTelemetry::public_srt_adapter ALIAS
        robotweax_srt_telemetry_public_srt_adapter)
    target_compile_features(robotweax_srt_telemetry_public_srt_adapter
        PUBLIC cxx_std_20)
    target_include_directories(robotweax_srt_telemetry_public_srt_adapter
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/adapters/public_srt/include>
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
        PRIVATE ${ROBOTWEAX_SRT_TELEMETRY_SRT_INCLUDE_DIR})
    target_link_libraries(robotweax_srt_telemetry_public_srt_adapter
        PUBLIC RobotweaxSRTTelemetry::core)
    target_compile_definitions(robotweax_srt_telemetry_public_srt_adapter
        PRIVATE ROBOTWEAX_SRT_TELEMETRY_PUBLIC_SRT_BUILDING)
    set_target_properties(robotweax_srt_telemetry_public_srt_adapter PROPERTIES
        CXX_VISIBILITY_PRESET hidden
        EXPORT_NAME public_srt_adapter
        VISIBILITY_INLINES_HIDDEN YES
        VERSION ${PROJECT_VERSION}
        SOVERSION 0)

    if(EXISTS "${ROBOTWEAX_SRT_TELEMETRY_SRT_INCLUDE_DIR}/srt.h" AND
       NOT EXISTS "${ROBOTWEAX_SRT_TELEMETRY_SRT_INCLUDE_DIR}/srt/srt.h")
        target_compile_definitions(robotweax_srt_telemetry_public_srt_adapter
            PRIVATE ROBOTWEAX_SRT_TELEMETRY_USE_FLAT_SRT_HEADER)
    endif()
    if(BUILD_SHARED_LIBS)
        target_compile_definitions(robotweax_srt_telemetry_public_srt_adapter
            PUBLIC ROBOTWEAX_SRT_TELEMETRY_PUBLIC_SRT_SHARED)
    endif()
    if(MSVC)
        target_compile_options(robotweax_srt_telemetry_public_srt_adapter
            PRIVATE /W4 /permissive-)
    else()
        target_compile_options(robotweax_srt_telemetry_public_srt_adapter PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
    endif()

    if(ROBOTWEAX_SRT_TELEMETRY_BUILD_TESTS)
        add_executable(robotweax_srt_telemetry_public_srt_adapter_tests
            adapters/public_srt/tests/test_public_srt_adapter.cpp)
        target_include_directories(robotweax_srt_telemetry_public_srt_adapter_tests
            PRIVATE ${ROBOTWEAX_SRT_TELEMETRY_SRT_INCLUDE_DIR})
        target_link_libraries(robotweax_srt_telemetry_public_srt_adapter_tests
            PRIVATE RobotweaxSRTTelemetry::public_srt_adapter)
        target_compile_features(robotweax_srt_telemetry_public_srt_adapter_tests
            PRIVATE cxx_std_20)
        add_test(NAME robotweax_srt_telemetry_public_srt_adapter_tests
            COMMAND robotweax_srt_telemetry_public_srt_adapter_tests)

        if(ROBOTWEAX_SRT_TELEMETRY_SRT_LIBRARY)
            if(NOT ROBOTWEAX_SRT_TELEMETRY_RUNTIME_TEST_PROVIDER STREQUAL "robotweax-srt" AND
               NOT ROBOTWEAX_SRT_TELEMETRY_RUNTIME_TEST_PROVIDER STREQUAL "haivision-srt")
                message(FATAL_ERROR
                    "Set ROBOTWEAX_SRT_TELEMETRY_RUNTIME_TEST_PROVIDER to robotweax-srt or haivision-srt")
            endif()
            add_executable(robotweax_srt_telemetry_public_srt_runtime_tests
                adapters/public_srt/tests/test_public_srt_runtime.cpp)
            add_executable(robotweax_srt_telemetry_public_srt_incident_peer
                adapters/public_srt/tests/public_srt_incident_peer.cpp)
            target_include_directories(robotweax_srt_telemetry_public_srt_runtime_tests
                PRIVATE ${ROBOTWEAX_SRT_TELEMETRY_SRT_INCLUDE_DIR})
            target_include_directories(robotweax_srt_telemetry_public_srt_incident_peer
                PRIVATE ${ROBOTWEAX_SRT_TELEMETRY_SRT_INCLUDE_DIR})
            target_link_libraries(robotweax_srt_telemetry_public_srt_runtime_tests
                PRIVATE RobotweaxSRTTelemetry::public_srt_adapter
                        ${ROBOTWEAX_SRT_TELEMETRY_SRT_LIBRARY})
            target_link_libraries(robotweax_srt_telemetry_public_srt_incident_peer
                PRIVATE RobotweaxSRTTelemetry::public_srt_adapter
                        ${ROBOTWEAX_SRT_TELEMETRY_SRT_LIBRARY})
            target_compile_features(robotweax_srt_telemetry_public_srt_runtime_tests
                PRIVATE cxx_std_20)
            target_compile_features(robotweax_srt_telemetry_public_srt_incident_peer
                PRIVATE cxx_std_20)
            if(ROBOTWEAX_SRT_TELEMETRY_RUNTIME_TEST_PROVIDER STREQUAL "robotweax-srt")
                target_compile_definitions(robotweax_srt_telemetry_public_srt_runtime_tests
                    PRIVATE ROBOTWEAX_SRT_TELEMETRY_RUNTIME_PROVIDER_ROBOTWEAX)
                target_compile_definitions(robotweax_srt_telemetry_public_srt_incident_peer
                    PRIVATE ROBOTWEAX_SRT_TELEMETRY_RUNTIME_PROVIDER_ROBOTWEAX)
            endif()
            if(EXISTS "${ROBOTWEAX_SRT_TELEMETRY_SRT_INCLUDE_DIR}/srt.h" AND
               NOT EXISTS "${ROBOTWEAX_SRT_TELEMETRY_SRT_INCLUDE_DIR}/srt/srt.h")
                target_compile_definitions(robotweax_srt_telemetry_public_srt_runtime_tests
                    PRIVATE ROBOTWEAX_SRT_TELEMETRY_RUNTIME_USE_FLAT_SRT_HEADER)
                target_compile_definitions(robotweax_srt_telemetry_public_srt_incident_peer
                    PRIVATE ROBOTWEAX_SRT_TELEMETRY_RUNTIME_USE_FLAT_SRT_HEADER)
            endif()
            add_test(NAME robotweax_srt_telemetry_public_srt_runtime_tests
                COMMAND robotweax_srt_telemetry_public_srt_runtime_tests)
            add_test(NAME robotweax_srt_telemetry_public_srt_lifecycle_tests
                COMMAND robotweax_srt_telemetry_public_srt_runtime_tests --lifecycle)
            set_tests_properties(robotweax_srt_telemetry_public_srt_runtime_tests
                PROPERTIES TIMEOUT 15)
            set_tests_properties(robotweax_srt_telemetry_public_srt_lifecycle_tests
                PROPERTIES TIMEOUT 15)
        endif()
    endif()
endif()

if(MSVC)
    target_compile_options(robotweax_srt_telemetry PRIVATE /W4 /permissive-)
else()
    target_compile_options(robotweax_srt_telemetry PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
endif()

if(ROBOTWEAX_SRT_TELEMETRY_BUILD_TESTS)
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

    set(ROBOTWEAX_SRT_TELEMETRY_GOLDEN_INCIDENT_DIR
        "${CMAKE_CURRENT_SOURCE_DIR}/testdata/golden-incidents/incident-001-constant-loss")
    add_executable(robotweax_srt_telemetry_golden_incident_test
        sdk/tests/test_golden_incident.cpp)
    target_link_libraries(robotweax_srt_telemetry_golden_incident_test
        PRIVATE RobotweaxSRTTelemetry::core)
    target_compile_features(robotweax_srt_telemetry_golden_incident_test
        PRIVATE cxx_std_20)
    add_test(NAME robotweax_srt_telemetry_golden_incident_test
        COMMAND robotweax_srt_telemetry_golden_incident_test
            "${ROBOTWEAX_SRT_TELEMETRY_GOLDEN_INCIDENT_DIR}/samples.csv")

    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        add_test(NAME robotweax_srt_telemetry_golden_incident_manifest_test
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/qualification/validate_golden_incident.py"
                "${ROBOTWEAX_SRT_TELEMETRY_GOLDEN_INCIDENT_DIR}/manifest.json")
        add_test(NAME robotweax_srt_telemetry_golden_incident_reproducibility_test
            COMMAND "${Python3_EXECUTABLE}"
                "${ROBOTWEAX_SRT_TELEMETRY_GOLDEN_INCIDENT_DIR}/generate.py"
                --check)
    endif()
endif()

install(TARGETS robotweax_srt_telemetry
    EXPORT RobotweaxSRTTelemetryTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY sdk/include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

if(ROBOTWEAX_SRT_TELEMETRY_BUILD_PUBLIC_SRT_ADAPTER)
    install(TARGETS robotweax_srt_telemetry_public_srt_adapter
        EXPORT RobotweaxSRTTelemetryTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    install(DIRECTORY adapters/public_srt/include/
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
endif()

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
