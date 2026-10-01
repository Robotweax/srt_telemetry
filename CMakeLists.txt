cmake_minimum_required(VERSION 3.21)

project(
    robotweax_srt_telemetry
    VERSION 0.1.0
    DESCRIPTION "Sender and receiver SRT stream telemetry for Robotweax SRT and Haivision SRT"
    HOMEPAGE_URL "https://github.com/Robotweax/srt_telemetry"
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

if(ROBOTWEAX_SRT_TELEMETRY_BUILD_TESTS)
    add_executable(robotweax_srt_telemetry_trigger_window_tests
        adapters/public_srt/tests/test_trigger_window.cpp)
    target_link_libraries(robotweax_srt_telemetry_trigger_window_tests
        PRIVATE RobotweaxSRTTelemetry::core)
    target_compile_features(robotweax_srt_telemetry_trigger_window_tests PRIVATE cxx_std_20)
    add_test(NAME robotweax_srt_telemetry_trigger_window_tests
        COMMAND robotweax_srt_telemetry_trigger_window_tests)
endif()

add_library(robotweax_srt_telemetry
    sdk/src/delta_engine.cpp
    sdk/src/metrics.cpp
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
        adapters/public_srt/src/public_srt_adapter.cpp
        adapters/public_srt/src/public_srt_connection.cpp)
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

        add_executable(robotweax_srt_telemetry_public_srt_connection_tests
            adapters/public_srt/tests/test_public_srt_connection.cpp)
        target_include_directories(robotweax_srt_telemetry_public_srt_connection_tests
            PRIVATE ${ROBOTWEAX_SRT_TELEMETRY_SRT_INCLUDE_DIR})
        target_link_libraries(robotweax_srt_telemetry_public_srt_connection_tests
            PRIVATE RobotweaxSRTTelemetry::public_srt_adapter)
        target_compile_features(robotweax_srt_telemetry_public_srt_connection_tests
            PRIVATE cxx_std_20)
        add_test(NAME robotweax_srt_telemetry_public_srt_connection_tests
            COMMAND robotweax_srt_telemetry_public_srt_connection_tests)

        if(ROBOTWEAX_SRT_TELEMETRY_SRT_LIBRARY)
            find_package(Threads REQUIRED)
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
                        ${ROBOTWEAX_SRT_TELEMETRY_SRT_LIBRARY}
                        Threads::Threads)
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
            add_test(NAME robotweax_srt_telemetry_public_srt_field_coverage_test
                COMMAND robotweax_srt_telemetry_public_srt_runtime_tests --field-coverage)
            add_test(NAME robotweax_srt_telemetry_public_srt_connection_events_test
                COMMAND robotweax_srt_telemetry_public_srt_runtime_tests --connection-events)
            find_package(Python3 COMPONENTS Interpreter QUIET)
            if(Python3_Interpreter_FOUND)
                add_test(NAME robotweax_srt_telemetry_public_srt_trigger_recording_test
                    COMMAND "${Python3_EXECUTABLE}"
                        "${CMAKE_CURRENT_SOURCE_DIR}/qualification/test_triggered_peer_recording.py"
                        --peer $<TARGET_FILE:robotweax_srt_telemetry_public_srt_incident_peer>
                        --provider "${ROBOTWEAX_SRT_TELEMETRY_RUNTIME_TEST_PROVIDER}")
                set_tests_properties(robotweax_srt_telemetry_public_srt_trigger_recording_test
                    PROPERTIES TIMEOUT 30
                        ENVIRONMENT "PYTHONPYCACHEPREFIX=${CMAKE_CURRENT_BINARY_DIR}/python-cache")
                add_test(NAME robotweax_srt_telemetry_public_srt_incident_delivery_test
                    COMMAND "${Python3_EXECUTABLE}"
                        "${CMAKE_CURRENT_SOURCE_DIR}/qualification/test_incident_peer_delivery.py"
                        --peer $<TARGET_FILE:robotweax_srt_telemetry_public_srt_incident_peer>
                        --provider "${ROBOTWEAX_SRT_TELEMETRY_RUNTIME_TEST_PROVIDER}")
                set_tests_properties(robotweax_srt_telemetry_public_srt_incident_delivery_test
                    PROPERTIES TIMEOUT 30
                        ENVIRONMENT "PYTHONPYCACHEPREFIX=${CMAKE_CURRENT_BINARY_DIR}/python-cache")
                add_test(NAME robotweax_srt_telemetry_public_srt_jsonl_recording_test
                    COMMAND "${Python3_EXECUTABLE}"
                        "${CMAKE_CURRENT_SOURCE_DIR}/qualification/test_jsonl_peer_recording.py"
                        --peer $<TARGET_FILE:robotweax_srt_telemetry_public_srt_incident_peer>
                        --provider "${ROBOTWEAX_SRT_TELEMETRY_RUNTIME_TEST_PROVIDER}"
                        --analyzer "${CMAKE_CURRENT_SOURCE_DIR}/tools/analyze_incident.py"
                        --work-dir "${CMAKE_CURRENT_BINARY_DIR}/jsonl-peer-recording")
                set_tests_properties(robotweax_srt_telemetry_public_srt_jsonl_recording_test
                    PROPERTIES
                        TIMEOUT 30
                        SKIP_RETURN_CODE 77
                        ENVIRONMENT
                            "PYTHONPYCACHEPREFIX=${CMAKE_CURRENT_BINARY_DIR}/python-cache")
            endif()
            set_tests_properties(robotweax_srt_telemetry_public_srt_runtime_tests
                PROPERTIES TIMEOUT 15)
            set_tests_properties(robotweax_srt_telemetry_public_srt_lifecycle_tests
                PROPERTIES TIMEOUT 15)
            set_tests_properties(robotweax_srt_telemetry_public_srt_field_coverage_test
                PROPERTIES TIMEOUT 15)
            set_tests_properties(robotweax_srt_telemetry_public_srt_connection_events_test
                PROPERTIES TIMEOUT 30)
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

    add_executable(robotweax_srt_telemetry_metrics_tests
        sdk/tests/test_metrics.cpp)
    target_link_libraries(robotweax_srt_telemetry_metrics_tests
        PRIVATE RobotweaxSRTTelemetry::core)
    target_compile_features(robotweax_srt_telemetry_metrics_tests PRIVATE cxx_std_20)
    add_test(NAME robotweax_srt_telemetry_metrics_tests
        COMMAND robotweax_srt_telemetry_metrics_tests)

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
    set(ROBOTWEAX_SRT_TELEMETRY_CAPTURED_GOLDEN_INCIDENT_DIR
        "${CMAKE_CURRENT_SOURCE_DIR}/testdata/golden-incidents/incident-002-captured-constant-loss-robotweax-haivision")
    add_test(NAME robotweax_srt_telemetry_captured_golden_incident_test
        COMMAND robotweax_srt_telemetry_golden_incident_test
            "${ROBOTWEAX_SRT_TELEMETRY_CAPTURED_GOLDEN_INCIDENT_DIR}/samples.csv")
    set(ROBOTWEAX_SRT_TELEMETRY_REVERSE_CAPTURED_GOLDEN_INCIDENT_DIR
        "${CMAKE_CURRENT_SOURCE_DIR}/testdata/golden-incidents/incident-003-captured-constant-loss-haivision-robotweax")
    add_test(NAME robotweax_srt_telemetry_reverse_captured_golden_incident_test
        COMMAND robotweax_srt_telemetry_golden_incident_test
            "${ROBOTWEAX_SRT_TELEMETRY_REVERSE_CAPTURED_GOLDEN_INCIDENT_DIR}/samples.csv")
    set(ROBOTWEAX_SRT_TELEMETRY_BURST_CAPTURED_GOLDEN_INCIDENT_DIR
        "${CMAKE_CURRENT_SOURCE_DIR}/testdata/golden-incidents/incident-004-captured-burst-loss-robotweax-haivision")
    add_test(NAME robotweax_srt_telemetry_burst_captured_golden_incident_test
        COMMAND robotweax_srt_telemetry_golden_incident_test
            "${ROBOTWEAX_SRT_TELEMETRY_BURST_CAPTURED_GOLDEN_INCIDENT_DIR}/samples.csv")
    set(ROBOTWEAX_SRT_TELEMETRY_REVERSE_BURST_CAPTURED_GOLDEN_INCIDENT_DIR
        "${CMAKE_CURRENT_SOURCE_DIR}/testdata/golden-incidents/incident-005-captured-burst-loss-haivision-robotweax")
    add_test(NAME robotweax_srt_telemetry_reverse_burst_captured_golden_incident_test
        COMMAND robotweax_srt_telemetry_golden_incident_test
            "${ROBOTWEAX_SRT_TELEMETRY_REVERSE_BURST_CAPTURED_GOLDEN_INCIDENT_DIR}/samples.csv")
    set(ROBOTWEAX_SRT_TELEMETRY_JSONL_CAPTURED_GOLDEN_INCIDENT_DIR
        "${CMAKE_CURRENT_SOURCE_DIR}/testdata/golden-incidents/incident-006-jsonl-constant-loss-robotweax-haivision")
    add_test(NAME robotweax_srt_telemetry_jsonl_captured_golden_incident_test
        COMMAND robotweax_srt_telemetry_golden_incident_test
            "${ROBOTWEAX_SRT_TELEMETRY_JSONL_CAPTURED_GOLDEN_INCIDENT_DIR}/samples.csv")
    set(ROBOTWEAX_SRT_TELEMETRY_REVERSE_JSONL_CAPTURED_GOLDEN_INCIDENT_DIR
        "${CMAKE_CURRENT_SOURCE_DIR}/testdata/golden-incidents/incident-007-jsonl-constant-loss-haivision-robotweax")
    add_test(NAME robotweax_srt_telemetry_reverse_jsonl_captured_golden_incident_test
        COMMAND robotweax_srt_telemetry_golden_incident_test
            "${ROBOTWEAX_SRT_TELEMETRY_REVERSE_JSONL_CAPTURED_GOLDEN_INCIDENT_DIR}/samples.csv")
    set(ROBOTWEAX_SRT_TELEMETRY_RTT_STEP_GOLDEN_INCIDENT_DIR
        "${CMAKE_CURRENT_SOURCE_DIR}/testdata/golden-incidents/incident-008-rtt-step-robotweax-haivision")
    add_test(NAME robotweax_srt_telemetry_rtt_step_golden_incident_test
        COMMAND robotweax_srt_telemetry_golden_incident_test
            "${ROBOTWEAX_SRT_TELEMETRY_RTT_STEP_GOLDEN_INCIDENT_DIR}/samples.csv")
    set(ROBOTWEAX_SRT_TELEMETRY_REVERSE_RTT_STEP_GOLDEN_INCIDENT_DIR
        "${CMAKE_CURRENT_SOURCE_DIR}/testdata/golden-incidents/incident-009-rtt-step-haivision-robotweax")
    add_test(NAME robotweax_srt_telemetry_reverse_rtt_step_golden_incident_test
        COMMAND robotweax_srt_telemetry_golden_incident_test
            "${ROBOTWEAX_SRT_TELEMETRY_REVERSE_RTT_STEP_GOLDEN_INCIDENT_DIR}/samples.csv")
    set(ROBOTWEAX_SRT_TELEMETRY_RECEIVER_PAUSE_GOLDEN_INCIDENT_DIR
        "${CMAKE_CURRENT_SOURCE_DIR}/testdata/golden-incidents/incident-010-receiver-pause-robotweax-haivision")
    add_test(NAME robotweax_srt_telemetry_receiver_pause_golden_incident_test
        COMMAND robotweax_srt_telemetry_golden_incident_test
            "${ROBOTWEAX_SRT_TELEMETRY_RECEIVER_PAUSE_GOLDEN_INCIDENT_DIR}/samples.csv")
    set(ROBOTWEAX_SRT_TELEMETRY_REVERSE_RECEIVER_PAUSE_GOLDEN_INCIDENT_DIR
        "${CMAKE_CURRENT_SOURCE_DIR}/testdata/golden-incidents/incident-011-receiver-pause-haivision-robotweax")
    add_test(NAME robotweax_srt_telemetry_reverse_receiver_pause_golden_incident_test
        COMMAND robotweax_srt_telemetry_golden_incident_test
            "${ROBOTWEAX_SRT_TELEMETRY_REVERSE_RECEIVER_PAUSE_GOLDEN_INCIDENT_DIR}/samples.csv")
    set(ROBOTWEAX_SRT_TELEMETRY_BANDWIDTH_REDUCTION_GOLDEN_INCIDENT_DIR
        "${CMAKE_CURRENT_SOURCE_DIR}/testdata/golden-incidents/incident-012-bandwidth-reduction-robotweax-haivision")
    add_test(NAME robotweax_srt_telemetry_bandwidth_reduction_golden_incident_test
        COMMAND robotweax_srt_telemetry_golden_incident_test
            "${ROBOTWEAX_SRT_TELEMETRY_BANDWIDTH_REDUCTION_GOLDEN_INCIDENT_DIR}/samples.csv")
    set(ROBOTWEAX_SRT_TELEMETRY_REVERSE_BANDWIDTH_REDUCTION_GOLDEN_INCIDENT_DIR
        "${CMAKE_CURRENT_SOURCE_DIR}/testdata/golden-incidents/incident-013-bandwidth-reduction-haivision-robotweax")
    add_test(NAME robotweax_srt_telemetry_reverse_bandwidth_reduction_golden_incident_test
        COMMAND robotweax_srt_telemetry_golden_incident_test
            "${ROBOTWEAX_SRT_TELEMETRY_REVERSE_BANDWIDTH_REDUCTION_GOLDEN_INCIDENT_DIR}/samples.csv")
    set(ROBOTWEAX_SRT_TELEMETRY_RECOVERY_MARGIN_GOLDEN_INCIDENT_DIR
        "${CMAKE_CURRENT_SOURCE_DIR}/testdata/golden-incidents/incident-014-recovery-margin-robotweax-haivision")
    add_test(NAME robotweax_srt_telemetry_recovery_margin_golden_incident_test
        COMMAND robotweax_srt_telemetry_golden_incident_test
            "${ROBOTWEAX_SRT_TELEMETRY_RECOVERY_MARGIN_GOLDEN_INCIDENT_DIR}/samples.csv")
    set(ROBOTWEAX_SRT_TELEMETRY_REVERSE_RECOVERY_MARGIN_GOLDEN_INCIDENT_DIR
        "${CMAKE_CURRENT_SOURCE_DIR}/testdata/golden-incidents/incident-015-recovery-margin-haivision-robotweax")
    add_test(NAME robotweax_srt_telemetry_reverse_recovery_margin_golden_incident_test
        COMMAND robotweax_srt_telemetry_golden_incident_test
            "${ROBOTWEAX_SRT_TELEMETRY_REVERSE_RECOVERY_MARGIN_GOLDEN_INCIDENT_DIR}/samples.csv")

    add_executable(robotweax_srt_telemetry_generate_jsonl_incident
        sdk/tests/generate_jsonl_incident.cpp)
    target_link_libraries(robotweax_srt_telemetry_generate_jsonl_incident
        PRIVATE RobotweaxSRTTelemetry::core)
    target_compile_features(robotweax_srt_telemetry_generate_jsonl_incident
        PRIVATE cxx_std_20)
    set(ROBOTWEAX_SRT_TELEMETRY_JSONL_FIXTURE_DIR
        "${CMAKE_CURRENT_BINARY_DIR}/incident-analysis-fixtures")
    file(MAKE_DIRECTORY "${ROBOTWEAX_SRT_TELEMETRY_JSONL_FIXTURE_DIR}")
    set(ROBOTWEAX_SRT_TELEMETRY_SENDER_JSONL_FIXTURE
        "${ROBOTWEAX_SRT_TELEMETRY_JSONL_FIXTURE_DIR}/sender.jsonl")
    set(ROBOTWEAX_SRT_TELEMETRY_RECEIVER_JSONL_FIXTURE
        "${ROBOTWEAX_SRT_TELEMETRY_JSONL_FIXTURE_DIR}/receiver.jsonl")
    add_test(NAME robotweax_srt_telemetry_jsonl_incident_fixture_test
        COMMAND robotweax_srt_telemetry_generate_jsonl_incident
            "${ROBOTWEAX_SRT_TELEMETRY_SENDER_JSONL_FIXTURE}"
            "${ROBOTWEAX_SRT_TELEMETRY_RECEIVER_JSONL_FIXTURE}")

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
        add_test(NAME robotweax_srt_telemetry_captured_golden_incident_manifest_test
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/qualification/validate_golden_incident.py"
                "${ROBOTWEAX_SRT_TELEMETRY_CAPTURED_GOLDEN_INCIDENT_DIR}/manifest.json")
        add_test(NAME robotweax_srt_telemetry_reverse_captured_golden_incident_manifest_test
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/qualification/validate_golden_incident.py"
                "${ROBOTWEAX_SRT_TELEMETRY_REVERSE_CAPTURED_GOLDEN_INCIDENT_DIR}/manifest.json")
        add_test(NAME robotweax_srt_telemetry_burst_captured_golden_incident_manifest_test
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/qualification/validate_golden_incident.py"
                "${ROBOTWEAX_SRT_TELEMETRY_BURST_CAPTURED_GOLDEN_INCIDENT_DIR}/manifest.json")
        add_test(NAME robotweax_srt_telemetry_reverse_burst_captured_golden_incident_manifest_test
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/qualification/validate_golden_incident.py"
                "${ROBOTWEAX_SRT_TELEMETRY_REVERSE_BURST_CAPTURED_GOLDEN_INCIDENT_DIR}/manifest.json")
        add_test(NAME robotweax_srt_telemetry_jsonl_captured_golden_incident_manifest_test
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/qualification/validate_golden_incident.py"
                "${ROBOTWEAX_SRT_TELEMETRY_JSONL_CAPTURED_GOLDEN_INCIDENT_DIR}/manifest.json")
        add_test(NAME robotweax_srt_telemetry_reverse_jsonl_captured_golden_incident_manifest_test
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/qualification/validate_golden_incident.py"
                "${ROBOTWEAX_SRT_TELEMETRY_REVERSE_JSONL_CAPTURED_GOLDEN_INCIDENT_DIR}/manifest.json")
        add_test(NAME robotweax_srt_telemetry_rtt_step_golden_incident_manifest_test
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/qualification/validate_golden_incident.py"
                "${ROBOTWEAX_SRT_TELEMETRY_RTT_STEP_GOLDEN_INCIDENT_DIR}/manifest.json")
        add_test(NAME robotweax_srt_telemetry_reverse_rtt_step_golden_incident_manifest_test
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/qualification/validate_golden_incident.py"
                "${ROBOTWEAX_SRT_TELEMETRY_REVERSE_RTT_STEP_GOLDEN_INCIDENT_DIR}/manifest.json")
        add_test(NAME robotweax_srt_telemetry_receiver_pause_golden_incident_manifest_test
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/qualification/validate_golden_incident.py"
                "${ROBOTWEAX_SRT_TELEMETRY_RECEIVER_PAUSE_GOLDEN_INCIDENT_DIR}/manifest.json")
        add_test(NAME robotweax_srt_telemetry_reverse_receiver_pause_golden_incident_manifest_test
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/qualification/validate_golden_incident.py"
                "${ROBOTWEAX_SRT_TELEMETRY_REVERSE_RECEIVER_PAUSE_GOLDEN_INCIDENT_DIR}/manifest.json")
        add_test(NAME robotweax_srt_telemetry_bandwidth_reduction_golden_incident_manifest_test
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/qualification/validate_golden_incident.py"
                "${ROBOTWEAX_SRT_TELEMETRY_BANDWIDTH_REDUCTION_GOLDEN_INCIDENT_DIR}/manifest.json")
        add_test(NAME robotweax_srt_telemetry_reverse_bandwidth_reduction_golden_incident_manifest_test
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/qualification/validate_golden_incident.py"
                "${ROBOTWEAX_SRT_TELEMETRY_REVERSE_BANDWIDTH_REDUCTION_GOLDEN_INCIDENT_DIR}/manifest.json")
        add_test(NAME robotweax_srt_telemetry_recovery_margin_golden_incident_manifest_test
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/qualification/validate_golden_incident.py"
                "${ROBOTWEAX_SRT_TELEMETRY_RECOVERY_MARGIN_GOLDEN_INCIDENT_DIR}/manifest.json")
        add_test(NAME robotweax_srt_telemetry_reverse_recovery_margin_golden_incident_manifest_test
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/qualification/validate_golden_incident.py"
                "${ROBOTWEAX_SRT_TELEMETRY_REVERSE_RECOVERY_MARGIN_GOLDEN_INCIDENT_DIR}/manifest.json")
        add_test(NAME robotweax_srt_telemetry_loss_shape_observability_result_test
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/qualification/validate_loss_shape_observability.py"
                "${CMAKE_CURRENT_SOURCE_DIR}/qualification/results/2026-08-29-macos-arm64-loss-shape-observability.json"
                --runtime-qualification
                "${CMAKE_CURRENT_SOURCE_DIR}/qualification/results/2026-08-28-macos-arm64-public-srt-runtime.json")
        add_test(NAME robotweax_srt_telemetry_linux_lab_replay_test
            COMMAND "${Python3_EXECUTABLE}" -B
                "${CMAKE_CURRENT_SOURCE_DIR}/qualification/test_linux_lab_replay.py")
        add_test(NAME robotweax_srt_telemetry_netem_receiver_buffer_test
            COMMAND "${Python3_EXECUTABLE}" -B
                "${CMAKE_CURRENT_SOURCE_DIR}/qualification/test_netem_receiver_buffer.py")
        add_test(NAME robotweax_srt_telemetry_incident_analysis_test
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/qualification/test_incident_analysis.py"
                --analyzer "${CMAKE_CURRENT_SOURCE_DIR}/tools/analyze_incident.py"
                --jsonl
                "${ROBOTWEAX_SRT_TELEMETRY_SENDER_JSONL_FIXTURE}"
                "${ROBOTWEAX_SRT_TELEMETRY_RECEIVER_JSONL_FIXTURE}"
                --manifest "${ROBOTWEAX_SRT_TELEMETRY_GOLDEN_INCIDENT_DIR}/manifest.json"
                --manifest "${ROBOTWEAX_SRT_TELEMETRY_CAPTURED_GOLDEN_INCIDENT_DIR}/manifest.json"
                --manifest "${ROBOTWEAX_SRT_TELEMETRY_REVERSE_CAPTURED_GOLDEN_INCIDENT_DIR}/manifest.json"
                --manifest "${ROBOTWEAX_SRT_TELEMETRY_BURST_CAPTURED_GOLDEN_INCIDENT_DIR}/manifest.json"
                --manifest "${ROBOTWEAX_SRT_TELEMETRY_REVERSE_BURST_CAPTURED_GOLDEN_INCIDENT_DIR}/manifest.json"
                --manifest "${ROBOTWEAX_SRT_TELEMETRY_JSONL_CAPTURED_GOLDEN_INCIDENT_DIR}/manifest.json"
                --manifest "${ROBOTWEAX_SRT_TELEMETRY_REVERSE_JSONL_CAPTURED_GOLDEN_INCIDENT_DIR}/manifest.json"
                --manifest "${ROBOTWEAX_SRT_TELEMETRY_RTT_STEP_GOLDEN_INCIDENT_DIR}/manifest.json"
                --manifest "${ROBOTWEAX_SRT_TELEMETRY_REVERSE_RTT_STEP_GOLDEN_INCIDENT_DIR}/manifest.json"
                --manifest "${ROBOTWEAX_SRT_TELEMETRY_RECEIVER_PAUSE_GOLDEN_INCIDENT_DIR}/manifest.json"
                --manifest "${ROBOTWEAX_SRT_TELEMETRY_REVERSE_RECEIVER_PAUSE_GOLDEN_INCIDENT_DIR}/manifest.json"
                --manifest "${ROBOTWEAX_SRT_TELEMETRY_BANDWIDTH_REDUCTION_GOLDEN_INCIDENT_DIR}/manifest.json"
                --manifest "${ROBOTWEAX_SRT_TELEMETRY_REVERSE_BANDWIDTH_REDUCTION_GOLDEN_INCIDENT_DIR}/manifest.json"
                --manifest "${ROBOTWEAX_SRT_TELEMETRY_RECOVERY_MARGIN_GOLDEN_INCIDENT_DIR}/manifest.json"
                --manifest "${ROBOTWEAX_SRT_TELEMETRY_REVERSE_RECOVERY_MARGIN_GOLDEN_INCIDENT_DIR}/manifest.json"
                --expected-analysis
                "${ROBOTWEAX_SRT_TELEMETRY_GOLDEN_INCIDENT_DIR}/analysis.json")
        set_tests_properties(robotweax_srt_telemetry_incident_analysis_test
            PROPERTIES
                DEPENDS robotweax_srt_telemetry_jsonl_incident_fixture_test
                ENVIRONMENT
                    "PYTHONPYCACHEPREFIX=${CMAKE_CURRENT_BINARY_DIR}/python-cache")
        add_test(NAME robotweax_srt_telemetry_operator_evaluation_test
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/qualification/test_operator_evaluation.py"
                --tool "${CMAKE_CURRENT_SOURCE_DIR}/tools/operator_evaluation.py"
                --analyzer "${CMAKE_CURRENT_SOURCE_DIR}/tools/analyze_incident.py"
                --manifest
                "${ROBOTWEAX_SRT_TELEMETRY_JSONL_CAPTURED_GOLDEN_INCIDENT_DIR}/manifest.json"
                --manifest
                "${ROBOTWEAX_SRT_TELEMETRY_RTT_STEP_GOLDEN_INCIDENT_DIR}/manifest.json"
                --manifest
                "${ROBOTWEAX_SRT_TELEMETRY_RECEIVER_PAUSE_GOLDEN_INCIDENT_DIR}/manifest.json"
                --manifest
                "${ROBOTWEAX_SRT_TELEMETRY_REVERSE_RECEIVER_PAUSE_GOLDEN_INCIDENT_DIR}/manifest.json"
                --manifest
                "${ROBOTWEAX_SRT_TELEMETRY_BANDWIDTH_REDUCTION_GOLDEN_INCIDENT_DIR}/manifest.json"
                --manifest
                "${ROBOTWEAX_SRT_TELEMETRY_REVERSE_BANDWIDTH_REDUCTION_GOLDEN_INCIDENT_DIR}/manifest.json"
                --manifest
                "${ROBOTWEAX_SRT_TELEMETRY_RECOVERY_MARGIN_GOLDEN_INCIDENT_DIR}/manifest.json"
                --manifest
                "${ROBOTWEAX_SRT_TELEMETRY_REVERSE_RECOVERY_MARGIN_GOLDEN_INCIDENT_DIR}/manifest.json"
                --work-dir "${CMAKE_CURRENT_BINARY_DIR}/operator-evaluation-fixtures")
        set_tests_properties(robotweax_srt_telemetry_operator_evaluation_test
            PROPERTIES
                ENVIRONMENT
                    "PYTHONPYCACHEPREFIX=${CMAKE_CURRENT_BINARY_DIR}/python-cache")
        set(ROBOTWEAX_SRT_TELEMETRY_REHEARSAL_PACKET_DIR
            "${CMAKE_CURRENT_SOURCE_DIR}/qualification/operator-evaluation/five-class-internal-rehearsal-v1")
        add_test(NAME robotweax_srt_telemetry_operator_reference_review_test
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/qualification/test_operator_reference_review.py")
        add_test(NAME robotweax_srt_telemetry_operator_rehearsal_readiness_test
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/operator_evaluation.py"
                rehearsal
                --study
                "${ROBOTWEAX_SRT_TELEMETRY_REHEARSAL_PACKET_DIR}/study.json"
                --check
                "${ROBOTWEAX_SRT_TELEMETRY_REHEARSAL_PACKET_DIR}/rehearsal-readiness.json")
        set_tests_properties(robotweax_srt_telemetry_operator_rehearsal_readiness_test
            PROPERTIES
                ENVIRONMENT
                    "PYTHONPYCACHEPREFIX=${CMAKE_CURRENT_BINARY_DIR}/python-cache")
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

if(BUILD_SHARED_LIBS)
    set(ROBOTWEAX_SRT_TELEMETRY_STATIC OFF)
else()
    set(ROBOTWEAX_SRT_TELEMETRY_STATIC ON)
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
install(FILES LICENSE
    DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/robotweax_srt_telemetry)
