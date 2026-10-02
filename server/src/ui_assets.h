#ifndef ROBOTWEAX_SRT_TELEMETRY_HTTP_UI_ASSETS_H
#define ROBOTWEAX_SRT_TELEMETRY_HTTP_UI_ASSETS_H

#include <cstddef>

/* The built-in web UI, embedded at build time (cmake/EmbedUiAssets.cmake). */
namespace robotweax_srt_telemetry_http_ui
{

struct Asset
{
    const char *path; /* URL path, "/" for index.html */
    const char *type; /* Content-Type */
    const unsigned char *data;
    std::size_t size;
    const char *etag; /* quoted */
};

extern const Asset assets[];
extern const std::size_t asset_count;

} // namespace robotweax_srt_telemetry_http_ui

#endif
