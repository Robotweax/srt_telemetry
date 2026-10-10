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
#include <robotweax/srt_telemetry_public_srt_group.h>

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
#define ROBOTWEAX_SRT_TELEMETRY_EMBED_SHORT_TEXT_CAPACITY 64u
/* Host labels (embed config and attach together); the embed adds stream
 * (from name), provider and, when captured, stream_id, and for connection
 * groups group and path, within the sampler's 10 labels. */
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
        /* Recordings (docs/architecture/embedding.md#recordings): when set,
         * an existing, writable directory. Every source keeps its 100 ms
         * samples in a ring of recording_seconds and is written there as one
         * versioned JSONL recording (the bounded recorder's format) by a
         * writer thread when its socket is detached, when it is removed and
         * at stop, which waits for the writes. Empty: no recordings. */
        char recording_directory[ROBOTWEAX_SRT_TELEMETRY_EMBED_TEXT_CAPACITY];
        /* Ring length per source, default 600 s (0 also means 600), at most
         * 36000 s; 472 bytes per sample, allocated at attach. The oldest
         * samples are overwritten and counted in the recording header. */
        uint32_t recording_seconds;
        uint32_t reserved2;
        /* Recording header metadata, printable ASCII: the SRT library version
         * (robotweax_srt_telemetry_host_embed_functions fills it from srt.h)
         * and its source revision, e.g. a commit. Empty: "unknown". */
        char provider_version[ROBOTWEAX_SRT_TELEMETRY_EMBED_SHORT_TEXT_CAPACITY];
        char provider_revision[ROBOTWEAX_SRT_TELEMETRY_EMBED_SHORT_TEXT_CAPACITY];
        /* Needed only for connection groups (embed_attach_group);
         * robotweax_srt_telemetry_host_embed_functions fills them. */
        robotweax_srt_telemetry_public_srt_group_data_fn group_data;
        robotweax_srt_telemetry_public_srt_getsockname_fn getsockname;
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
     * with their stream ID), no_ui, jsonl=DIR (recordings), jsonl_s=N
     * (recording ring), provider_revision=REV.
     * A bare port binds 127.0.0.1, ":port" every interface; "off" needs
     * otlp= or jsonl=. INVALID_ARGUMENT names nothing; the spec
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

    /* A connection group (Broadcast or Main/Backup) to monitor; see
     * docs/architecture/embedding.md#connection-groups. */
    typedef struct robotweax_srt_telemetry_embed_group_v1
    {
        uint32_t struct_size;
        uint32_t abi_version;
        /* The group handle (srt_create_group, or srt_accept with
         * SRTO_GROUPCONNECT). */
        int32_t group;
        robotweax_srt_telemetry_endpoint_role role;
        /* GROUP_TYPE_BROADCAST or GROUP_TYPE_BACKUP. */
        uint32_t group_type;
        /* Must be zero. */
        uint32_t reserved;
        /* As for embed_attach; copied. */
        const robotweax_srt_telemetry_label_v1 *labels;
        size_t label_count;
        /* Optional path names, as for the group session: called on the
         * embed's thread under its lock, so it must not call the embed. */
        robotweax_srt_telemetry_public_srt_path_name_fn path_name;
        void *path_name_user_data;
    } robotweax_srt_telemetry_embed_group_v1;

    /* Zeroes group and sets the sizes. */
    ROBOTWEAX_SRT_TELEMETRY_EMBED_API void
    robotweax_srt_telemetry_embed_group_default(robotweax_srt_telemetry_embed_group_v1 *group);

    /* Monitors a connection group and its members from now on: one source
     * for the group (labels + group=<type>) and one per member path (also
     * path=<name>), sampled together in every tick. A member's verdict is
     * capped at Watch; a member that reconnects on the same path continues
     * that path's row. Group events: member_added, member_removed and
     * active_member_changed on the group's source, member_state_changed on
     * the member's. Detach the group with embed_detach (its members go with
     * it) before closing it. INVALID_STATE when the handle is attached or
     * the embed has no group_data/getsockname. Thread-safe; NULL embed: OK. */
    ROBOTWEAX_SRT_TELEMETRY_EMBED_API robotweax_srt_telemetry_error
    robotweax_srt_telemetry_embed_attach_group(robotweax_srt_telemetry_embed *embed,
                                               const robotweax_srt_telemetry_embed_group_v1 *group);

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

    /* Recording file writes done and failed so far (either may be NULL); a
     * source is written up to three times (detach, removal, stop). A failed
     * write (directory gone, disk full) leaves the previous version; the
     * source's next write, if any, tries again. */
    ROBOTWEAX_SRT_TELEMETRY_EMBED_API void
    robotweax_srt_telemetry_embed_recordings(robotweax_srt_telemetry_embed *embed, uint64_t *written,
                                             uint64_t *failed);

    /* The bound HTTP port, 0 without a server. */
    ROBOTWEAX_SRT_TELEMETRY_EMBED_API uint16_t
    robotweax_srt_telemetry_embed_http_port(robotweax_srt_telemetry_embed *embed);

    /* Stops everything, writes every recording's final version and waits
     * for it, forgets every socket (the host still closes them) and frees
     * the embed. Must not overlap attach or detach on another thread.
     * NULL: nothing done. */
    ROBOTWEAX_SRT_TELEMETRY_EMBED_API void robotweax_srt_telemetry_embed_stop(robotweax_srt_telemetry_embed *embed);

#ifdef __cplusplus
}
#endif

#endif
