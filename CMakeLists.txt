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
option(ROBOTWEAX_SRT_TELEMETRY_REQUIRE_OTLP_PROTO_TEST
    "Fail configuration when the OTLP protobuf test cannot run (CI)" OFF)
option(ROBOTWEAX_SRT_TELEMETRY_REQUIRE_GROUP_TEST
    "Fail, instead of skip, the live group test when the SRT library has no groups" OFF)
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
    add_test(NAME robotweax_srt_telemetry_enum_declaration_check
        COMMAND ${CMAKE_COMMAND}
            "-DHEADER_DIRS=${CMAKE_CURRENT_SOURCE_DIR}/sdk/include;${CMAKE_CURRENT_SOURCE_DIR}/adapters/public_srt/include;${CMAKE_CURRENT_SOURCE_DIR}/server/include"
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/CheckEnumDeclarations.cmake)
endif()

add_library(robotweax_srt_telemetry
    sdk/src/delta_engine.cpp
    sdk/src/export.cpp
    sdk/src/health.cpp
    sdk/src/history.cpp
    sdk/src/metrics.cpp
    sdk/src/recorder.cpp
    sdk/src/sampler.cpp
    sdk/src/version.cpp)
add_library(
    RobotweaxSRTTelemetry::core ALIAS robotweax_srt_telemetry)

target_compile_features(robotweax_srt_telemetry PUBLIC cxx_std_20)
find_package(Threads REQUIRED)
target_link_libraries(robotweax_srt_telemetry PUBLIC Threads::Threads)
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

if(WIN32)
    set(ROBOTWEAX_SRT_TELEMETRY_HTTP_DEFAULT OFF)
else()
    set(ROBOTWEAX_SRT_TELEMETRY_HTTP_DEFAULT ON)
endif()
option(ROBOTWEAX_SRT_TELEMETRY_BUILD_HTTP_SERVER
    "Build the optional embedded HTTP/WebSocket server (POSIX)" ${ROBOTWEAX_SRT_TELEMETRY_HTTP_DEFAULT})
if(ROBOTWEAX_SRT_TELEMETRY_BUILD_HTTP_SERVER)
    # The built-in web UI, embedded in the library (no file serving).
    set(ROBOTWEAX_SRT_TELEMETRY_UI_FILES index.html app.css app.js favicon.svg)
    set(ui_sources "")
    foreach(ui_file IN LISTS ROBOTWEAX_SRT_TELEMETRY_UI_FILES)
        list(APPEND ui_sources "${CMAKE_CURRENT_SOURCE_DIR}/server/ui/${ui_file}")
    endforeach()
    list(JOIN ROBOTWEAX_SRT_TELEMETRY_UI_FILES "," ui_file_list)
    set(ui_generated "${CMAKE_CURRENT_BINARY_DIR}/generated/ui_assets.cpp")
    add_custom_command(
        OUTPUT "${ui_generated}"
        COMMAND ${CMAKE_COMMAND}
            "-DOUTPUT=${ui_generated}"
            "-DUI_DIR=${CMAKE_CURRENT_SOURCE_DIR}/server/ui"
            "-DFILES=${ui_file_list}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedUiAssets.cmake"
        DEPENDS ${ui_sources} "${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedUiAssets.cmake"
        COMMENT "Embedding the web UI"
        VERBATIM)
    add_library(robotweax_srt_telemetry_http server/src/http_server.cpp server/src/otlp_exporter.cpp "${ui_generated}")
    target_include_directories(robotweax_srt_telemetry_http PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/server/src")
    add_library(RobotweaxSRTTelemetry::http ALIAS robotweax_srt_telemetry_http)
    target_compile_features(robotweax_srt_telemetry_http PUBLIC cxx_std_20)
    target_include_directories(robotweax_srt_telemetry_http
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/server/include>
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
    target_link_libraries(robotweax_srt_telemetry_http PUBLIC RobotweaxSRTTelemetry::core)
    target_compile_definitions(robotweax_srt_telemetry_http PRIVATE ROBOTWEAX_SRT_TELEMETRY_HTTP_BUILDING)
    set_target_properties(robotweax_srt_telemetry_http PROPERTIES
        CXX_VISIBILITY_PRESET hidden
        EXPORT_NAME http
        VISIBILITY_INLINES_HIDDEN YES
        VERSION ${PROJECT_VERSION}
        SOVERSION 0)
    if(BUILD_SHARED_LIBS)
        target_compile_definitions(robotweax_srt_telemetry_http PUBLIC ROBOTWEAX_SRT_TELEMETRY_HTTP_SHARED)
    endif()
    if(NOT MSVC)
        target_compile_options(robotweax_srt_telemetry_http PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
    endif()
    if(ROBOTWEAX_SRT_TELEMETRY_BUILD_TESTS)
        add_executable(robotweax_srt_telemetry_http_tests server/tests/test_http_server.cpp)
        target_link_libraries(robotweax_srt_telemetry_http_tests PRIVATE RobotweaxSRTTelemetry::http)
        target_compile_features(robotweax_srt_telemetry_http_tests PRIVATE cxx_std_20)
        add_test(NAME robotweax_srt_telemetry_http_tests COMMAND robotweax_srt_telemetry_http_tests)
        set_tests_properties(robotweax_srt_telemetry_http_tests PROPERTIES TIMEOUT 60)
        add_executable(robotweax_srt_telemetry_otlp_exporter_tests server/tests/test_otlp_exporter.cpp)
        target_link_libraries(robotweax_srt_telemetry_otlp_exporter_tests PRIVATE RobotweaxSRTTelemetry::http)
        target_compile_features(robotweax_srt_telemetry_otlp_exporter_tests PRIVATE cxx_std_20)
        add_test(NAME robotweax_srt_telemetry_otlp_exporter_tests COMMAND robotweax_srt_telemetry_otlp_exporter_tests)
        set_tests_properties(robotweax_srt_telemetry_otlp_exporter_tests PROPERTIES TIMEOUT 60)
    endif()
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
        adapters/public_srt/src/public_srt_connection.cpp
        adapters/public_srt/src/public_srt_group.cpp)
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

    # Embedding layer: sampler, HTTP server, OTLP and socket monitoring in a
    # few host calls (docs/architecture/embedding.md).
    if(TARGET robotweax_srt_telemetry_http)
        add_library(robotweax_srt_telemetry_embed adapters/public_srt/src/embed.cpp)
        add_library(RobotweaxSRTTelemetry::embed ALIAS robotweax_srt_telemetry_embed)
        target_compile_features(robotweax_srt_telemetry_embed PUBLIC cxx_std_20)
        target_link_libraries(robotweax_srt_telemetry_embed
            PUBLIC RobotweaxSRTTelemetry::public_srt_adapter RobotweaxSRTTelemetry::http)
        find_package(Threads REQUIRED)
        target_link_libraries(robotweax_srt_telemetry_embed PRIVATE Threads::Threads)
        target_compile_definitions(robotweax_srt_telemetry_embed
            PRIVATE ROBOTWEAX_SRT_TELEMETRY_EMBED_BUILDING)
        set_target_properties(robotweax_srt_telemetry_embed PROPERTIES
            CXX_VISIBILITY_PRESET hidden
            EXPORT_NAME embed
            VISIBILITY_INLINES_HIDDEN YES
            VERSION ${PROJECT_VERSION}
            SOVERSION 0)
        if(BUILD_SHARED_LIBS)
            target_compile_definitions(robotweax_srt_telemetry_embed
                PUBLIC ROBOTWEAX_SRT_TELEMETRY_EMBED_SHARED)
        endif()
        if(NOT MSVC)
            target_compile_options(robotweax_srt_telemetry_embed PRIVATE
                -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
        endif()
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

        add_executable(robotweax_srt_telemetry_public_srt_group_tests
            adapters/public_srt/tests/test_public_srt_group.cpp)
        target_include_directories(robotweax_srt_telemetry_public_srt_group_tests
            PRIVATE ${ROBOTWEAX_SRT_TELEMETRY_SRT_INCLUDE_DIR})
        target_link_libraries(robotweax_srt_telemetry_public_srt_group_tests
            PRIVATE RobotweaxSRTTelemetry::public_srt_adapter)
        target_compile_features(robotweax_srt_telemetry_public_srt_group_tests
            PRIVATE cxx_std_20)
        add_test(NAME robotweax_srt_telemetry_public_srt_group_tests
            COMMAND robotweax_srt_telemetry_public_srt_group_tests)

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
            add_executable(robotweax_srt_telemetry_public_srt_fault_lab
                adapters/public_srt/tests/fault_lab.cpp)
            target_include_directories(robotweax_srt_telemetry_public_srt_fault_lab
                PRIVATE ${ROBOTWEAX_SRT_TELEMETRY_SRT_INCLUDE_DIR})
            target_link_libraries(robotweax_srt_telemetry_public_srt_fault_lab
                PRIVATE RobotweaxSRTTelemetry::public_srt_adapter
                        ${ROBOTWEAX_SRT_TELEMETRY_SRT_LIBRARY}
                        Threads::Threads)
            target_compile_features(robotweax_srt_telemetry_public_srt_fault_lab PRIVATE cxx_std_20)
            if(ROBOTWEAX_SRT_TELEMETRY_RUNTIME_TEST_PROVIDER STREQUAL "robotweax-srt")
                target_compile_definitions(robotweax_srt_telemetry_public_srt_fault_lab
                    PRIVATE ROBOTWEAX_SRT_TELEMETRY_RUNTIME_PROVIDER_ROBOTWEAX)
            endif()
            if(EXISTS "${ROBOTWEAX_SRT_TELEMETRY_SRT_INCLUDE_DIR}/srt.h" AND
               NOT EXISTS "${ROBOTWEAX_SRT_TELEMETRY_SRT_INCLUDE_DIR}/srt/srt.h")
                target_compile_definitions(robotweax_srt_telemetry_public_srt_fault_lab
                    PRIVATE ROBOTWEAX_SRT_TELEMETRY_RUNTIME_USE_FLAT_SRT_HEADER)
            endif()
            if(TARGET robotweax_srt_telemetry_http)
                add_executable(robotweax_srt_telemetry_public_srt_dashboard_demo
                    adapters/public_srt/tests/dashboard_demo.cpp)
                target_include_directories(robotweax_srt_telemetry_public_srt_dashboard_demo
                    PRIVATE ${ROBOTWEAX_SRT_TELEMETRY_SRT_INCLUDE_DIR})
                target_link_libraries(robotweax_srt_telemetry_public_srt_dashboard_demo
                    PRIVATE RobotweaxSRTTelemetry::public_srt_adapter
                            RobotweaxSRTTelemetry::http
                            ${ROBOTWEAX_SRT_TELEMETRY_SRT_LIBRARY}
                            Threads::Threads)
                target_compile_features(robotweax_srt_telemetry_public_srt_dashboard_demo PRIVATE cxx_std_20)
                if(ROBOTWEAX_SRT_TELEMETRY_RUNTIME_TEST_PROVIDER STREQUAL "robotweax-srt")
                    target_compile_definitions(robotweax_srt_telemetry_public_srt_dashboard_demo
                        PRIVATE ROBOTWEAX_SRT_TELEMETRY_RUNTIME_PROVIDER_ROBOTWEAX)
                endif()
                if(EXISTS "${ROBOTWEAX_SRT_TELEMETRY_SRT_INCLUDE_DIR}/srt.h" AND
                   NOT EXISTS "${ROBOTWEAX_SRT_TELEMETRY_SRT_INCLUDE_DIR}/srt/srt.h")
                    target_compile_definitions(robotweax_srt_telemetry_public_srt_dashboard_demo
                        PRIVATE ROBOTWEAX_SRT_TELEMETRY_RUNTIME_USE_FLAT_SRT_HEADER)
                endif()
                add_executable(robotweax_srt_telemetry_embed_tests adapters/public_srt/tests/test_embed.cpp)
                target_include_directories(robotweax_srt_telemetry_embed_tests
                    PRIVATE ${ROBOTWEAX_SRT_TELEMETRY_SRT_INCLUDE_DIR})
                target_link_libraries(robotweax_srt_telemetry_embed_tests
                    PRIVATE RobotweaxSRTTelemetry::embed ${ROBOTWEAX_SRT_TELEMETRY_SRT_LIBRARY} Threads::Threads)
                target_compile_features(robotweax_srt_telemetry_embed_tests PRIVATE cxx_std_20)
                if(EXISTS "${ROBOTWEAX_SRT_TELEMETRY_SRT_INCLUDE_DIR}/srt.h" AND
                   NOT EXISTS "${ROBOTWEAX_SRT_TELEMETRY_SRT_INCLUDE_DIR}/srt/srt.h")
                    target_compile_definitions(robotweax_srt_telemetry_embed_tests
                        PRIVATE ROBOTWEAX_SRT_TELEMETRY_USE_FLAT_SRT_HEADER)
                endif()
                add_test(NAME robotweax_srt_telemetry_embed_tests COMMAND robotweax_srt_telemetry_embed_tests)
                set_tests_properties(robotweax_srt_telemetry_embed_tests PROPERTIES TIMEOUT 120)
                add_executable(robotweax_srt_telemetry_embed_group_tests
                    adapters/public_srt/tests/test_embed_group.cpp)
                target_include_directories(robotweax_srt_telemetry_embed_group_tests
                    PRIVATE ${ROBOTWEAX_SRT_TELEMETRY_SRT_INCLUDE_DIR})
                target_link_libraries(robotweax_srt_telemetry_embed_group_tests
                    PRIVATE RobotweaxSRTTelemetry::embed ${ROBOTWEAX_SRT_TELEMETRY_SRT_LIBRARY} Threads::Threads)
                target_compile_features(robotweax_srt_telemetry_embed_group_tests PRIVATE cxx_std_20)
                if(EXISTS "${ROBOTWEAX_SRT_TELEMETRY_SRT_INCLUDE_DIR}/srt.h" AND
                   NOT EXISTS "${ROBOTWEAX_SRT_TELEMETRY_SRT_INCLUDE_DIR}/srt/srt.h")
                    target_compile_definitions(robotweax_srt_telemetry_embed_group_tests
                        PRIVATE ROBOTWEAX_SRT_TELEMETRY_USE_FLAT_SRT_HEADER)
                endif()
                if(ROBOTWEAX_SRT_TELEMETRY_REQUIRE_GROUP_TEST)
                    add_test(NAME robotweax_srt_telemetry_embed_group_tests
                        COMMAND robotweax_srt_telemetry_embed_group_tests --require)
                else()
                    add_test(NAME robotweax_srt_telemetry_embed_group_tests
                        COMMAND robotweax_srt_telemetry_embed_group_tests)
                    set_tests_properties(robotweax_srt_telemetry_embed_group_tests PROPERTIES SKIP_RETURN_CODE 77)
                endif()
                set_tests_properties(robotweax_srt_telemetry_embed_group_tests PROPERTIES TIMEOUT 120)
                # Browser smoke test of the built-in UI against the demo.
                find_package(Python3 COMPONENTS Interpreter)
                find_program(ROBOTWEAX_SRT_TELEMETRY_BROWSER
                    NAMES google-chrome google-chrome-stable chromium chromium-browser chrome)
                if(Python3_Interpreter_FOUND AND ROBOTWEAX_SRT_TELEMETRY_BROWSER)
                    add_test(NAME robotweax_srt_telemetry_ui_smoke_test
                        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/ui_smoke_test.py
                            --demo $<TARGET_FILE:robotweax_srt_telemetry_public_srt_dashboard_demo>
                            --browser ${ROBOTWEAX_SRT_TELEMETRY_BROWSER})
                    set_tests_properties(robotweax_srt_telemetry_ui_smoke_test
                        PROPERTIES TIMEOUT 300 LABELS ui)
                else()
                    message(STATUS "UI smoke test skipped: needs Python 3 and Chrome or Chromium")
                endif()
            endif()
            add_test(NAME robotweax_srt_telemetry_public_srt_fault_lab
                COMMAND robotweax_srt_telemetry_public_srt_fault_lab)
            set_tests_properties(robotweax_srt_telemetry_public_srt_fault_lab
                PROPERTIES TIMEOUT 600 LABELS lab)
            add_test(NAME robotweax_srt_telemetry_public_srt_runtime_tests
                COMMAND robotweax_srt_telemetry_public_srt_runtime_tests)
            # Live groups need an SRT library with connection groups (Haivision
            # SRT with ENABLE_BONDING=ON); the test is skipped otherwise unless
            # ROBOTWEAX_SRT_TELEMETRY_REQUIRE_GROUP_TEST makes that a failure.
            add_executable(robotweax_srt_telemetry_public_srt_group_live_tests
                adapters/public_srt/tests/test_public_srt_group_live.cpp)
            target_include_directories(robotweax_srt_telemetry_public_srt_group_live_tests
                PRIVATE ${ROBOTWEAX_SRT_TELEMETRY_SRT_INCLUDE_DIR})
            target_link_libraries(robotweax_srt_telemetry_public_srt_group_live_tests
                PRIVATE RobotweaxSRTTelemetry::public_srt_adapter
                        ${ROBOTWEAX_SRT_TELEMETRY_SRT_LIBRARY}
                        Threads::Threads)
            target_compile_features(robotweax_srt_telemetry_public_srt_group_live_tests
                PRIVATE cxx_std_20)
            if(EXISTS "${ROBOTWEAX_SRT_TELEMETRY_SRT_INCLUDE_DIR}/srt.h" AND
               NOT EXISTS "${ROBOTWEAX_SRT_TELEMETRY_SRT_INCLUDE_DIR}/srt/srt.h")
                target_compile_definitions(robotweax_srt_telemetry_public_srt_group_live_tests
                    PRIVATE ROBOTWEAX_SRT_TELEMETRY_USE_FLAT_SRT_HEADER)
            endif()
            if(ROBOTWEAX_SRT_TELEMETRY_REQUIRE_GROUP_TEST)
                add_test(NAME robotweax_srt_telemetry_public_srt_group_live_tests
                    COMMAND robotweax_srt_telemetry_public_srt_group_live_tests --require)
            else()
                add_test(NAME robotweax_srt_telemetry_public_srt_group_live_tests
                    COMMAND robotweax_srt_telemetry_public_srt_group_live_tests)
                set_tests_properties(robotweax_srt_telemetry_public_srt_group_live_tests
                    PROPERTIES SKIP_RETURN_CODE 77)
            endif()
            add_test(NAME robotweax_srt_telemetry_public_srt_lifecycle_tests
                COMMAND robotweax_srt_telemetry_public_srt_runtime_tests --lifecycle)
            add_test(NAME robotweax_srt_telemetry_public_srt_field_coverage_test
                COMMAND robotweax_srt_telemetry_public_srt_runtime_tests --field-coverage)
            add_test(NAME robotweax_srt_telemetry_public_srt_connection_events_test
                COMMAND robotweax_srt_telemetry_public_srt_runtime_tests --connection-events)
            add_test(NAME robotweax_srt_telemetry_public_srt_sampler_test
                COMMAND robotweax_srt_telemetry_public_srt_runtime_tests --sampler)
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
            set_tests_properties(robotweax_srt_telemetry_public_srt_sampler_test
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

    add_executable(robotweax_srt_telemetry_sampler_tests
        sdk/tests/test_sampler.cpp)
    target_link_libraries(robotweax_srt_telemetry_sampler_tests
        PRIVATE RobotweaxSRTTelemetry::core)
    target_compile_features(robotweax_srt_telemetry_sampler_tests PRIVATE cxx_std_20)
    add_test(NAME robotweax_srt_telemetry_sampler_tests
        COMMAND robotweax_srt_telemetry_sampler_tests)
    set_tests_properties(robotweax_srt_telemetry_sampler_tests PROPERTIES TIMEOUT 30)

    add_executable(robotweax_srt_telemetry_history_tests
        sdk/tests/test_history.cpp)
    target_link_libraries(robotweax_srt_telemetry_history_tests
        PRIVATE RobotweaxSRTTelemetry::core)
    target_compile_features(robotweax_srt_telemetry_history_tests PRIVATE cxx_std_20)
    add_test(NAME robotweax_srt_telemetry_history_tests
        COMMAND robotweax_srt_telemetry_history_tests)
    add_executable(robotweax_srt_telemetry_health_tests
        sdk/tests/test_health.cpp)
    target_link_libraries(robotweax_srt_telemetry_health_tests
        PRIVATE RobotweaxSRTTelemetry::core)
    target_compile_features(robotweax_srt_telemetry_health_tests PRIVATE cxx_std_20)
    add_test(NAME robotweax_srt_telemetry_health_tests
        COMMAND robotweax_srt_telemetry_health_tests)
    add_executable(robotweax_srt_telemetry_export_tests
        sdk/tests/test_export.cpp)
    target_link_libraries(robotweax_srt_telemetry_export_tests
        PRIVATE RobotweaxSRTTelemetry::core)
    target_compile_features(robotweax_srt_telemetry_export_tests PRIVATE cxx_std_20)
    target_compile_definitions(robotweax_srt_telemetry_export_tests PRIVATE
        ROBOTWEAX_SRT_TELEMETRY_NAMES_DOC="${CMAKE_CURRENT_SOURCE_DIR}/docs/architecture/metric-names.md")
    add_test(NAME robotweax_srt_telemetry_export_tests
        COMMAND robotweax_srt_telemetry_export_tests)
    # OTLP output against the OpenTelemetry protobuf schema, when available
    # (pip install opentelemetry-proto).
    find_package(Python3 COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        execute_process(
            COMMAND ${Python3_EXECUTABLE} -c "import opentelemetry.proto.collector.metrics.v1.metrics_service_pb2"
            RESULT_VARIABLE otlp_proto_missing OUTPUT_QUIET ERROR_QUIET)
        if(otlp_proto_missing EQUAL 0)
            add_test(NAME robotweax_srt_telemetry_otlp_proto_test
                COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/otlp_validate.py
                    --command $<TARGET_FILE:robotweax_srt_telemetry_export_tests> --dump-otlp)
        elseif(ROBOTWEAX_SRT_TELEMETRY_REQUIRE_OTLP_PROTO_TEST)
            message(FATAL_ERROR "OTLP schema test required: pip install opentelemetry-proto")
        else()
            message(STATUS "OTLP schema test skipped: pip install opentelemetry-proto")
        endif()
    elseif(ROBOTWEAX_SRT_TELEMETRY_REQUIRE_OTLP_PROTO_TEST)
        message(FATAL_ERROR "OTLP schema test required: no Python 3 interpreter")
    endif()

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

if(TARGET robotweax_srt_telemetry_embed)
    install(TARGETS robotweax_srt_telemetry_embed
        EXPORT RobotweaxSRTTelemetryTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

if(ROBOTWEAX_SRT_TELEMETRY_BUILD_HTTP_SERVER)
    install(TARGETS robotweax_srt_telemetry_http
        EXPORT RobotweaxSRTTelemetryTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    install(DIRECTORY server/include/
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
# Alert rules, Grafana dashboards and Collector example (docs/architecture/alerting.md).
install(DIRECTORY observability/
    DESTINATION ${CMAKE_INSTALL_DATADIR}/robotweax_srt_telemetry/observability
    PATTERN "build_dashboards.py" EXCLUDE
    PATTERN "__pycache__" EXCLUDE)
