// Synthetic compatibility oracle: only the existing public C API is called.
#include <bit>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <robotweax/srt_telemetry.h>

namespace
{
std::uint64_t seed = 0xa73d8b15ce29f041ULL;
std::uint64_t random_value()
{
    seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
    return seed;
}
void word(std::uint64_t v)
{
    std::cout << std::hex << std::setw(16) << std::setfill('0') << v;
}
} // namespace
int main()
{
    robotweax_srt_telemetry_delta_engine *engine = nullptr;
    if (robotweax_srt_telemetry_delta_engine_create(&engine) != ROBOTWEAX_SRT_TELEMETRY_OK)
        return 1;
    std::uint64_t mono = 1000;
    for (unsigned iteration = 0; iteration < 2048; ++iteration)
    {
        if (iteration % 97 == 0)
            robotweax_srt_telemetry_delta_engine_reset(engine);
        robotweax_srt_telemetry_raw_sample_v1 raw{};
        raw.struct_size = sizeof(raw);
        raw.abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
        robotweax_srt_telemetry_ext_sample_v1 ext{};
        ext.struct_size = sizeof(ext);
        ext.abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
        raw.provider = static_cast<robotweax_srt_telemetry_provider>((iteration / 19) % 4);
        raw.available_fields = random_value() & ((UINT64_C(1) << 27) - 1);
        if (iteration % 23 == 0)
            raw.available_fields |= UINT64_C(1) << 63;
        ext.available_fields = random_value() & ((UINT64_C(1) << 26) - 1);
        if (iteration % 29 == 0)
            ext.available_fields |= UINT64_C(1) << 57;
        if (iteration % 11 == 0)
            mono = 0;
        else if (iteration % 7 != 0)
            mono += random_value() % 10000;
        raw.sample_monotonic_nano = mono;
        raw.observed_at_unix_nano = random_value();
        raw.rtt_seconds = std::numeric_limits<double>::quiet_NaN(); // unused legacy gauge semantics
#define RWX_RAW_COUNTER(bit, member, delta)                                                        \
    raw.member = iteration % 31 == 0 ? UINT64_MAX : random_value();
#define RWX_EXT_COUNTER(bit, member, delta)                                                        \
    ext.member = iteration % 37 == 0 ? UINT64_MAX : random_value();
#include "../../sdk/src/counter_fields.inc"
#undef RWX_RAW_COUNTER
#undef RWX_EXT_COUNTER
        robotweax_srt_telemetry_delta_v1 delta{};
        delta.struct_size = sizeof(delta);
        delta.abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
        robotweax_srt_telemetry_ext_delta_v1 extended{};
        extended.struct_size = sizeof(extended);
        extended.abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
        const auto status = iteration % 13 == 0
                                ? robotweax_srt_telemetry_delta_engine_update(engine, &raw, &delta)
                                : robotweax_srt_telemetry_delta_engine_update_ext(
                                      engine, &raw, &ext, &delta, &extended);
        if (status != ROBOTWEAX_SRT_TELEMETRY_OK)
        {
            robotweax_srt_telemetry_delta_engine_destroy(engine);
            return 1;
        }
        word(delta.struct_size);
        word(delta.abi_version);
        word(delta.quality_flags);
        word(delta.interval_nano);
        word(delta.available_delta_fields);
        word(delta.counter_reset_fields);
        word(delta.available_rates);
#define RWX_RAW_COUNTER(bit, member, value) word(delta.value);
#include "../../sdk/src/counter_fields.inc"
#undef RWX_RAW_COUNTER
        word(std::bit_cast<std::uint64_t>(delta.sent_wire_bits_per_second));
        word(std::bit_cast<std::uint64_t>(delta.received_wire_bits_per_second));
        word(std::bit_cast<std::uint64_t>(delta.unique_sent_wire_bits_per_second));
        word(std::bit_cast<std::uint64_t>(delta.unique_received_wire_bits_per_second));
        word(extended.struct_size);
        word(extended.abi_version);
        word(extended.available_delta_fields);
        word(extended.counter_reset_fields);
#define RWX_EXT_COUNTER(bit, member, value) word(extended.value);
#include "../../sdk/src/counter_fields.inc"
#undef RWX_EXT_COUNTER
        std::cout << '\n';
    }
    robotweax_srt_telemetry_delta_engine_destroy(engine);
}
