cmake_minimum_required(VERSION 3.21)

project(robotweax_srt_telemetry_package_consumer LANGUAGES C)

find_package(RobotweaxSRTTelemetry 0.1 CONFIG REQUIRED)

add_executable(robotweax_srt_telemetry_package_consumer main.c)
target_link_libraries(robotweax_srt_telemetry_package_consumer
    PRIVATE RobotweaxSRTTelemetry::core)

if(TARGET RobotweaxSRTTelemetry::public_srt_adapter)
    add_executable(robotweax_srt_telemetry_adapter_package_consumer adapter_main.c)
    target_link_libraries(robotweax_srt_telemetry_adapter_package_consumer
        PRIVATE RobotweaxSRTTelemetry::public_srt_adapter)
endif()
