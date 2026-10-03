#ifndef ROBOTWEAX_SRT_TELEMETRY_EMBED_H
#define ROBOTWEAX_SRT_TELEMETRY_EMBED_H

/* Embedding SRT Telemetry in an SRT application with a handful of lines
 * (library RobotweaxSRTTelemetry::embed). One object runs everything a host
 * would otherwise wire up itself: the sampler with health, history and event
 * journal, the HTTP server with the built-in dashboard, /metrics and the API,
 * an optional OTLP exporter, and per socket the statistics session and the
 * connection monitor (connection state, settings and events).
 *
 *   #include <robotweax/srt_telemetry_public_srt_host.h> // in a file using srt.h
 *
 *   robotweax_srt_telemetry_embed *telemetry = NULL;
 *   robotweax_srt_telemetry_host_embed_start("0.0.0.0:9810,token_file=/etc/srt/token", "my-app",
 *                                            &telemetry);
 *   ...
 *   robotweax_srt_telemetry_embed_attach(telemetry, socket, ROBOTWEAX_SRT_TELEMETRY_ENDPOINT_ROLE_SENDER,
 *                                        NULL, 0);                 // once the socket is set up
 *   robotweax_srt_telemetry_embed_detach(telemetry, socket);     // before srt_close(socket)
 *   ...
 *   robotweax_srt_telemetry_embed_stop(telemetry);
 *
 * attach, detach and stop accept a NULL embed and then do nothing, so a host
 * can call them whether or not telemetry was enabled.
 *
 * A detached socket stays on the dashboard as "connection lost" (Failing)
 * for linger_seconds, then disappears. A socket attached during the linger
 * with the same role and labels continues that source (one row, continuous
 * history): a reconnection, or the next attempt while the peer is down.
 * Hosts with several simultaneous connections of one role should label
 * them apart (e.g. by stream), or two peers can share a row.
 * See docs/architecture/embedding.md. */

#include <robotweax/srt_telemetry_public_srt.h>
#include <robotweax/srt_telemetry_public_srt_connection.h>

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#if defined(ROBOTWEAX_SRT_TELEMETRY_EMBED_SHARED)
#if defined(ROBOTWEAX_SRT_TELEMETRY_EMBED_BUILDING)
#define ROBOTWEAX_SRT_TELEMETRY_EMBED_API __declspec(dllexport)
#else
#define ROBOTWEAX_SRT_TELEMETRY_EMBED_API __declspec(dllimport)
#endif
#else
#define ROBOTWEAX_SRT_TELEMETRY_EMBED_API
#endif
#elif defined(__GNUC__) || defined(__clang__)
#define ROBOTWEAX_SRT_TELEMETRY_EMBED_API __attribute__((visibility("default")))
#else
#define ROBOTWEAX_SRT_TELEMETRY_EMBED_API
#endif

#ifdef __cplusplus
extern "C"
{
#endif

#define ROBOTWEAX_SRT_TELEMETRY_EMBED_TEXT_CAPACITY 256u
/* Host labels (embed config and attach together); the embed adds stream
 * (from name), provider and, when captured, stream_id, within the sampler's
 * 8 labels. */
#define ROBOTWEAX_SRT_TELEMETRY_EMBED_MAX_HOST_LABELS 5u

/* embed_config_v1.flags */
/* No HTTP server (OTLP only, or a host that serves the sampler itself). */
#define ROBOTWEAX_SRT_TELEMETRY_EMBED_NO_HTTP UINT32_C(1)
/* HTTP server without the built-in dashboard (/metrics and the API only). */
#define ROBOTWEAX_SRT_TELEMETRY_EMBED_NO_UI UINT32_C(2)
/* Read SRTO_STREAMID and label each source with it (stream_id). Stream IDs
 * can identify customers or channels, so this is off by default. */
#define ROBOTWEAX_SRT_TELEMETRY_EMBED_STREAM_ID_LABEL UINT32_C(4)

    typedef struct robotweax_srt_telemetry_embed_config_v1
    {
        uint32_t struct_size;
        uint32_t abi_version;
        /* The host's SRT library: robotweax_srt_telemetry_host_embed_functions
         * (srt_telemetry_public_srt_host.h) fills these from srt.h. */
        robotweax_srt_telemetry_provider provider;
        uint32_t flags;
        robotweax_srt_telemetry_public_srt_bistats_fn bistats;
        robotweax_srt_telemetry_public_srt_getsockstate_fn getsockstate;
        robotweax_srt_telemetry_public_srt_getrejectreason_fn getrejectreason;
        robotweax_srt_telemetry_public_srt_getsockflag_fn getsockflag;
        /* HTTP server: numeric address, default "127.0.0.1"; port default
         * 9810, 0 picks a free one (embed_http_port). */
        char bind_address[ROBOTWEAX_SRT_TELEMETRY_EMBED_TEXT_CAPACITY];
        uint16_t port;
        uint16_t reserved;
        /* Optional bearer token and allowed host names, as in
         * http_server_config_v1. Without a token, a server bound to every
         * interface answers only requests addressed by IP literal, localhost
         * or one of allowed_hosts. */
        char bearer_token[ROBOTWEAX_SRT_TELEMETRY_EMBED_TEXT_CAPACITY];
        char allowed_hosts[ROBOTWEAX_SRT_TELEMETRY_EMBED_TEXT_CAPACITY];
        /* OTLP/HTTP endpoint, e.g. "http://127.0.0.1:4318"; empty: no OTLP. */
        char otlp_endpoint[ROBOTWEAX_SRT_TELEMETRY_EMBED_TEXT_CAPACITY];
        /* OTLP export interval, default 10000 ms. */
        uint32_t otlp_interval_ms;
        /* History kept per source, default 3600 s (0: none). */
        uint32_t history_seconds;
        /* Event journal entries, default 4096 (0: none). */
        uint32_t journal_capacity;
        /* How long a detached socket stays as "connection lost", default
         * 60 s (0: removed at once). */
        uint32_t linger_seconds;
        /* OTLP service.name, e.g. "srt-live-transmit"; empty: "srt-telemetry". */
        char service_name[ROBOTWEAX_SRT_TELEMETRY_EMBED_TEXT_CAPACITY];
        /* When set, every source gets the label stream=<name> (unless the
         * host labels it otherwise): the name operators know the stream by,
         * e.g. "cam-2". Printable ASCII. */
        char name[ROBOTWEAX_SRT_TELEMETRY_EMBED_TEXT_CAPACITY];
        /* Labels for every source, e.g. {"site", "studio-2"}; copied at
         * start. */
        const robotweax_srt_telemetry_label_v1 *labels;
        size_t label_count;
    } robotweax_srt_telemetry_embed_config_v1;

    /* Defaults as above; the SRT functions are left NULL. */
    ROBOTWEAX_SRT_TELEMETRY_EMBED_API void
    robotweax_srt_telemetry_embed_config_default(robotweax_srt_telemetry_embed_config_v1 *config);

    /* Applies a command-line style spec to a configuration:
     *
     *   [address:]port[,option...]    e.g. "9810", "0.0.0.0:9810", "[::]:9810,token_file=/run/token"
     *   off                            no HTTP server (with otlp=...)
     *
     * Options: name=NAME (stream label), token=T, token_file=PATH (first
     * line, trimmed), host=NAME (allowed host name; repeatable), otlp=URL,
     * otlp_interval_ms=N, history_s=N, linger_s=N, stream_id (label sources
     * with their stream ID), no_ui.
     * A bare port binds 127.0.0.1, ":port" every interface; "off" needs
     * otlp=. INVALID_ARGUMENT names nothing; the spec
     * is short enough to check by eye. */
    ROBOTWEAX_SRT_TELEMETRY_EMBED_API robotweax_srt_telemetry_error
    robotweax_srt_telemetry_embed_config_parse(robotweax_srt_telemetry_embed_config_v1 *config,
                                               const char *spec);

    typedef struct robotweax_srt_telemetry_embed robotweax_srt_telemetry_embed;

    /* Starts the sampling thread, the HTTP server and the OTLP exporter.
     * IO_FAILURE when the HTTP port cannot be bound. */
    ROBOTWEAX_SRT_TELEMETRY_EMBED_API robotweax_srt_telemetry_error
    robotweax_srt_telemetry_embed_start(const robotweax_srt_telemetry_embed_config_v1 *config,
                                        robotweax_srt_telemetry_embed **embed);

    /* Monitors a socket from now on: call it once the socket is set up
     * (after srt_connect started or srt_accept returned); a connecting
     * socket shows as connecting (as lost when it continues a lost source,
     * until it connects). role: SENDER for a socket that sends
     * media, RECEIVER for one that receives it. labels (copied) describe it,
     * e.g. {"stream", "camera-2"}; the sampler's label rules apply, keys
     * unique also against the embed's labels. INVALID_STATE when the socket
     * is already attached. Thread-safe; NULL embed: OK, nothing done. */
    ROBOTWEAX_SRT_TELEMETRY_EMBED_API robotweax_srt_telemetry_error
    robotweax_srt_telemetry_embed_attach(robotweax_srt_telemetry_embed *embed, int32_t socket,
                                         robotweax_srt_telemetry_endpoint_role role,
                                         const robotweax_srt_telemetry_label_v1 *labels, size_t label_count);

    /* Stops reading the socket; call it before srt_close. After it returns
     * the embed makes no further SRT calls on the socket, which stays on
     * the dashboard as lost for linger_seconds. NOT_FOUND for a socket that
     * is not attached. Thread-safe; NULL embed: OK. */
    ROBOTWEAX_SRT_TELEMETRY_EMBED_API robotweax_srt_telemetry_error
    robotweax_srt_telemetry_embed_detach(robotweax_srt_telemetry_embed *embed, int32_t socket);

    /* The sampler, for hosts that want more (thresholds, callbacks,
     * rendering). Valid until stop. The embed ticks it: do not start, stop,
     * tick or destroy it, and do not call attach or detach from its
     * callbacks (they run on the embed's thread under its lock). */
    ROBOTWEAX_SRT_TELEMETRY_EMBED_API robotweax_srt_telemetry_sampler *
    robotweax_srt_telemetry_embed_sampler(robotweax_srt_telemetry_embed *embed);

    /* The bound HTTP port, 0 without a server. */
    ROBOTWEAX_SRT_TELEMETRY_EMBED_API uint16_t
    robotweax_srt_telemetry_embed_http_port(robotweax_srt_telemetry_embed *embed);

    /* Stops everything, forgets every socket (the host still closes them)
     * and frees the embed. NULL: nothing done. */
    ROBOTWEAX_SRT_TELEMETRY_EMBED_API void robotweax_srt_telemetry_embed_stop(robotweax_srt_telemetry_embed *embed);

#ifdef __cplusplus
}
#endif

#endif
