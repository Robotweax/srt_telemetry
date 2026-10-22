#ifndef ROBOTWEAX_SRT_TELEMETRY_PEER_METADATA_INTERNAL_H
#define ROBOTWEAX_SRT_TELEMETRY_PEER_METADATA_INTERNAL_H

#include <robotweax/srt_telemetry_sampler.h>

#include <cstddef>

namespace robotweax_srt_telemetry_internal
{
inline bool peer_alnum(const char c)
{
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

/* A NUL-terminated text of 1..max bytes in SRT's identity character set
 * (name: space allowed, not last), zero after the terminator; or empty and
 * all zero when absent. */
inline bool peer_text(const char *text, const std::size_t capacity, const bool present, const bool name)
{
    std::size_t length = 0;
    while (length < capacity && text[length] != '\0')
        ++length;
    if (length == capacity)
        return false; /* no terminator */
    for (std::size_t i = length; i < capacity; ++i)
        if (text[i] != '\0')
            return false;
    if (!present)
        return length == 0;
    if (length == 0 || length > capacity - 1 || !peer_alnum(text[0]))
        return false;
    for (std::size_t i = 1; i < length; ++i)
    {
        const char c = text[i];
        if (!(peer_alnum(c) || c == '.' || c == '_' || c == '+' || c == '-' || (name && c == ' ')))
            return false;
    }
    return text[length - 1] != ' ';
}

inline bool valid_peer_metadata(const robotweax_srt_telemetry_peer_metadata_v1 &m)
{
    if (m.struct_size < sizeof(m) || m.abi_version != ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION ||
        m.identity_availability > 2 || (m.handshake_availability != 0 && m.handshake_availability != 2) ||
        m.padding[0] || m.padding[1] || m.reserved[0] || m.reserved[1])
        return false;
    const bool name = (m.identity_fields & ROBOTWEAX_SRT_TELEMETRY_PEER_IDENTITY_NAME) != 0;
    const bool release = (m.identity_fields & ROBOTWEAX_SRT_TELEMETRY_PEER_IDENTITY_RELEASE) != 0;
    if (m.identity_availability != 2)
    {
        if (m.identity_fields != 0 || (m.identity_availability == 0 && m.identity_native_result != 0))
            return false;
    }
    else if (m.identity_native_result != 0 || (m.identity_fields & ~3u) != 0 || (release && !name))
    {
        return false;
    }
    if (!peer_text(m.identity_name, sizeof(m.identity_name), name, true) ||
        !peer_text(m.identity_release, sizeof(m.identity_release), release, false))
        return false;
    if (m.handshake_availability == 0)
        return m.handshake_srt_version == 0 && m.handshake_flags == 0 &&
               m.handshake_receiver_delay_milliseconds == 0 && m.handshake_sender_delay_milliseconds == 0;
    return true;
}
} // namespace robotweax_srt_telemetry_internal
#endif
