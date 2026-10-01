#ifndef ROBOTWEAX_SRT_TELEMETRY_PUBLIC_SRT_HOST_H
#define ROBOTWEAX_SRT_TELEMETRY_PUBLIC_SRT_HOST_H

/* Optional helper for hosts. Include it in a translation unit that already
 * uses the host's own SRT library: the inline wrappers adapt srt.h signatures
 * to the plain-integer function types of the connection monitor. The SDK
 * itself never links an SRT library. Define
 * ROBOTWEAX_SRT_TELEMETRY_USE_FLAT_SRT_HEADER when srt.h is not under srt/. */

#if defined(ROBOTWEAX_SRT_TELEMETRY_USE_FLAT_SRT_HEADER)
#include <srt.h>
#else
#include <srt/srt.h>
#endif

#include <robotweax/srt_telemetry_public_srt_connection.h>

static inline int robotweax_srt_telemetry_host_getsockstate(int32_t socket)
{
    return (int)srt_getsockstate((SRTSOCKET)socket);
}

static inline int robotweax_srt_telemetry_host_getrejectreason(int32_t socket)
{
    return srt_getrejectreason((SRTSOCKET)socket);
}

static inline int robotweax_srt_telemetry_host_getsockflag(int32_t socket, int option, void *value,
                                                          int *length)
{
    return srt_getsockflag((SRTSOCKET)socket, (SRT_SOCKOPT)option, value, length);
}

/* Fills the injected functions of a connection config. */
static inline void robotweax_srt_telemetry_host_connection_functions(
    robotweax_srt_telemetry_public_srt_connection_config_v1 *config)
{
    config->getsockstate = robotweax_srt_telemetry_host_getsockstate;
    config->getrejectreason = robotweax_srt_telemetry_host_getrejectreason;
    config->getsockflag = robotweax_srt_telemetry_host_getsockflag;
}

#endif
