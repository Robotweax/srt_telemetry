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
#include <robotweax/srt_telemetry_public_srt_group.h>

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

static inline int robotweax_srt_telemetry_host_group_data(int32_t group, struct SRT_SocketGroupData_ *members,
                                                         size_t *count)
{
    return srt_group_data((SRTSOCKET)group, members, count);
}

static inline int robotweax_srt_telemetry_host_getsockname(int32_t socket, struct sockaddr *name, int *length)
{
    return srt_getsockname((SRTSOCKET)socket, name, length);
}

/* Fills the provider and the injected functions of a group config. Haivision
 * SRT built without ENABLE_BONDING still links: srt_group_data then fails and
 * so does group_session_sample. */
static inline void robotweax_srt_telemetry_host_group_functions(
    robotweax_srt_telemetry_public_srt_group_config_v1 *config)
{
    config->provider = ROBOTWEAX_SRT_TELEMETRY_HOST_PROVIDER;
    config->bistats = robotweax_srt_telemetry_host_bistats;
    config->group_data = robotweax_srt_telemetry_host_group_data;
    config->getsockname = robotweax_srt_telemetry_host_getsockname;
    config->getsockstate = robotweax_srt_telemetry_host_getsockstate;
}

/* The version of the srt.h in use, for recording headers: Robotweax SRT's
 * own release version, else the Haivision SRT version. */
#if defined(ROBOTWEAX_SRT_VERSION_STRING)
#define ROBOTWEAX_SRT_TELEMETRY_HOST_PROVIDER_VERSION ROBOTWEAX_SRT_VERSION_STRING
#elif defined(SRT_VERSION_STRING)
#define ROBOTWEAX_SRT_TELEMETRY_HOST_PROVIDER_VERSION SRT_VERSION_STRING
#else
#define ROBOTWEAX_SRT_TELEMETRY_HOST_PROVIDER_VERSION ""
#endif

/* Fills the provider, its version and the SRT functions of an embed config. */
static inline void robotweax_srt_telemetry_host_embed_functions(robotweax_srt_telemetry_embed_config_v1 *config)
{
    if (strlen(ROBOTWEAX_SRT_TELEMETRY_HOST_PROVIDER_VERSION) < sizeof(config->provider_version))
    {
        memcpy(config->provider_version, ROBOTWEAX_SRT_TELEMETRY_HOST_PROVIDER_VERSION,
               strlen(ROBOTWEAX_SRT_TELEMETRY_HOST_PROVIDER_VERSION) + 1);
    }
    config->provider = ROBOTWEAX_SRT_TELEMETRY_HOST_PROVIDER;
    config->bistats = robotweax_srt_telemetry_host_bistats;
    config->getsockstate = robotweax_srt_telemetry_host_getsockstate;
    config->getrejectreason = robotweax_srt_telemetry_host_getrejectreason;
    config->getsockflag = robotweax_srt_telemetry_host_getsockflag;
    config->group_data = robotweax_srt_telemetry_host_group_data;
    config->getsockname = robotweax_srt_telemetry_host_getsockname;
}

/* ROBOTWEAX_SRT_PATHID_NEGOTIATED (a record flag) comes with the getter in
 * Robotweax SRT's srt.h: it marks a header that declares it. */
#if defined(ROBOTWEAX_SRT_PATHID_NEGOTIATED)
static inline int robotweax_srt_telemetry_host_group_path_data(int32_t group,
                                                               struct ROBOTWEAX_SRT_GroupPathDataV1_ *members,
                                                               size_t *count)
{
    return robotweax_srt_group_path_data_v1((SRTSOCKET)group, members, count);
}

/* Robotweax SRT with negotiated path labels (protocol PR #298): names group
 * paths from the caller's label on both sides (embed_config_v1.
 * group_path_data). Call it after robotweax_srt_telemetry_host_embed_functions;
 * the peers enable labels themselves (SRTO_ROBOTWEAX_PATHID_REQUIRED on the
 * group and the listener, SRTO_ROBOTWEAX_PATHID per endpoint). Not defined
 * for an srt.h without them. */
static inline void robotweax_srt_telemetry_host_embed_path_data(robotweax_srt_telemetry_embed_config_v1 *config)
{
    config->group_path_data = robotweax_srt_telemetry_host_group_path_data;
}
#endif

#if defined(ROBOTWEAX_SRT_GROUP_SEND_STATE_VERSION) && ROBOTWEAX_SRT_GROUP_SEND_STATE_VERSION == 1
/* Explicit opt-in after qualifying the selected provider build. */
static inline robotweax_srt_telemetry_error
robotweax_srt_telemetry_host_embed_group_send_state(robotweax_srt_telemetry_embed *embed)
{
    return robotweax_srt_telemetry_embed_set_group_send_state_v1(embed,
                                                                 robotweax_srt_group_send_state_v1);
}
#endif

#if defined(ROBOTWEAX_SRT_RTTDATA_VERSION) && ROBOTWEAX_SRT_RTTDATA_VERSION == 1
static inline int robotweax_srt_telemetry_host_rtt_data(int32_t socket,
                                                        struct ROBOTWEAX_SRT_RttDataV1_ *output,
                                                        size_t output_size)
{
    return robotweax_srt_rtt_data_v1((SRTSOCKET)socket, output, output_size);
}
/* Opt-in after build qualification; ordinary host helpers remain compatible
 * with old providers and never resolve/link this symbol by themselves. */
static inline robotweax_srt_telemetry_error
robotweax_srt_telemetry_host_embed_rtt_data(robotweax_srt_telemetry_embed *embed)
{
    return robotweax_srt_telemetry_embed_set_rtt_data_v1(embed,
                                                         robotweax_srt_telemetry_host_rtt_data);
}
#endif

#if defined(ROBOTWEAX_SRT_STANDBY_RTT_VERSION) && ROBOTWEAX_SRT_STANDBY_RTT_VERSION == 1
/* Explicit opt-in only; ordinary helpers never link the optional symbol. */
static inline robotweax_srt_telemetry_error
robotweax_srt_telemetry_host_embed_standby_rtt_data(robotweax_srt_telemetry_embed *embed)
{
    return robotweax_srt_telemetry_embed_set_standby_rtt_data_v1(embed,
                                                                 robotweax_srt_standby_rtt_data_v1);
}
#endif

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
