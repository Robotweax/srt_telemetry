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

#include <robotweax/srt_telemetry_embed.h>

#include <string.h>
#include <robotweax/srt_telemetry_public_srt_connection.h>

/* The provider of the srt.h in use: Robotweax SRT's compatibility headers
 * define these guards; anything else is taken as Haivision SRT. */
#if defined(ROBOTWEAX_SRT_COMPAT_SRT_H) || defined(ROBOTWEAX_SRT_FLAT_COMPAT_HEADER_H)
#define ROBOTWEAX_SRT_TELEMETRY_HOST_PROVIDER ROBOTWEAX_SRT_TELEMETRY_PROVIDER_ROBOTWEAX_SRT
#else
#define ROBOTWEAX_SRT_TELEMETRY_HOST_PROVIDER ROBOTWEAX_SRT_TELEMETRY_PROVIDER_HAIVISION_SRT
#endif

static inline int robotweax_srt_telemetry_host_bistats(int32_t socket, struct CBytePerfMon *statistics, int clear,
                                                       int instantaneous)
{
    return srt_bistats((SRTSOCKET)socket, (SRT_TRACEBSTATS *)statistics, clear, instantaneous);
}

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

/* Fills the provider and the SRT functions of an embed config. */
static inline void robotweax_srt_telemetry_host_embed_functions(robotweax_srt_telemetry_embed_config_v1 *config)
{
    config->provider = ROBOTWEAX_SRT_TELEMETRY_HOST_PROVIDER;
    config->bistats = robotweax_srt_telemetry_host_bistats;
    config->getsockstate = robotweax_srt_telemetry_host_getsockstate;
    config->getrejectreason = robotweax_srt_telemetry_host_getrejectreason;
    config->getsockflag = robotweax_srt_telemetry_host_getsockflag;
}

/* Starts an embed from a command-line style spec (see
 * robotweax_srt_telemetry_embed_config_parse), e.g. "0.0.0.0:9810" or
 * "9810,token_file=/run/srt/token". service_name names the application in
 * OTLP (may be NULL). */
static inline robotweax_srt_telemetry_error robotweax_srt_telemetry_host_embed_start(
    const char *spec, const char *service_name, robotweax_srt_telemetry_embed **embed)
{
    robotweax_srt_telemetry_embed_config_v1 config;
    robotweax_srt_telemetry_error error;
    if (embed == NULL)
    {
        return ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT;
    }
    *embed = NULL;
    robotweax_srt_telemetry_embed_config_default(&config);
    robotweax_srt_telemetry_host_embed_functions(&config);
    if (service_name != NULL && strlen(service_name) < sizeof(config.service_name))
    {
        memcpy(config.service_name, service_name, strlen(service_name) + 1);
    }
    error = robotweax_srt_telemetry_embed_config_parse(&config, spec);
    return error != ROBOTWEAX_SRT_TELEMETRY_OK ? error : robotweax_srt_telemetry_embed_start(&config, embed);
}

#endif
