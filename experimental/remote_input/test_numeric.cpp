#include "numeric_input_draft.hpp"
#include <cmath>
#include <iostream>
#include <limits>
#include <source_location>

namespace d = robotweax::srt::telemetry::remote_draft;
namespace
{
unsigned checks;
void check(bool ok, std::source_location where = std::source_location::current())
{
    ++checks;
    if (!ok)
        throw std::runtime_error("numeric draft line " + std::to_string(where.line()));
}
template <class F> void rejects(const char *field, F fn)
{
    bool matched = false;
    try
    {
        fn();
    }
    catch (const d::ValidationError &e)
    {
        matched = e.field() == field;
    }
    check(matched);
}
d::Context context(const char *segment = "segment-1")
{
    d::Context c;
    c.segment_id = segment;
    c.producer_id = "producer";
    c.source_id = "source";
    c.observer_clock_domain = "observer-process-1";
    c.role = d::EndpointRole::sender;
    c.origin = {"numeric-fixture", "fixture/1", {0, 255}};
    return c;
}
d::Observation observation(std::uint64_t seq, std::uint64_t producer)
{
    return {seq, producer, {}, {}, {1, 2, 3}};
}
d::NumericQualification qualification()
{
    d::NumericQualification q;
    q.id = "synthetic-test-evidence";
    q.provider = ROBOTWEAX_SRT_TELEMETRY_PROVIDER_ROBOTWEAX_SRT;
    q.raw_fields = ROBOTWEAX_SRT_TELEMETRY_FIELD_SENT_WIRE_BYTE_TOTAL |
                   ROBOTWEAX_SRT_TELEMETRY_FIELD_RECEIVED_WIRE_BYTE_TOTAL |
                   ROBOTWEAX_SRT_TELEMETRY_FIELD_RTT_SECONDS;
    q.extended_fields =
        ROBOTWEAX_SRT_TELEMETRY_EXT_FIELD_RECEIVED_RETRANSMITTED_PACKET_SINCE_CLEAR |
        ROBOTWEAX_SRT_TELEMETRY_EXT_FIELD_BELATED_PACKET_SINCE_CLEAR;
    return q;
}
d::NumericValues values(std::uint64_t sent, std::uint64_t received = 100)
{
    d::NumericValues v;
    v.raw.available_fields = qualification().raw_fields;
    v.raw.sent_wire_byte_total = sent;
    v.raw.received_wire_byte_total = received;
    v.raw.rtt_seconds = 0.0125;
    v.extended.available_fields = qualification().extended_fields;
    v.extended.received_retransmitted_packet_since_clear = 20;
    v.extended.belated_packet_since_clear = 10;
    return v;
}
void qualification_guards()
{
    d::NumericProcessor empty(context());
    auto first = empty.push(observation(1, 1'000'000'000), {100, {}}, values(100));
    check(first.status == d::PushResult::accepted && first.sample);
    check(!first.sample->qualified_values.raw.available_fields &&
          !first.sample->qualified_values.extended.available_fields);
    check(first.sample->qualified_values.raw.sent_wire_byte_total == 0);
    check(empty.push(observation(2, 3'000'000'000), {101, {}}, values(300))
              .sample->delta.available_rates == 0);
    auto q = qualification();
    q.id.clear();
    rejects("qualification.id", [&] { d::NumericProcessor p(context(), q); });
    q = qualification();
    q.provider = ROBOTWEAX_SRT_TELEMETRY_PROVIDER_UNKNOWN;
    rejects("qualification.provider", [&] { d::NumericProcessor p(context(), q); });
    q = qualification();
    auto unknown = context();
    unknown.role = d::EndpointRole::unknown;
    rejects("qualification.role", [&] { d::NumericProcessor p(unknown, q); });
    q = qualification();
    q.raw_fields |= UINT64_C(1) << 63;
    rejects("qualification.raw_fields", [&] { d::NumericProcessor p(context(), q); });
    q = qualification();
    q.extended_fields |= UINT64_C(1) << 63;
    rejects("qualification.extended_fields", [&] { d::NumericProcessor p(context(), q); });
    q = qualification();
    q.raw_fields |= ROBOTWEAX_SRT_TELEMETRY_FIELD_SEND_BUFFER_BYTES;
    q.queue_sampling = d::GaugeSampling::averaged;
    rejects("qualification.queue_sampling", [&] { d::NumericProcessor p(context(), q); });
    q.queue_sampling = d::GaugeSampling::instantaneous;
    d::NumericProcessor instant(context(), q);
    auto v = values(100);
    v.raw.available_fields |= ROBOTWEAX_SRT_TELEMETRY_FIELD_SEND_BUFFER_BYTES;
    v.raw.send_buffer_bytes = 42;
    check(instant.push(observation(1, 1000), {1, {}}, v)
              .sample->qualified_values.raw.send_buffer_bytes == 42);
    q = qualification();
    q.extended_fields = ROBOTWEAX_SRT_TELEMETRY_EXT_FIELD_ESTIMATED_LINK_CAPACITY_BITS_PER_SECOND;
    rejects("qualification.extended_fields.provider", [&] { d::NumericProcessor p(context(), q); });
    q.provider = ROBOTWEAX_SRT_TELEMETRY_PROVIDER_HAIVISION_SRT;
    d::NumericProcessor haivision(context(), q);
    q.extended_fields = ROBOTWEAX_SRT_TELEMETRY_EXT_FIELD_DATA_ARRIVAL_RATE_BITS_PER_SECOND;
    rejects("qualification.extended_fields.provider", [&] { d::NumericProcessor p(context(), q); });
}
void sampling_and_input_edges()
{
    const std::uint64_t raw_queues[] = {ROBOTWEAX_SRT_TELEMETRY_FIELD_AVAILABLE_SEND_BUFFER_BYTES,
                                        ROBOTWEAX_SRT_TELEMETRY_FIELD_SEND_BUFFER_BYTES,
                                        ROBOTWEAX_SRT_TELEMETRY_FIELD_SEND_BUFFER_SECONDS,
                                        ROBOTWEAX_SRT_TELEMETRY_FIELD_RECEIVE_BUFFER_BYTES,
                                        ROBOTWEAX_SRT_TELEMETRY_FIELD_RECEIVE_BUFFER_SECONDS};
    const std::uint64_t ext_queues[] = {ROBOTWEAX_SRT_TELEMETRY_EXT_FIELD_SEND_BUFFER_PACKETS,
                                        ROBOTWEAX_SRT_TELEMETRY_EXT_FIELD_RECEIVE_BUFFER_PACKETS};
    for (auto field : raw_queues)
        for (auto mode : {d::GaugeSampling::unknown, d::GaugeSampling::averaged})
        {
            auto q = qualification();
            q.raw_fields = field;
            q.extended_fields = 0;
            q.queue_sampling = mode;
            rejects("qualification.queue_sampling", [&] { d::NumericProcessor p(context(), q); });
        }
    for (auto field : ext_queues)
        for (auto mode : {d::GaugeSampling::unknown, d::GaugeSampling::averaged})
        {
            auto q = qualification();
            q.raw_fields = 0;
            q.extended_fields = field;
            q.queue_sampling = mode;
            rejects("qualification.queue_sampling", [&] { d::NumericProcessor p(context(), q); });
        }
    d::NumericProcessor p(context(), qualification());
    auto v = values(0);
    v.raw.rtt_seconds = 0;
    auto zero = p.push(observation(1, 1000), {10, {}}, v);
    check((zero.sample->qualified_values.raw.available_fields &
           ROBOTWEAX_SRT_TELEMETRY_FIELD_SENT_WIRE_BYTE_TOTAL) &&
          zero.sample->qualified_values.raw.sent_wire_byte_total == 0);
    check((zero.sample->qualified_values.raw.available_fields &
           ROBOTWEAX_SRT_TELEMETRY_FIELD_RTT_SECONDS) &&
          zero.sample->qualified_values.raw.rtt_seconds == 0);
    auto missing = values(0);
    missing.raw.available_fields &= ~ROBOTWEAX_SRT_TELEMETRY_FIELD_SENT_WIRE_BYTE_TOTAL;
    auto gap = p.push(observation(2, 2000), {11, {}}, missing);
    check(!(gap.sample->qualified_values.raw.available_fields &
            ROBOTWEAX_SRT_TELEMETRY_FIELD_SENT_WIRE_BYTE_TOTAL));
    auto returning = p.push(observation(3, 3000), {12, {}}, values(100));
    check(!(returning.sample->delta.available_rates &
            ROBOTWEAX_SRT_TELEMETRY_RATE_SENT_WIRE_BITS_PER_SECOND));
    auto invalid = values(200);
    invalid.raw.available_fields |= UINT64_C(1) << 63;
    rejects("values.raw.available_fields",
            [&] { p.push(observation(4, 4000), {13, {}}, invalid); });
    invalid = values(200);
    invalid.extended.available_fields |= UINT64_C(1) << 63;
    rejects("values.extended.available_fields",
            [&] { p.push(observation(4, 4000), {13, {}}, invalid); });
    invalid = values(200);
    invalid.raw.rtt_seconds = -1;
    rejects("values.raw.rtt_seconds", [&] { p.push(observation(4, 4000), {13, {}}, invalid); });
    check(p.latest(12).record->observation.sequence == 3);
}

void producer_time_and_transaction()
{
    d::NumericProcessor p(context(), qualification());
    auto one = observation(10, 1'000'000'000);
    auto v = values(100);
    auto first = p.push(one, {100, {}}, v);
    check(first.sample->delta.quality_flags & ROBOTWEAX_SRT_TELEMETRY_QUALITY_FIRST_SAMPLE);
    check(first.sample->delta.available_rates == 0);
    check(p.push(one, {200, {}}, v).status == d::PushResult::duplicate);
    auto changed = v;
    changed.raw.sent_wire_byte_total = 150;
    check(p.push(one, {201, {}}, changed).status == d::PushResult::conflict);
    auto invalid = values(999);
    invalid.raw.rtt_seconds = std::numeric_limits<double>::quiet_NaN();
    rejects("values.raw.rtt_seconds",
            [&] { p.push(observation(11, 2'000'000'000), {202, {}}, invalid); });
    auto malformed = observation(11, 2'000'000'000);
    malformed.origin_payload.clear();
    rejects("origin_payload", [&] { p.push(malformed, {203, {}}, values(999)); });
    auto two = p.push(observation(15, 3'000'000'000), {204, {}}, values(300, 200));
    check(two.status == d::PushResult::accepted && two.sample->delta.sent_wire_byte_delta == 200);
    check(two.sample->delta.interval_nano == 2'000'000'000 &&
          two.sample->delta.sent_wire_bits_per_second == 800);
    check(two.sample->delta.received_wire_bits_per_second == 400);
    check(p.latest(204).record->first_receipt.monotonic_nano == 204);
    check(p.push(observation(12, 2'000'000'000), {205, {}}, values(900)).status ==
          d::PushResult::stale_sequence);
    check(p.push(observation(16, 2'000'000'000), {206, {}}, values(900)).status ==
          d::PushResult::producer_clock_regression);
    auto reset = p.push(observation(17, 4'000'000'000), {207, {}}, values(5, 400));
    check(reset.sample->delta.counter_reset_fields ==
          ROBOTWEAX_SRT_TELEMETRY_FIELD_SENT_WIRE_BYTE_TOTAL);
    check(!(reset.sample->delta.available_rates &
            ROBOTWEAX_SRT_TELEMETRY_RATE_SENT_WIRE_BITS_PER_SECOND));
    check(reset.sample->delta.received_wire_bits_per_second == 1600);
    auto equal = p.push(observation(18, 4'000'000'000), {208, {}}, values(10, 450));
    check(!equal.sample->delta.available_rates &&
          equal.sample->delta.quality_flags &
              ROBOTWEAX_SRT_TELEMETRY_QUALITY_MONOTONIC_CLOCK_REGRESSION);
    auto after = p.push(observation(19, 5'000'000'000), {209, {}}, values(20, 500));
    check(after.sample->delta.sent_wire_byte_delta == 10 &&
          after.sample->delta.sent_wire_bits_per_second == 80);
    check(p.export_history_jsonl(208).find("\"receiptAgeNano\":\"0\"") !=
          std::string::npos); // merged reader interleaving semantics
    check(p.export_history_jsonl(210).find("\"producerWallTime\":null") != std::string::npos);
    p.retire();
    check(p.latest(211).status == d::Status::retired);
    check(p.push(observation(20, 6'000'000'000), {212, {}}, values(30)).status ==
          d::PushResult::retired);
    d::NumericProcessor next(context("new-segment"), qualification());
    check(next.push(observation(1, 1000), {300, {}}, values(100)).sample->delta.quality_flags &
          ROBOTWEAX_SRT_TELEMETRY_QUALITY_FIRST_SAMPLE);
}
void clear_boundaries()
{
    constexpr auto field =
        ROBOTWEAX_SRT_TELEMETRY_EXT_FIELD_RECEIVED_RETRANSMITTED_PACKET_SINCE_CLEAR;
    auto v = values(100);
    d::NumericProcessor unknown(context(), qualification());
    unknown.push(observation(1, 1000), {1, {}}, v);
    v.extended.received_retransmitted_packet_since_clear = 25;
    auto unknown_delta = unknown.push(observation(2, 2000), {2, {}}, v);
    check(unknown_delta.sample->qualified_values.extended.available_fields & field);
    check(!(unknown_delta.sample->extended_delta.available_delta_fields & field) &&
          !unknown_delta.sample->since_clear_delta_eligible);
    auto q = qualification();
    q.since_clear = d::SinceClearPolicy::exclusive_no_clear;
    d::NumericProcessor exclusive(context(), q);
    exclusive.push(observation(1, 1000), {1, {}}, values(100));
    auto known = exclusive.push(observation(2, 2000), {2, {}}, v);
    check(known.sample->since_clear_delta_eligible &&
          known.sample->extended_delta.received_retransmitted_packet_delta == 5);
    v.extended.received_retransmitted_packet_since_clear = 1;
    auto decrease = exclusive.push(observation(3, 3000), {3, {}}, v);
    check(decrease.sample->extended_delta.counter_reset_fields & field);
    check(!(decrease.sample->delta.quality_flags & ROBOTWEAX_SRT_TELEMETRY_QUALITY_COUNTER_RESET));
    q.since_clear = d::SinceClearPolicy::explicit_boundary;
    d::NumericProcessor boundary(context(), q);
    v = values(100);
    v.clear_boundary = 0;
    boundary.push(observation(1, 1000), {1, {}}, v);
    v = values(200);
    v.clear_boundary = 0;
    v.extended.received_retransmitted_packet_since_clear = 30;
    auto stable = boundary.push(observation(2, 2000), {2, {}}, v);
    check(stable.sample->since_clear_delta_eligible &&
          stable.sample->extended_delta.received_retransmitted_packet_delta == 10);
    v = values(300);
    v.clear_boundary = 1;
    v.extended.received_retransmitted_packet_since_clear = 100;
    auto crossed = boundary.push(observation(3, 3000), {3, {}}, v);
    check(crossed.sample->clear_boundary_changed && !crossed.sample->since_clear_delta_eligible);
    check(!(crossed.sample->extended_delta.available_delta_fields & field));
    check(crossed.sample->delta.sent_wire_byte_delta == 100);
    v.clear_boundary.reset();
    auto missing = boundary.push(observation(4, 4000), {4, {}}, v);
    check(!missing.sample->since_clear_delta_eligible);
    v.clear_boundary = 1;
    auto first_again = boundary.push(observation(5, 5000), {5, {}}, v);
    check(!first_again.sample->since_clear_delta_eligible);
}
} // namespace
int main()
{
    try
    {
        qualification_guards();
        sampling_and_input_edges();
        producer_time_and_transaction();
        clear_boundaries();
        std::cout << "PASS " << checks
                  << " numeric qualification/producer-time/reset/order/clear-policy checks; no "
                     "runtime activation\n";
    }
    catch (const std::exception &e)
    {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
