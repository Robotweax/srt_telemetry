#include <robotweax/srt_telemetry_history.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <new>
#include <vector>

static_assert(sizeof(robotweax_srt_telemetry_compact_record_v1) == 112,
              "compact record layout is part of the ABI");

namespace
{

std::uint32_t saturate(const double value)
{
    if (!(value > 0.0))
    {
        return 0;
    }
    constexpr auto limit = static_cast<double>(std::numeric_limits<std::uint32_t>::max());
    return value >= limit ? std::numeric_limits<std::uint32_t>::max()
                          : static_cast<std::uint32_t>(std::llround(value));
}

} // namespace

extern "C" robotweax_srt_telemetry_error robotweax_srt_telemetry_compact_record_from_window(
    const robotweax_srt_telemetry_window_summary_v1 *window,
    robotweax_srt_telemetry_compact_record_v1 *record)
{
    if (window == nullptr || record == nullptr)
    {
        return ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT;
    }
    if (window->struct_size < sizeof(*window) ||
        window->abi_version != ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION)
    {
        return ROBOTWEAX_SRT_TELEMETRY_ABI_MISMATCH;
    }
    std::memset(record, 0, sizeof(*record));
    record->start_unix_nano = window->start_unix_nano;
    record->window_milliseconds = static_cast<std::uint32_t>(
        std::min<std::uint64_t>(window->window_nano / UINT64_C(1'000'000), UINT32_MAX));
    record->quality_flags = static_cast<std::uint16_t>(window->quality_flags & 0xFFFFU);
    record->sample_count =
        static_cast<std::uint16_t>(std::min<std::uint32_t>(window->sample_count, UINT16_MAX));

    const auto has = [&](const robotweax_srt_telemetry_metric metric)
    { return (window->available_metrics & (UINT64_C(1) << metric)) != 0; };
    const auto &m = window->metrics;
    const auto put = [&](const robotweax_srt_telemetry_metric metric, const std::uint32_t bit,
                         float &destination, const double value)
    {
        if (has(metric))
        {
            destination = static_cast<float>(value);
            record->available |= bit;
        }
    };
    const auto count = [&](const robotweax_srt_telemetry_metric metric, const std::uint32_t bit,
                           std::uint32_t &destination)
    {
        if (has(metric))
        {
            destination = saturate(m[metric].total);
            record->available |= bit;
        }
    };

    put(ROBOTWEAX_SRT_TELEMETRY_METRIC_UNIQUE_SENT_BITS_PER_SECOND,
        ROBOTWEAX_SRT_TELEMETRY_COMPACT_UNIQUE_SENT_BPS, record->unique_sent_bits_per_second,
        m[ROBOTWEAX_SRT_TELEMETRY_METRIC_UNIQUE_SENT_BITS_PER_SECOND].rate);
    put(ROBOTWEAX_SRT_TELEMETRY_METRIC_RETRANSMITTED_BITS_PER_SECOND,
        ROBOTWEAX_SRT_TELEMETRY_COMPACT_RETRANSMITTED_BPS, record->retransmitted_bits_per_second,
        m[ROBOTWEAX_SRT_TELEMETRY_METRIC_RETRANSMITTED_BITS_PER_SECOND].rate);
    put(ROBOTWEAX_SRT_TELEMETRY_METRIC_UNIQUE_RECEIVED_BITS_PER_SECOND,
        ROBOTWEAX_SRT_TELEMETRY_COMPACT_UNIQUE_RECEIVED_BPS,
        record->unique_received_bits_per_second,
        m[ROBOTWEAX_SRT_TELEMETRY_METRIC_UNIQUE_RECEIVED_BITS_PER_SECOND].rate);
    put(ROBOTWEAX_SRT_TELEMETRY_METRIC_LINK_CAPACITY_BITS_PER_SECOND,
        ROBOTWEAX_SRT_TELEMETRY_COMPACT_LINK_CAPACITY_BPS, record->link_capacity_bits_per_second,
        m[ROBOTWEAX_SRT_TELEMETRY_METRIC_LINK_CAPACITY_BITS_PER_SECOND].mean);
    if (has(ROBOTWEAX_SRT_TELEMETRY_METRIC_RTT_SECONDS))
    {
        record->rtt_mean_seconds =
            static_cast<float>(m[ROBOTWEAX_SRT_TELEMETRY_METRIC_RTT_SECONDS].mean);
        record->rtt_max_seconds = static_cast<float>(m[ROBOTWEAX_SRT_TELEMETRY_METRIC_RTT_SECONDS].max);
        record->available |= ROBOTWEAX_SRT_TELEMETRY_COMPACT_RTT;
    }
    if (has(ROBOTWEAX_SRT_TELEMETRY_METRIC_RECOVERY_HEADROOM_RATIO))
    {
        const auto &headroom = m[ROBOTWEAX_SRT_TELEMETRY_METRIC_RECOVERY_HEADROOM_RATIO];
        record->recovery_headroom_min = static_cast<float>(headroom.min);
        record->recovery_headroom_mean = static_cast<float>(headroom.mean);
        record->available |= ROBOTWEAX_SRT_TELEMETRY_COMPACT_RECOVERY_HEADROOM;
    }
    put(ROBOTWEAX_SRT_TELEMETRY_METRIC_SEND_BUFFER_SECONDS, ROBOTWEAX_SRT_TELEMETRY_COMPACT_SEND_BUFFER,
        record->send_buffer_max_seconds, m[ROBOTWEAX_SRT_TELEMETRY_METRIC_SEND_BUFFER_SECONDS].max);
    put(ROBOTWEAX_SRT_TELEMETRY_METRIC_RECEIVE_BUFFER_SECONDS,
        ROBOTWEAX_SRT_TELEMETRY_COMPACT_RECEIVE_BUFFER, record->receive_buffer_min_seconds,
        m[ROBOTWEAX_SRT_TELEMETRY_METRIC_RECEIVE_BUFFER_SECONDS].min);
    put(ROBOTWEAX_SRT_TELEMETRY_METRIC_SEND_BUFFER_TO_DROP_THRESHOLD_RATIO,
        ROBOTWEAX_SRT_TELEMETRY_COMPACT_SEND_BUFFER_RATIO, record->send_buffer_ratio_max,
        m[ROBOTWEAX_SRT_TELEMETRY_METRIC_SEND_BUFFER_TO_DROP_THRESHOLD_RATIO].max);
    put(ROBOTWEAX_SRT_TELEMETRY_METRIC_FLIGHT_TO_FLOW_WINDOW_RATIO,
        ROBOTWEAX_SRT_TELEMETRY_COMPACT_FLIGHT_TO_FLOW, record->flight_to_flow_max,
        m[ROBOTWEAX_SRT_TELEMETRY_METRIC_FLIGHT_TO_FLOW_WINDOW_RATIO].max);

    count(ROBOTWEAX_SRT_TELEMETRY_METRIC_SENDER_LOSS_PACKETS_PER_SECOND,
          ROBOTWEAX_SRT_TELEMETRY_COMPACT_SENDER_LOSS, record->sender_loss_packets);
    count(ROBOTWEAX_SRT_TELEMETRY_METRIC_RECEIVER_LOSS_PACKETS_PER_SECOND,
          ROBOTWEAX_SRT_TELEMETRY_COMPACT_RECEIVER_LOSS, record->receiver_loss_packets);
    count(ROBOTWEAX_SRT_TELEMETRY_METRIC_RETRANSMITTED_PACKETS_PER_SECOND,
          ROBOTWEAX_SRT_TELEMETRY_COMPACT_RETRANSMITTED, record->retransmitted_packets);
    count(ROBOTWEAX_SRT_TELEMETRY_METRIC_RECEIVED_RETRANSMITTED_PACKETS_PER_SECOND,
          ROBOTWEAX_SRT_TELEMETRY_COMPACT_RECEIVED_RETRANSMITTED,
          record->received_retransmitted_packets);
    count(ROBOTWEAX_SRT_TELEMETRY_METRIC_SENDER_DROP_PACKETS_PER_SECOND,
          ROBOTWEAX_SRT_TELEMETRY_COMPACT_SENDER_DROP, record->sender_drop_packets);
    count(ROBOTWEAX_SRT_TELEMETRY_METRIC_RECEIVER_DROP_PACKETS_PER_SECOND,
          ROBOTWEAX_SRT_TELEMETRY_COMPACT_RECEIVER_DROP, record->receiver_drop_packets);
    count(ROBOTWEAX_SRT_TELEMETRY_METRIC_UNDECRYPTABLE_PACKETS_PER_SECOND,
          ROBOTWEAX_SRT_TELEMETRY_COMPACT_UNDECRYPTABLE, record->undecryptable_packets);
    count(ROBOTWEAX_SRT_TELEMETRY_METRIC_BELATED_PACKETS_PER_SECOND,
          ROBOTWEAX_SRT_TELEMETRY_COMPACT_BELATED, record->belated_packets);
    count(ROBOTWEAX_SRT_TELEMETRY_METRIC_FILTER_RECOVERED_PACKETS_PER_SECOND,
          ROBOTWEAX_SRT_TELEMETRY_COMPACT_FILTER_RECOVERED, record->filter_recovered_packets);
    count(ROBOTWEAX_SRT_TELEMETRY_METRIC_FILTER_UNRECOVERED_PACKETS_PER_SECOND,
          ROBOTWEAX_SRT_TELEMETRY_COMPACT_FILTER_UNRECOVERED, record->filter_unrecovered_packets);
    return ROBOTWEAX_SRT_TELEMETRY_OK;
}

/* ------------------------------------------------------------------ journal */

struct robotweax_srt_telemetry_event_journal
{
    mutable std::mutex mutex;
    std::vector<robotweax_srt_telemetry_event_v1> ring;
    std::size_t head = 0; /* index of the oldest event */
    std::size_t size = 0;
    std::uint64_t next_sequence = 1;
    std::uint64_t overwritten = 0;
};

extern "C" robotweax_srt_telemetry_error
robotweax_srt_telemetry_event_journal_create(const std::size_t capacity,
                                             robotweax_srt_telemetry_event_journal **journal)
{
    if (journal == nullptr)
    {
        return ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT;
    }
    *journal = nullptr;
    if (capacity == 0 || capacity > (std::size_t{1} << 24))
    {
        return ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT;
    }
    auto *created = new (std::nothrow) robotweax_srt_telemetry_event_journal{};
    if (created == nullptr)
    {
        return ROBOTWEAX_SRT_TELEMETRY_ALLOCATION_FAILURE;
    }
    try
    {
        created->ring.resize(capacity);
    }
    catch (const std::bad_alloc &)
    {
        delete created;
        return ROBOTWEAX_SRT_TELEMETRY_ALLOCATION_FAILURE;
    }
    *journal = created;
    return ROBOTWEAX_SRT_TELEMETRY_OK;
}

extern "C" void
robotweax_srt_telemetry_event_journal_destroy(robotweax_srt_telemetry_event_journal *journal)
{
    delete journal;
}

extern "C" robotweax_srt_telemetry_error
robotweax_srt_telemetry_event_journal_push(robotweax_srt_telemetry_event_journal *journal,
                                           const robotweax_srt_telemetry_event_v1 *event,
                                           std::uint64_t *sequence)
{
    if (journal == nullptr || event == nullptr)
    {
        return ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT;
    }
    if (event->struct_size < sizeof(*event) ||
        event->abi_version != ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION)
    {
        return ROBOTWEAX_SRT_TELEMETRY_ABI_MISMATCH;
    }
    auto copy = *event;
    copy.struct_size = sizeof(copy);
    copy.detail[sizeof(copy.detail) - 1] = '\0';
    for (auto &c : copy.detail)
    {
        if (c == '\0')
        {
            break;
        }
        const auto byte = static_cast<unsigned char>(c);
        if (byte < 0x20 || byte > 0x7e)
        {
            c = '?';
        }
    }

    std::lock_guard lock(journal->mutex);
    copy.sequence = journal->next_sequence++;
    const auto capacity = journal->ring.size();
    if (journal->size < capacity)
    {
        journal->ring[(journal->head + journal->size) % capacity] = copy;
        journal->size += 1;
    }
    else
    {
        journal->ring[journal->head] = copy;
        journal->head = (journal->head + 1) % capacity;
        journal->overwritten += 1;
    }
    if (sequence != nullptr)
    {
        *sequence = copy.sequence;
    }
    return ROBOTWEAX_SRT_TELEMETRY_OK;
}

extern "C" robotweax_srt_telemetry_error
robotweax_srt_telemetry_event_journal_read(const robotweax_srt_telemetry_event_journal *journal,
                                           const std::uint64_t after_sequence,
                                           robotweax_srt_telemetry_event_v1 *events,
                                           const std::size_t capacity, std::size_t *count)
{
    if (journal == nullptr || count == nullptr || (events == nullptr && capacity > 0))
    {
        return ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT;
    }
    *count = 0;
    std::lock_guard lock(journal->mutex);
    if (journal->size == 0)
    {
        return ROBOTWEAX_SRT_TELEMETRY_OK;
    }
    const auto ring_capacity = journal->ring.size();
    const auto oldest = journal->ring[journal->head].sequence;
    /* Sequences in the ring are contiguous, so the start index is direct. */
    std::size_t skip = 0;
    if (after_sequence >= oldest)
    {
        const auto ahead = after_sequence - oldest + 1;
        if (ahead >= journal->size)
        {
            return ROBOTWEAX_SRT_TELEMETRY_OK;
        }
        skip = static_cast<std::size_t>(ahead);
    }
    const auto available = journal->size - skip;
    const auto copied = std::min(available, capacity);
    for (std::size_t index = 0; index < copied; ++index)
    {
        events[index] = journal->ring[(journal->head + skip + index) % ring_capacity];
    }
    *count = copied;
    return ROBOTWEAX_SRT_TELEMETRY_OK;
}

extern "C" std::size_t
robotweax_srt_telemetry_event_journal_size(const robotweax_srt_telemetry_event_journal *journal)
{
    if (journal == nullptr)
    {
        return 0;
    }
    std::lock_guard lock(journal->mutex);
    return journal->size;
}

extern "C" std::uint64_t robotweax_srt_telemetry_event_journal_overwritten_count(
    const robotweax_srt_telemetry_event_journal *journal)
{
    if (journal == nullptr)
    {
        return 0;
    }
    std::lock_guard lock(journal->mutex);
    return journal->overwritten;
}

extern "C" const char *
robotweax_srt_telemetry_event_kind_name(const robotweax_srt_telemetry_event_kind kind)
{
    switch (kind)
    {
    case ROBOTWEAX_SRT_TELEMETRY_EVENT_HOST:
        return "host";
    case ROBOTWEAX_SRT_TELEMETRY_EVENT_SOURCE_ADDED:
        return "source_added";
    case ROBOTWEAX_SRT_TELEMETRY_EVENT_SOURCE_REMOVED:
        return "source_removed";
    case ROBOTWEAX_SRT_TELEMETRY_EVENT_SOURCE_FAILED:
        return "source_failed";
    case ROBOTWEAX_SRT_TELEMETRY_EVENT_SOURCE_RECOVERED:
        return "source_recovered";
    case ROBOTWEAX_SRT_TELEMETRY_EVENT_SAMPLING_GAP:
        return "sampling_gap";
    case ROBOTWEAX_SRT_TELEMETRY_EVENT_CLOCK_STEP:
        return "clock_step";
    case ROBOTWEAX_SRT_TELEMETRY_EVENT_COUNTER_RESET:
        return "counter_reset";
    case ROBOTWEAX_SRT_TELEMETRY_EVENT_CONNECTED:
        return "connected";
    case ROBOTWEAX_SRT_TELEMETRY_EVENT_DISCONNECTED:
        return "disconnected";
    case ROBOTWEAX_SRT_TELEMETRY_EVENT_REJECTED:
        return "rejected";
    case ROBOTWEAX_SRT_TELEMETRY_EVENT_KM_STATE_CHANGED:
        return "km_state_changed";
    case ROBOTWEAX_SRT_TELEMETRY_EVENT_SETTINGS_CHANGED:
        return "settings_changed";
    case ROBOTWEAX_SRT_TELEMETRY_EVENT_PEER_VERSION_CHANGED:
        return "peer_version_changed";
    case ROBOTWEAX_SRT_TELEMETRY_EVENT_VERDICT_CHANGED:
        return "verdict_changed";
    }
    return "unknown";
}
