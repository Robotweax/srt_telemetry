#include <robotweax/srt_telemetry.h>

#include <cstring>
#include <new>

struct robotweax_srt_telemetry_delta_engine
{
    bool has_previous = false;
    robotweax_srt_telemetry_raw_sample_v1 previous{};
};

namespace
{

bool valid_sample(const robotweax_srt_telemetry_raw_sample_v1 *sample)
{
    return sample != nullptr &&
           sample->struct_size >= sizeof(robotweax_srt_telemetry_raw_sample_v1) &&
           sample->abi_version == ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
}

bool valid_delta(const robotweax_srt_telemetry_delta_v1 *delta)
{
    return delta != nullptr && delta->struct_size >= sizeof(robotweax_srt_telemetry_delta_v1) &&
           delta->abi_version == ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
}

void initialize_delta(robotweax_srt_telemetry_delta_v1 &delta)
{
    std::memset(&delta, 0, sizeof(delta));
    delta.struct_size = sizeof(delta);
    delta.abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
}

void calculate_counter(const uint64_t field, const uint64_t current_available,
                       const uint64_t previous_available, const uint64_t current,
                       const uint64_t previous, uint64_t &destination,
                       robotweax_srt_telemetry_delta_v1 &delta)
{
    if ((current_available & field) == 0 || (previous_available & field) == 0)
    {
        return;
    }
    if (current < previous)
    {
        delta.counter_reset_fields |= field;
        delta.quality_flags |= ROBOTWEAX_SRT_TELEMETRY_QUALITY_COUNTER_RESET;
        return;
    }
    destination = current - previous;
    delta.available_delta_fields |= field;
}

void calculate_rate(const uint64_t required_delta_field, const uint64_t rate_field,
                    const uint64_t byte_delta, const uint64_t interval_nano,
                    robotweax_srt_telemetry_delta_v1 &delta, double &destination)
{
    if ((delta.available_delta_fields & required_delta_field) == 0 || interval_nano == 0)
    {
        return;
    }
    destination = static_cast<double>(byte_delta) * 8.0 * 1'000'000'000.0 /
                  static_cast<double>(interval_nano);
    delta.available_rates |= rate_field;
}

} // namespace

extern "C" robotweax_srt_telemetry_error
robotweax_srt_telemetry_delta_engine_create(robotweax_srt_telemetry_delta_engine **engine)
{
    if (engine == nullptr)
    {
        return ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT;
    }
    *engine = new (std::nothrow) robotweax_srt_telemetry_delta_engine{};
    return *engine == nullptr ? ROBOTWEAX_SRT_TELEMETRY_ALLOCATION_FAILURE
                              : ROBOTWEAX_SRT_TELEMETRY_OK;
}

extern "C" void
robotweax_srt_telemetry_delta_engine_destroy(robotweax_srt_telemetry_delta_engine *engine)
{
    delete engine;
}

extern "C" void
robotweax_srt_telemetry_delta_engine_reset(robotweax_srt_telemetry_delta_engine *engine)
{
    if (engine != nullptr)
    {
        *engine = {};
    }
}

extern "C" robotweax_srt_telemetry_error
robotweax_srt_telemetry_delta_engine_update(robotweax_srt_telemetry_delta_engine *engine,
                                            const robotweax_srt_telemetry_raw_sample_v1 *sample,
                                            robotweax_srt_telemetry_delta_v1 *delta)
{
    if (engine == nullptr || sample == nullptr || delta == nullptr)
    {
        return ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT;
    }
    if (!valid_sample(sample) || !valid_delta(delta))
    {
        return ROBOTWEAX_SRT_TELEMETRY_ABI_MISMATCH;
    }

    initialize_delta(*delta);
    if (!engine->has_previous)
    {
        delta->quality_flags |= ROBOTWEAX_SRT_TELEMETRY_QUALITY_FIRST_SAMPLE;
        engine->previous = *sample;
        engine->has_previous = true;
        return ROBOTWEAX_SRT_TELEMETRY_OK;
    }

    const auto &previous = engine->previous;
    if (sample->provider != previous.provider)
    {
        delta->quality_flags |= ROBOTWEAX_SRT_TELEMETRY_QUALITY_PROVIDER_CHANGED;
        engine->previous = *sample;
        return ROBOTWEAX_SRT_TELEMETRY_OK;
    }
    if (sample->sample_monotonic_nano <= previous.sample_monotonic_nano)
    {
        delta->quality_flags |= ROBOTWEAX_SRT_TELEMETRY_QUALITY_MONOTONIC_CLOCK_REGRESSION;
        engine->previous = *sample;
        return ROBOTWEAX_SRT_TELEMETRY_OK;
    }

    delta->interval_nano = sample->sample_monotonic_nano - previous.sample_monotonic_nano;

#define ROBOTWEAX_CALCULATE_COUNTER(field_name, member_name, delta_name)                           \
    calculate_counter(field_name, sample->available_fields, previous.available_fields,             \
                      sample->member_name, previous.member_name, delta->delta_name, *delta)

    ROBOTWEAX_CALCULATE_COUNTER(ROBOTWEAX_SRT_TELEMETRY_FIELD_SENT_PACKET_TOTAL, sent_packet_total,
                                sent_packet_delta);
    ROBOTWEAX_CALCULATE_COUNTER(ROBOTWEAX_SRT_TELEMETRY_FIELD_RECEIVED_PACKET_TOTAL,
                                received_packet_total, received_packet_delta);
    ROBOTWEAX_CALCULATE_COUNTER(ROBOTWEAX_SRT_TELEMETRY_FIELD_UNIQUE_SENT_PACKET_TOTAL,
                                unique_sent_packet_total, unique_sent_packet_delta);
    ROBOTWEAX_CALCULATE_COUNTER(ROBOTWEAX_SRT_TELEMETRY_FIELD_UNIQUE_RECEIVED_PACKET_TOTAL,
                                unique_received_packet_total, unique_received_packet_delta);
    ROBOTWEAX_CALCULATE_COUNTER(ROBOTWEAX_SRT_TELEMETRY_FIELD_SENDER_LOSS_PACKET_TOTAL,
                                sender_loss_packet_total, sender_loss_packet_delta);
    ROBOTWEAX_CALCULATE_COUNTER(ROBOTWEAX_SRT_TELEMETRY_FIELD_RECEIVER_LOSS_PACKET_TOTAL,
                                receiver_loss_packet_total, receiver_loss_packet_delta);
    ROBOTWEAX_CALCULATE_COUNTER(ROBOTWEAX_SRT_TELEMETRY_FIELD_RETRANSMITTED_PACKET_TOTAL,
                                retransmitted_packet_total, retransmitted_packet_delta);
    ROBOTWEAX_CALCULATE_COUNTER(ROBOTWEAX_SRT_TELEMETRY_FIELD_SENDER_DROP_PACKET_TOTAL,
                                sender_drop_packet_total, sender_drop_packet_delta);
    ROBOTWEAX_CALCULATE_COUNTER(ROBOTWEAX_SRT_TELEMETRY_FIELD_RECEIVER_DROP_PACKET_TOTAL,
                                receiver_drop_packet_total, receiver_drop_packet_delta);
    ROBOTWEAX_CALCULATE_COUNTER(ROBOTWEAX_SRT_TELEMETRY_FIELD_RECEIVER_UNDECRYPTABLE_PACKET_TOTAL,
                                receiver_undecryptable_packet_total,
                                receiver_undecryptable_packet_delta);
    ROBOTWEAX_CALCULATE_COUNTER(ROBOTWEAX_SRT_TELEMETRY_FIELD_SENT_WIRE_BYTE_TOTAL,
                                sent_wire_byte_total, sent_wire_byte_delta);
    ROBOTWEAX_CALCULATE_COUNTER(ROBOTWEAX_SRT_TELEMETRY_FIELD_RECEIVED_WIRE_BYTE_TOTAL,
                                received_wire_byte_total, received_wire_byte_delta);
    ROBOTWEAX_CALCULATE_COUNTER(ROBOTWEAX_SRT_TELEMETRY_FIELD_UNIQUE_SENT_WIRE_BYTE_TOTAL,
                                unique_sent_wire_byte_total, unique_sent_wire_byte_delta);
    ROBOTWEAX_CALCULATE_COUNTER(ROBOTWEAX_SRT_TELEMETRY_FIELD_UNIQUE_RECEIVED_WIRE_BYTE_TOTAL,
                                unique_received_wire_byte_total, unique_received_wire_byte_delta);
    ROBOTWEAX_CALCULATE_COUNTER(ROBOTWEAX_SRT_TELEMETRY_FIELD_RETRANSMITTED_WIRE_BYTE_TOTAL,
                                retransmitted_wire_byte_total, retransmitted_wire_byte_delta);

#undef ROBOTWEAX_CALCULATE_COUNTER

    calculate_rate(ROBOTWEAX_SRT_TELEMETRY_FIELD_SENT_WIRE_BYTE_TOTAL,
                   ROBOTWEAX_SRT_TELEMETRY_RATE_SENT_WIRE_BITS_PER_SECOND,
                   delta->sent_wire_byte_delta, delta->interval_nano, *delta,
                   delta->sent_wire_bits_per_second);
    calculate_rate(ROBOTWEAX_SRT_TELEMETRY_FIELD_RECEIVED_WIRE_BYTE_TOTAL,
                   ROBOTWEAX_SRT_TELEMETRY_RATE_RECEIVED_WIRE_BITS_PER_SECOND,
                   delta->received_wire_byte_delta, delta->interval_nano, *delta,
                   delta->received_wire_bits_per_second);
    calculate_rate(ROBOTWEAX_SRT_TELEMETRY_FIELD_UNIQUE_SENT_WIRE_BYTE_TOTAL,
                   ROBOTWEAX_SRT_TELEMETRY_RATE_UNIQUE_SENT_WIRE_BITS_PER_SECOND,
                   delta->unique_sent_wire_byte_delta, delta->interval_nano, *delta,
                   delta->unique_sent_wire_bits_per_second);
    calculate_rate(ROBOTWEAX_SRT_TELEMETRY_FIELD_UNIQUE_RECEIVED_WIRE_BYTE_TOTAL,
                   ROBOTWEAX_SRT_TELEMETRY_RATE_UNIQUE_RECEIVED_WIRE_BITS_PER_SECOND,
                   delta->unique_received_wire_byte_delta, delta->interval_nano, *delta,
                   delta->unique_received_wire_bits_per_second);

    engine->previous = *sample;
    return ROBOTWEAX_SRT_TELEMETRY_OK;
}
