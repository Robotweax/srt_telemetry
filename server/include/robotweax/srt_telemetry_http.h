#ifndef ROBOTWEAX_SRT_TELEMETRY_HTTP_H
#define ROBOTWEAX_SRT_TELEMETRY_HTTP_H

/* Optional embedded HTTP server: serves a sampler's state to Prometheus, to
 * the built-in UI and to any HTTP or WebSocket client. One thread, no
 * dependencies beyond POSIX sockets, read-only (GET only).
 *
 *   GET /metrics                         Prometheus text exposition
 *   GET /api/v1/snapshot[?window=S]      JSON snapshot (srt-telemetry.snapshot.v1)
 *   GET /api/v1/sources/{id}/history[?since_ms=T]
 *                                        compact per-window history (JSON)
 *   GET /api/v1/events[?after=N]         event journal after sequence N (JSON)
 *   GET /api/v1/live                     WebSocket feed: snapshot every
 *                                        interval, new events as they arrive
 *   GET /healthz                         "ok"
 *
 * Security defaults: it binds to 127.0.0.1, serves nothing writable, bounds
 * request size and time, connections, output backlog and WebSocket input,
 * and can require a bearer token. Without a token it answers only requests
 * addressed to localhost, an IP literal or an allowed host name (DNS
 * rebinding). Exposing it beyond the host is the host's decision
 * (bind_address). See docs/architecture/http-server.md. */

#include <robotweax/srt_telemetry_sampler.h>

#include <stdint.h>

#if defined(_WIN32)
#if defined(ROBOTWEAX_SRT_TELEMETRY_HTTP_SHARED)
#if defined(ROBOTWEAX_SRT_TELEMETRY_HTTP_BUILDING)
#define ROBOTWEAX_SRT_TELEMETRY_HTTP_API __declspec(dllexport)
#else
#define ROBOTWEAX_SRT_TELEMETRY_HTTP_API __declspec(dllimport)
#endif
#else
#define ROBOTWEAX_SRT_TELEMETRY_HTTP_API
#endif
#elif defined(__GNUC__) || defined(__clang__)
#define ROBOTWEAX_SRT_TELEMETRY_HTTP_API __attribute__((visibility("default")))
#else
#define ROBOTWEAX_SRT_TELEMETRY_HTTP_API
#endif

#ifdef __cplusplus
extern "C"
{
#endif

#define ROBOTWEAX_SRT_TELEMETRY_HTTP_ADDRESS_CAPACITY 64u
#define ROBOTWEAX_SRT_TELEMETRY_HTTP_TOKEN_CAPACITY 128u
#define ROBOTWEAX_SRT_TELEMETRY_HTTP_HOSTS_CAPACITY 256u

/* http_server_config_v1.flags */
/* Do not serve the WebSocket feed. */
#define ROBOTWEAX_SRT_TELEMETRY_HTTP_NO_LIVE_FEED UINT32_C(1)

    typedef struct robotweax_srt_telemetry_http_server_config_v1
    {
        uint32_t struct_size;
        uint32_t abi_version;
        /* Served sampler; must outlive the server. */
        robotweax_srt_telemetry_sampler *sampler;
        /* Numeric IPv4 or IPv6 address; default "127.0.0.1". "0.0.0.0" or
         * "::" exposes the server on every interface. */
        char bind_address[ROBOTWEAX_SRT_TELEMETRY_HTTP_ADDRESS_CAPACITY];
        /* 0 picks a free port (see http_server_port). */
        uint16_t port;
        uint16_t reserved;
        uint32_t flags;
        /* Simultaneous connections, including WebSocket clients; default 32.
         * WebSocket clients may use all but 4, which stay free for scrapes. */
        uint32_t max_connections;
        /* Live feed snapshot interval, 100 ms to 60 s; default 1000 ms. */
        uint32_t live_interval_ms;
        /* Time a client has to send its request and to read the response;
         * default 10000 ms. */
        uint32_t request_timeout_ms;
        uint32_t reserved2;
        /* When not empty, every request needs "Authorization: Bearer <token>"
         * (the WebSocket feed also accepts "?token=<token>", for browsers).
         * Printable ASCII without spaces. */
        char bearer_token[ROBOTWEAX_SRT_TELEMETRY_HTTP_TOKEN_CAPACITY];
        /* Without a token: comma-separated host names, besides localhost and
         * IP literals, that requests may address (Host header), e.g.
         * "encoder-7.studio.lan". Empty: localhost and IP literals only. */
        char allowed_hosts[ROBOTWEAX_SRT_TELEMETRY_HTTP_HOSTS_CAPACITY];
    } robotweax_srt_telemetry_http_server_config_v1;

    ROBOTWEAX_SRT_TELEMETRY_HTTP_API void
    robotweax_srt_telemetry_http_server_config_default(robotweax_srt_telemetry_sampler *sampler,
                                                       robotweax_srt_telemetry_http_server_config_v1 *config);

    typedef struct robotweax_srt_telemetry_http_server robotweax_srt_telemetry_http_server;

    /* Binds, listens and starts the server thread. INVALID_ARGUMENT for a bad
     * configuration, IO_FAILURE when the socket cannot be bound (port in
     * use, address not local). */
    ROBOTWEAX_SRT_TELEMETRY_HTTP_API robotweax_srt_telemetry_error
    robotweax_srt_telemetry_http_server_start(const robotweax_srt_telemetry_http_server_config_v1 *config,
                                              robotweax_srt_telemetry_http_server **server);

    /* The bound port (useful with port 0). */
    ROBOTWEAX_SRT_TELEMETRY_HTTP_API uint16_t
    robotweax_srt_telemetry_http_server_port(const robotweax_srt_telemetry_http_server *server);

    /* Closes every connection and joins the thread. Must not be called from a
     * sampler callback. Destroy the server before the sampler. */
    ROBOTWEAX_SRT_TELEMETRY_HTTP_API void
    robotweax_srt_telemetry_http_server_stop(robotweax_srt_telemetry_http_server *server);

#ifdef __cplusplus
}
#endif

#endif
