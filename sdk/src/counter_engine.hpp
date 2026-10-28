#pragma once
// Shared private numeric kernel. No Unix clock, origin codec or I/O enters it.
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <robotweax/srt_telemetry.h>

namespace robotweax_srt_telemetry_internal::counters
{
struct Point
{
    int provider{};
    std::uint64_t monotonic{}, available{};
    std::array<std::uint64_t, 15> values{};
};
struct ExtPoint
{
    std::uint64_t available{};
    std::array<std::uint64_t, 15> values{};
};
struct State
{
    bool has_previous{}, has_previous_ext{};
    Point previous;
    ExtPoint previous_ext;
};
template <class T> Point point(const T &raw, int provider, std::uint64_t monotonic)
{
    Point out{provider, monotonic, raw.available_fields, {}};
    std::size_t i = 0;
#define RWX_RAW_COUNTER(bit, member, delta) out.values[i++] = raw.member;
#include "counter_fields.inc"
#undef RWX_RAW_COUNTER
    return out;
}
template <class T> ExtPoint ext_point(const T &ext)
{
    ExtPoint out{ext.available_fields, {}};
    std::size_t i = 0;
#define RWX_EXT_COUNTER(bit, member, delta) out.values[i++] = ext.member;
#include "counter_fields.inc"
#undef RWX_EXT_COUNTER
    return out;
}
inline void rate(std::uint64_t field, std::uint64_t rate_field, std::uint64_t bytes,
                 robotweax_srt_telemetry_delta_v1 &out, double &value)
{
    if (!(out.available_delta_fields & field) || !out.interval_nano)
        return;
    value =
        static_cast<double>(bytes) * 8.0 * 1'000'000'000.0 / static_cast<double>(out.interval_nano);
    out.available_rates |= rate_field;
}
inline bool raw_update(State &state, const Point &current, robotweax_srt_telemetry_delta_v1 &out)
{
    std::memset(&out, 0, sizeof(out));
    out.struct_size = sizeof(out);
    out.abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
    bool interval = false;
    if (!state.has_previous)
        out.quality_flags |= ROBOTWEAX_SRT_TELEMETRY_QUALITY_FIRST_SAMPLE;
    else if (current.provider != state.previous.provider)
        out.quality_flags |= ROBOTWEAX_SRT_TELEMETRY_QUALITY_PROVIDER_CHANGED;
    else if (current.monotonic <= state.previous.monotonic)
        out.quality_flags |= ROBOTWEAX_SRT_TELEMETRY_QUALITY_MONOTONIC_CLOCK_REGRESSION;
    else
    {
        interval = true;
        out.interval_nano = current.monotonic - state.previous.monotonic;
        constexpr std::array<std::uint64_t, 15> bits{
#define RWX_RAW_COUNTER(bit, member, delta) bit,
#include "counter_fields.inc"
#undef RWX_RAW_COUNTER
        };
        const std::array<std::uint64_t *, 15> destinations{
#define RWX_RAW_COUNTER(bit, member, delta) &out.delta,
#include "counter_fields.inc"
#undef RWX_RAW_COUNTER
        };
        for (std::size_t i = 0; i < bits.size(); ++i)
        {
            if (!(current.available & state.previous.available & bits[i]))
                continue;
            if (current.values[i] < state.previous.values[i])
            {
                out.counter_reset_fields |= bits[i];
                out.quality_flags |= ROBOTWEAX_SRT_TELEMETRY_QUALITY_COUNTER_RESET;
            }
            else
            {
                *destinations[i] = current.values[i] - state.previous.values[i];
                out.available_delta_fields |= bits[i];
            }
        }
        rate(ROBOTWEAX_SRT_TELEMETRY_FIELD_SENT_WIRE_BYTE_TOTAL,
             ROBOTWEAX_SRT_TELEMETRY_RATE_SENT_WIRE_BITS_PER_SECOND, out.sent_wire_byte_delta, out,
             out.sent_wire_bits_per_second);
        rate(ROBOTWEAX_SRT_TELEMETRY_FIELD_RECEIVED_WIRE_BYTE_TOTAL,
             ROBOTWEAX_SRT_TELEMETRY_RATE_RECEIVED_WIRE_BITS_PER_SECOND,
             out.received_wire_byte_delta, out, out.received_wire_bits_per_second);
        rate(ROBOTWEAX_SRT_TELEMETRY_FIELD_UNIQUE_SENT_WIRE_BYTE_TOTAL,
             ROBOTWEAX_SRT_TELEMETRY_RATE_UNIQUE_SENT_WIRE_BITS_PER_SECOND,
             out.unique_sent_wire_byte_delta, out, out.unique_sent_wire_bits_per_second);
        rate(ROBOTWEAX_SRT_TELEMETRY_FIELD_UNIQUE_RECEIVED_WIRE_BYTE_TOTAL,
             ROBOTWEAX_SRT_TELEMETRY_RATE_UNIQUE_RECEIVED_WIRE_BITS_PER_SECOND,
             out.unique_received_wire_byte_delta, out, out.unique_received_wire_bits_per_second);
    }
    state.previous = current;
    state.has_previous = true;
    return interval;
}
inline void update(State &state, const Point &current, const ExtPoint *ext,
                   robotweax_srt_telemetry_delta_v1 &out,
                   robotweax_srt_telemetry_ext_delta_v1 *extended = nullptr,
                   std::uint64_t allowed_ext_deltas = ROBOTWEAX_SRT_TELEMETRY_EXT_COUNTER_FIELDS)
{
    const auto had_ext = state.has_previous_ext;
    const auto prior_ext = state.previous_ext;
    const bool interval = raw_update(state, current, out);
    if (!ext)
    {
        state.has_previous_ext = false;
        return;
    }
    // Callers supply the output whenever an extended input is present.
    std::memset(extended, 0, sizeof(*extended));
    extended->struct_size = sizeof(*extended);
    extended->abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
    if (interval && had_ext)
    {
        constexpr std::array<std::uint64_t, 15> bits{
#define RWX_EXT_COUNTER(bit, member, delta) bit,
#include "counter_fields.inc"
#undef RWX_EXT_COUNTER
        };
        const std::array<std::uint64_t *, 15> destinations{
#define RWX_EXT_COUNTER(bit, member, delta) &extended->delta,
#include "counter_fields.inc"
#undef RWX_EXT_COUNTER
        };
        for (std::size_t i = 0; i < bits.size(); ++i)
        {
            if (!(ext->available & prior_ext.available & allowed_ext_deltas & bits[i]))
                continue;
            if (ext->values[i] < prior_ext.values[i])
                extended->counter_reset_fields |= bits[i];
            else
            {
                *destinations[i] = ext->values[i] - prior_ext.values[i];
                extended->available_delta_fields |= bits[i];
            }
        }
    }
    state.previous_ext = *ext;
    state.has_previous_ext = true;
}
} // namespace robotweax_srt_telemetry_internal::counters
