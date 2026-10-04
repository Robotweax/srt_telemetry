#include <robotweax/srt_telemetry_health.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <limits>
#include <new>

static_assert(sizeof(robotweax_srt_telemetry_health_indicator_status_v1) == 24,
              "indicator status layout is part of the ABI");
static_assert(sizeof(robotweax_srt_telemetry_health_status_v1) == 72 + 16 * 24,
              "health status layout is part of the ABI");
static_assert(offsetof(robotweax_srt_telemetry_health_status_v1, suggested_value) == 56 &&
                  offsetof(robotweax_srt_telemetry_health_status_v1, indicators) == 72,
              "health status layout is part of the ABI");
static_assert(sizeof(robotweax_srt_telemetry_health_config_v1) == 48, "health config layout is part of the ABI");
static_assert(sizeof(robotweax_srt_telemetry_health_context_v1) == 32, "health context layout is part of the ABI");

namespace
{

using Verdict = robotweax_srt_telemetry_health_verdict;
using Reason = robotweax_srt_telemetry_health_reason;
using Knob = robotweax_srt_telemetry_health_knob;
using Direction = robotweax_srt_telemetry_knob_direction;

constexpr Verdict UNKNOWN = ROBOTWEAX_SRT_TELEMETRY_HEALTH_UNKNOWN;
constexpr Verdict OK = ROBOTWEAX_SRT_TELEMETRY_HEALTH_OK;
constexpr Verdict WATCH = ROBOTWEAX_SRT_TELEMETRY_HEALTH_WATCH;
constexpr Verdict AT_RISK = ROBOTWEAX_SRT_TELEMETRY_HEALTH_AT_RISK;
constexpr Verdict FAILING = ROBOTWEAX_SRT_TELEMETRY_HEALTH_FAILING;

constexpr std::size_t INDICATORS = ROBOTWEAX_SRT_TELEMETRY_INDICATOR_COUNT;
constexpr std::uint64_t SECOND = UINT64_C(1'000'000'000);

enum Indicator : std::size_t
{
    HEADROOM = ROBOTWEAX_SRT_TELEMETRY_INDICATOR_RECOVERY_HEADROOM,
    LATENCY = ROBOTWEAX_SRT_TELEMETRY_INDICATOR_LATENCY_SUFFICIENCY,
    UNRECOVERED = ROBOTWEAX_SRT_TELEMETRY_INDICATOR_UNRECOVERED_RATIO,
    EFFICIENCY = ROBOTWEAX_SRT_TELEMETRY_INDICATOR_RECOVERY_EFFICIENCY,
    OVERHEAD = ROBOTWEAX_SRT_TELEMETRY_INDICATOR_OVERHEAD_BUDGET,
    PATH = ROBOTWEAX_SRT_TELEMETRY_INDICATOR_PATH_HEADROOM,
    BACKLOG = ROBOTWEAX_SRT_TELEMETRY_INDICATOR_SENDER_BACKLOG,
    STALL = ROBOTWEAX_SRT_TELEMETRY_INDICATOR_PEER_READ_STALL,
};

/* Lower values are worse for these indicators. */
bool lower_is_worse(const std::size_t indicator)
{
    return indicator == HEADROOM || indicator == LATENCY || indicator == EFFICIENCY;
}

/* When two indicators share the worst state, the earlier one explains it. */
constexpr std::array<std::size_t, INDICATORS> priority = {UNRECOVERED, BACKLOG, HEADROOM, PATH,
                                                          OVERHEAD,    LATENCY, EFFICIENCY, STALL};

/* Counts summed over the count window. */
enum Count : std::size_t
{
    C_UNIQUE_SENT_BITS,
    C_RETRANSMITTED_BITS,
    C_UNIQUE_SENT_PACKETS,
    C_UNIQUE_RECEIVED_BITS,
    C_UNIQUE_RECEIVED_PACKETS,
    C_RECEIVER_LOSS,
    C_SENDER_LOSS,
    C_SENDER_DROP,
    C_RECEIVER_DROP,
    C_UNDECRYPTABLE,
    C_BELATED,
    COUNTS
};
constexpr std::array<robotweax_srt_telemetry_metric, COUNTS> count_metric = {
    ROBOTWEAX_SRT_TELEMETRY_METRIC_UNIQUE_SENT_BITS_PER_SECOND,
    ROBOTWEAX_SRT_TELEMETRY_METRIC_RETRANSMITTED_BITS_PER_SECOND,
    ROBOTWEAX_SRT_TELEMETRY_METRIC_UNIQUE_SENT_PACKETS_PER_SECOND,
    ROBOTWEAX_SRT_TELEMETRY_METRIC_UNIQUE_RECEIVED_BITS_PER_SECOND,
    ROBOTWEAX_SRT_TELEMETRY_METRIC_UNIQUE_RECEIVED_PACKETS_PER_SECOND,
    ROBOTWEAX_SRT_TELEMETRY_METRIC_RECEIVER_LOSS_PACKETS_PER_SECOND,
    ROBOTWEAX_SRT_TELEMETRY_METRIC_SENDER_LOSS_PACKETS_PER_SECOND,
    ROBOTWEAX_SRT_TELEMETRY_METRIC_SENDER_DROP_PACKETS_PER_SECOND,
    ROBOTWEAX_SRT_TELEMETRY_METRIC_RECEIVER_DROP_PACKETS_PER_SECOND,
    ROBOTWEAX_SRT_TELEMETRY_METRIC_UNDECRYPTABLE_PACKETS_PER_SECOND,
    ROBOTWEAX_SRT_TELEMETRY_METRIC_BELATED_PACKETS_PER_SECOND,
};

/* Gauges examined over the gauge window. */
enum Gauge : std::size_t
{
    G_RTT,
    G_RECEIVER_LATENCY,
    G_SENDER_LATENCY,
    G_LINK_CAPACITY,
    G_HEADROOM,
    G_FLIGHT_TO_FLOW,
    G_SEND_BUFFER_RATIO,
    G_FLOW_WINDOW,
    GAUGES
};
constexpr std::array<robotweax_srt_telemetry_metric, GAUGES> gauge_metric = {
    ROBOTWEAX_SRT_TELEMETRY_METRIC_RTT_SECONDS,
    ROBOTWEAX_SRT_TELEMETRY_METRIC_RECEIVER_TSBPD_DELAY_SECONDS,
    ROBOTWEAX_SRT_TELEMETRY_METRIC_SENDER_TSBPD_DELAY_SECONDS,
    ROBOTWEAX_SRT_TELEMETRY_METRIC_LINK_CAPACITY_BITS_PER_SECOND,
    ROBOTWEAX_SRT_TELEMETRY_METRIC_RECOVERY_HEADROOM_RATIO,
    ROBOTWEAX_SRT_TELEMETRY_METRIC_FLIGHT_TO_FLOW_WINDOW_RATIO,
    ROBOTWEAX_SRT_TELEMETRY_METRIC_SEND_BUFFER_TO_DROP_THRESHOLD_RATIO,
    ROBOTWEAX_SRT_TELEMETRY_METRIC_FLOW_WINDOW_PACKETS,
};

std::uint64_t bit(const robotweax_srt_telemetry_metric metric)
{
    return ROBOTWEAX_SRT_TELEMETRY_METRIC_BIT(metric);
}

struct Entry
{
    std::uint64_t monotonic = 0;
    std::uint64_t interval = 0;
    std::uint64_t available = 0;
    std::array<double, COUNTS> counts{};
    std::array<double, GAUGES> gauges{};
};

struct Thresholds
{
    double watch = 0;
    double at_risk = 0;
    double failing = 0;
};

/* Shipped defaults (see docs/architecture/health-engine.md). */
bool default_thresholds(const robotweax_srt_telemetry_endpoint_role role, const std::size_t indicator,
                        Thresholds &out)
{
    const bool sender = role == ROBOTWEAX_SRT_TELEMETRY_ENDPOINT_ROLE_SENDER;
    const bool receiver = role == ROBOTWEAX_SRT_TELEMETRY_ENDPOINT_ROLE_RECEIVER;
    switch (indicator)
    {
    case HEADROOM:
        out = {0.6, 0.35, 0.15};
        return receiver;
    case LATENCY:
        out = {4.0, 2.5, 1.5};
        return sender || receiver;
    case UNRECOVERED:
        out = {0.0, 1e-3, 1e-2};
        return receiver;
    case EFFICIENCY:
        out = {0.99, 0.9, 0.5};
        return receiver;
    case OVERHEAD:
        out = {0.5, 0.8, 1.0};
        return sender;
    case PATH:
        /* The providers' capacity estimate follows the arrival rate on a
         * healthy path (fault lab: within about 10 %), so the ratio sits
         * near 1; it rises only when the path delivers clearly less than
         * is sent. */
        out = {1.25, 1.5, 2.0};
        return sender;
    case BACKLOG:
        out = {0.35, 0.6, 0.9};
        return sender;
    case STALL:
        if (receiver)
        {
            out = {1.5, 2.5, 4.0};
            return true;
        }
        /* Sender: the larger of flight / flow window and the share of the
         * receive buffer held beyond what the latency explains. */
        out = {0.25, 0.5, 0.9};
        return sender;
    default:
        return false;
    }
}

bool ordered(const std::size_t indicator, const Thresholds &t)
{
    if (!std::isfinite(t.watch) || !std::isfinite(t.at_risk) || !std::isfinite(t.failing))
    {
        return false;
    }
    return lower_is_worse(indicator) ? (t.watch >= t.at_risk && t.at_risk >= t.failing)
                                     : (t.watch <= t.at_risk && t.at_risk <= t.failing);
}

Verdict grade(const std::size_t indicator, const Thresholds &t, const double value)
{
    if (lower_is_worse(indicator))
    {
        return value < t.failing ? FAILING : value < t.at_risk ? AT_RISK : value < t.watch ? WATCH : OK;
    }
    return value > t.failing ? FAILING : value > t.at_risk ? AT_RISK : value > t.watch ? WATCH : OK;
}

struct Advice
{
    Reason reason = ROBOTWEAX_SRT_TELEMETRY_REASON_NONE;
    Knob knob = ROBOTWEAX_SRT_TELEMETRY_KNOB_NONE;
    Direction direction = ROBOTWEAX_SRT_TELEMETRY_KNOB_DIRECTION_NONE;
    double value = 0;
    /* Inferred without direct evidence of a cause (the sender's last
     * resorts: heavy retransmission, or nothing at all). A recent concrete
     * diagnosis of the same drops is preferred to it. */
    bool weak = false;
};

/* One sample's evaluation of one indicator. */
struct Raw
{
    bool applicable = false;
    bool available = false;
    double value = 0;
    Verdict grade = UNKNOWN;
    /* Why unavailable, or the advice when graded worse than OK. */
    Advice advice;
    std::uint64_t missing = 0;
};

struct Hysteresis
{
    Verdict state = UNKNOWN;
    bool up_pending = false;
    std::uint64_t up_since = 0;
    Verdict up_level = UNKNOWN;
    bool down_pending = false;
    std::uint64_t down_since = 0;
    Verdict down_level = UNKNOWN;
    bool ever_known = false;
    std::uint64_t last_known = 0;
    /* Reason and advice behind the current state (when worse than OK). */
    Advice latched;
};

double round_up(const double value, const double step)
{
    return std::ceil(value / step - 1e-9) * step;
}

} // namespace

struct robotweax_srt_telemetry_health_engine
{
    robotweax_srt_telemetry_health_config_v1 config{};
    robotweax_srt_telemetry_health_context_v1 context{};
    std::array<Thresholds, INDICATORS> thresholds{};
    std::array<bool, INDICATORS> role_applies{};
    std::array<Hysteresis, INDICATORS> hysteresis{};

    std::deque<Entry> entries;
    std::array<double, COUNTS> sums{};
    std::array<std::size_t, COUNTS> available_entries{};
    /* Time the count window actually covers (sum of sample intervals). */
    std::uint64_t covered_nano = 0;
    /* Path RTT without queueing: the lowest RTT of loss-free samples over
     * the last 5 minutes, in 10 s buckets. Lossy samples never raise it, so
     * a congested path keeps its diagnosis; a route change without loss is
     * adopted within 5 minutes. With no loss-free sample in 5 minutes the
     * last baseline is kept. */
    bool has_baseline_rtt = false;
    double baseline_rtt = 0;
    struct RttBucket
    {
        std::uint64_t start;
        double min;
    };
    std::deque<RttBucket> rtt_buckets;
    bool has_rtt_bucket = false;
    RttBucket rtt_bucket{0, 0};
    /* Largest flow window seen: a receiver that stops reading shrinks it. */
    double max_flow_window = 0;
    /* Last sample that was not a source error. */
    std::uint64_t last_good = 0;
    bool has_good = false;
    /* Since when a required indicator has had no grade. */
    bool required_missing = false;
    std::uint64_t required_missing_since = 0;
    /* Since when it has had one again: OK needs enter_hold of evidence
     * after a gap, as a worse state does. */
    bool required_seen = false;
    std::uint64_t required_available_since = 0;
    std::uint64_t last_monotonic = 0;
    bool has_last = false;

    /* Recovery headroom is low for about one latency period after data
     * starts; it is not graded until then. */
    bool headroom_seen = false;
    std::uint64_t headroom_first = 0;
    std::uint64_t headroom_last = 0;

    /* Receiver arrival gaps (an outage, or a paused source), remembered for
     * as long as their aftermath can sit in the count window: the drops
     * that arrive with the first packets after a gap belong to the gap, even
     * once the gap itself has left the window. */
    struct ArrivalGap
    {
        std::uint64_t end;      /* monotonic time arrivals resumed */
        std::uint64_t length;   /* nanoseconds without arrivals */
        std::uint64_t interval; /* sampling interval when they resumed */
    };
    std::deque<ArrivalGap> arrival_gaps;
    std::uint64_t arrival_gap_run = 0;
    bool arrivals_seen = false; /* no gap before the stream's first packet */

    /* Drops are diagnosed when they happen. They stay in the count window
     * after their cause has gone (the cap lifted, MAXBW reset, the path
     * back); judged again with the evidence of a healthy moment they would
     * get a wrong reason and knob. The diagnosis of the latest drops is
     * kept for as long as drops are counted without new ones. */
    struct DropDiagnosis
    {
        bool valid = false;
        Advice advice;
        std::uint64_t at = 0; /* when the drops were diagnosed */
    };
    std::array<DropDiagnosis, INDICATORS> drop_diagnosis{};
    /* Sender: last time data waited without acknowledgements (an outage).
     * The drops when the path returns are the outage's: the backlog is
     * too old to send, and the burst of loss reports is no evidence of a
     * bitrate problem. */
    /* Last time the path was diagnosed as congested (queueing), and what
     * it delivered then. Late repairs, drops and low headroom right after
     * are that episode's aftermath: the bitrate, not the latency. */
    bool congestion_seen = false;
    std::uint64_t congestion_last = 0;
    double congestion_rate = 0;
    bool ack_stall_seen = false;
    std::uint64_t ack_stall_last = 0;
    std::uint64_t last_sender_drop = 0; /* monotonic time of the latest */

    Verdict verdict = UNKNOWN;
    std::uint64_t verdict_since = 0;
};

namespace
{

using Engine = robotweax_srt_telemetry_health_engine;

/* Shortest arrival gap worth remembering. */
constexpr std::uint64_t min_arrival_gap = 100'000'000;
/* Most gaps remembered: a trickle stream has many short ones. */
constexpr std::size_t max_arrival_gaps = 64;

/* How long after a gap (or an acknowledgement stall) drops are still its
 * aftermath: packets that were due during it arrive too late, and the
 * sender's backlog, up to its drop threshold max(latency, 1 s) old, drains.
 * 2 s + latency + drop threshold, plus two sampling intervals. */
std::uint64_t aftermath_nano(const double latency_seconds, const std::uint64_t interval)
{
    const double latency = std::clamp(latency_seconds, 0.0, 3600.0);
    return static_cast<std::uint64_t>((2.0 + latency + std::max(latency, 1.0)) * 1e9) + 2 * interval;
}

/* A kept diagnosis whose setting has since been changed as advised: the
 * absolute MAXBW raised enough or replaced (unlimited, or relative with 0),
 * or the latency raised enough (after a reconnect). The drops are still
 * counted, but there is nothing more to change. */
Advice resolved(const Engine &engine, Advice advice, const bool has_latency, const double latency)
{
    const auto &context = engine.context;
    const bool known = (context.flags & ROBOTWEAX_SRT_TELEMETRY_HEALTH_CONTEXT_BANDWIDTH_SETTINGS) != 0;
    bool done = false;
    if (known && advice.knob == ROBOTWEAX_SRT_TELEMETRY_KNOB_MAX_BANDWIDTH)
    {
        done = context.max_bandwidth_bytes_per_second <= 0 ||
               static_cast<double>(context.max_bandwidth_bytes_per_second) >= advice.value;
    }
    else if (has_latency && advice.knob == ROBOTWEAX_SRT_TELEMETRY_KNOB_LATENCY &&
             advice.direction == ROBOTWEAX_SRT_TELEMETRY_KNOB_DIRECTION_INCREASE && advice.value > 0)
    {
        done = latency + 1e-6 >= advice.value;
    }
    if (done)
    {
        advice.knob = ROBOTWEAX_SRT_TELEMETRY_KNOB_NONE;
        advice.direction = ROBOTWEAX_SRT_TELEMETRY_KNOB_DIRECTION_NONE;
        advice.value = 0;
    }
    return advice;
}

void track_arrivals(Engine &engine, const Entry &entry)
{
    if ((entry.available & bit(count_metric[C_UNIQUE_RECEIVED_PACKETS])) == 0)
    {
        return;
    }
    if (entry.counts[C_UNIQUE_RECEIVED_PACKETS] > 0)
    {
        if (engine.arrivals_seen && engine.arrival_gap_run >= min_arrival_gap)
        {
            engine.arrival_gaps.push_back({entry.monotonic > entry.interval ? entry.monotonic - entry.interval : 0,
                                           engine.arrival_gap_run, entry.interval});
            if (engine.arrival_gaps.size() > max_arrival_gaps)
            {
                engine.arrival_gaps.pop_front();
            }
        }
        engine.arrivals_seen = true;
        engine.arrival_gap_run = 0;
    }
    else if (engine.arrivals_seen)
    {
        engine.arrival_gap_run += entry.interval;
    }
    /* Kept while their aftermath can be in the count window; the latency
     * is not known here, so with the largest latency the aftermath allows
     * for a minute. */
    const auto keep = engine.config.count_window_nano + 64'000'000'000ull;
    while (!engine.arrival_gaps.empty() && engine.arrival_gaps.front().end + keep <= entry.monotonic)
    {
        engine.arrival_gaps.pop_front();
    }
}

void add_entry(Engine &engine, const Entry &entry)
{
    track_arrivals(engine, entry);
    engine.entries.push_back(entry);
    engine.covered_nano += entry.interval;
    for (std::size_t c = 0; c < COUNTS; ++c)
    {
        if ((entry.available & bit(count_metric[c])) != 0)
        {
            engine.sums[c] += entry.counts[c];
            engine.available_entries[c] += 1;
        }
    }
    const auto now = entry.monotonic;
    const auto window = engine.config.count_window_nano;
    while (!engine.entries.empty() && engine.entries.front().monotonic + window <= now)
    {
        const auto &old = engine.entries.front();
        engine.covered_nano -= old.interval;
        for (std::size_t c = 0; c < COUNTS; ++c)
        {
            if ((old.available & bit(count_metric[c])) != 0)
            {
                engine.sums[c] -= old.counts[c];
                engine.available_entries[c] -= 1;
            }
        }
        engine.entries.pop_front();
    }
    if (engine.entries.size() == 1)
    {
        engine.covered_nano = entry.interval;
        /* Rounding from subtraction cannot accumulate past an empty window. */
        for (std::size_t c = 0; c < COUNTS; ++c)
        {
            const bool has = (entry.available & bit(count_metric[c])) != 0;
            engine.sums[c] = has ? entry.counts[c] : 0.0;
        }
    }
}

struct GaugeView
{
    std::array<bool, GAUGES> available{};
    std::array<double, GAUGES> min{};
    std::array<double, GAUGES> max{};
    std::array<double, GAUGES> sum{};
    std::array<std::size_t, GAUGES> samples{};
    std::array<double, GAUGES> last{};
    /* Sent bits and covered time inside the gauge window. */
    double sent_bits = 0;
    double retransmitted_bits = 0;
    double sent_interval = 0;
    /* Unique packets inside the gauge window: recent activity. */
    double received_packets = 0;
    double sent_packets = 0;
};

GaugeView gauge_view(const Engine &engine, const std::uint64_t now)
{
    GaugeView view;
    for (std::size_t g = 0; g < GAUGES; ++g)
    {
        view.min[g] = std::numeric_limits<double>::infinity();
        view.max[g] = -std::numeric_limits<double>::infinity();
    }
    const auto window = engine.config.gauge_window_nano;
    const auto sent_mask = bit(count_metric[C_UNIQUE_SENT_BITS]);
    for (auto it = engine.entries.rbegin(); it != engine.entries.rend(); ++it)
    {
        if (it->monotonic + window <= now)
        {
            break;
        }
        for (std::size_t g = 0; g < GAUGES; ++g)
        {
            if ((it->available & bit(gauge_metric[g])) == 0)
            {
                continue;
            }
            const auto v = it->gauges[g];
            if (!view.available[g])
            {
                view.last[g] = v; /* newest first */
            }
            view.available[g] = true;
            view.min[g] = std::min(view.min[g], v);
            view.max[g] = std::max(view.max[g], v);
            view.sum[g] += v;
            view.samples[g] += 1;
        }
        if ((it->available & bit(count_metric[C_UNIQUE_RECEIVED_PACKETS])) != 0)
        {
            view.received_packets += it->counts[C_UNIQUE_RECEIVED_PACKETS];
        }
        if ((it->available & bit(count_metric[C_UNIQUE_SENT_PACKETS])) != 0)
        {
            view.sent_packets += it->counts[C_UNIQUE_SENT_PACKETS];
        }
        if ((it->available & sent_mask) != 0 && it->interval > 0)
        {
            view.sent_bits += it->counts[C_UNIQUE_SENT_BITS];
            if ((it->available & bit(count_metric[C_RETRANSMITTED_BITS])) != 0)
            {
                view.sent_bits += it->counts[C_RETRANSMITTED_BITS];
                view.retransmitted_bits += it->counts[C_RETRANSMITTED_BITS];
            }
            view.sent_interval += static_cast<double>(it->interval) / 1e9;
        }
    }
    return view;
}

/* A receiver that stopped reading shrinks the flow window; once its buffer
 * overflows, the window can recover while the sender keeps dropping, so the
 * evidence is kept for the count window. */
bool flow_window_shrunk(const Engine &engine)
{
    if (engine.max_flow_window <= 0)
    {
        return false;
    }
    for (const auto &entry : engine.entries)
    {
        if ((entry.available & bit(gauge_metric[G_FLOW_WINDOW])) != 0 &&
            entry.gauges[G_FLOW_WINDOW] < 0.5 * engine.max_flow_window)
        {
            return true;
        }
    }
    return false;
}

bool count_available(const Engine &engine, const Count c)
{
    return engine.available_entries[c] > 0;
}

std::uint64_t missing_counts(const Engine &engine, std::initializer_list<Count> counts)
{
    std::uint64_t missing = 0;
    for (const auto c : counts)
    {
        if (!count_available(engine, c))
        {
            missing |= bit(count_metric[c]);
        }
    }
    return missing;
}

/* Context shared by the indicator computations of one sample. */
struct Evaluation
{
    const Engine &engine;
    GaugeView gauges;
    bool receiver;
    /* latency / RTT when known (for advice). */
    bool has_latency;
    double latency = 0;
    double rtt = 0;
    /* The path queues: RTT well above its baseline with sustained loss (a
     * short queue that drops), or RTT climbing steadily without loss (a
     * deep queue that fills). Latency does not fix that; the bitrate does. */
    bool congested = false;
    double congestion_rate = 0; /* bit/s the path delivers, for advice */
};

Advice bitrate_decrease(Reason reason, double bits_per_second);

/* How long RTT must climb to count as a queue filling. */
constexpr std::uint64_t queue_span_nano = 3 * SECOND;

/* A deep queue filling on the path: over queue_span, RTT rises by more
 * than the baseline (and at least 50 ms) to over twice the baseline, the
 * rise is spread over the span (at least a quarter of it in each half), and
 * RTT falls in at most 30 % of the sample pairs. A step (route change, the
 * fault lab's rtt_step, or a staircase of two or three samples) puts its
 * whole rise into one half; jitter falls as often as it rises; a slow drift
 * does not add 50 ms in 3 s. Works from about 1 s sampling down (three RTT
 * samples), and repeated equal values (libsrt updates RTT less often than a
 * fine sampler reads it) do not count against it. Returns the bit/s that
 * arrived meanwhile (receiver: what the path delivers), or a negative value
 * when the RTT is not climbing. */
double queue_filling(const Engine &engine, const bool receiver, const std::uint64_t now)
{
    if (!engine.has_baseline_rtt || now < queue_span_nano || engine.entries.empty())
    {
        return -1.0;
    }
    const auto since = now - queue_span_nano;
    const auto in_span = [&](const Entry &entry)
    { return entry.monotonic >= since && (entry.available & bit(gauge_metric[G_RTT])) != 0; };
    /* First pass: the span's ends, falls and what arrived (no allocation). */
    std::size_t points = 0;
    std::size_t falls = 0;
    std::uint64_t start_at = 0;
    std::uint64_t end_at = 0;
    double start = 0;
    double end = 0;
    double bits = 0;
    double seconds = 0;
    for (const auto &entry : engine.entries)
    {
        if (!in_span(entry))
        {
            continue;
        }
        const auto rtt = entry.gauges[G_RTT];
        if (points == 0)
        {
            start = rtt;
            start_at = entry.monotonic;
        }
        else
        {
            falls += rtt < end ? 1u : 0u;
            const auto count = receiver ? C_UNIQUE_RECEIVED_BITS : C_UNIQUE_SENT_BITS;
            if ((entry.available & bit(count_metric[count])) != 0)
            {
                bits += entry.counts[count];
                seconds += static_cast<double>(entry.interval) / 1e9;
            }
        }
        end = rtt;
        end_at = entry.monotonic;
        points += 1;
    }
    /* The span the samples cover may miss up to two sampling intervals. */
    const auto slack = std::max<std::uint64_t>(200'000'000, 2 * engine.entries.back().interval);
    if (points < 3 || now - start_at + slack < queue_span_nano)
    {
        return -1.0;
    }
    /* Second pass: the sample nearest the middle of the span. */
    const auto middle_time = start_at + (end_at - start_at) / 2;
    std::uint64_t best = std::numeric_limits<std::uint64_t>::max();
    double middle = start;
    for (const auto &entry : engine.entries)
    {
        if (!in_span(entry) || entry.monotonic == start_at || entry.monotonic == end_at)
        {
            continue;
        }
        const auto distance =
            entry.monotonic > middle_time ? entry.monotonic - middle_time : middle_time - entry.monotonic;
        if (distance < best)
        {
            best = distance;
            middle = entry.gauges[G_RTT];
        }
    }
    const auto baseline = engine.baseline_rtt;
    const auto rise = end - start;
    if (rise < std::max(0.05, baseline) || end <= 2.0 * baseline + 0.01 || middle - start < 0.25 * rise ||
        end - middle < 0.25 * rise || 10 * falls > 3 * (points - 1))
    {
        return -1.0;
    }
    return seconds > 0 ? bits / seconds : 0.0;
}

Advice latency_advice(const Evaluation &e, const Reason reason, const double loss_ratio)
{
    if (e.congested)
    {
        return bitrate_decrease(ROBOTWEAX_SRT_TELEMETRY_REASON_PATH_CONGESTED, 0.8 * e.congestion_rate);
    }
    Advice advice{reason, ROBOTWEAX_SRT_TELEMETRY_KNOB_LATENCY,
                  ROBOTWEAX_SRT_TELEMETRY_KNOB_DIRECTION_INCREASE, 0.0};
    if (e.has_latency)
    {
        /* Enough rounds for the RTT (more with heavy loss), and a clear step
         * above the current latency. */
        const double rounds = loss_ratio > 0.01 ? 6.0 : 4.0;
        const double target = std::max(e.latency * 1.5, e.rtt * rounds);
        advice.value = round_up(target, 0.01);
    }
    return advice;
}

Advice bitrate_decrease(const Reason reason, const double bits_per_second)
{
    return {reason, ROBOTWEAX_SRT_TELEMETRY_KNOB_BITRATE, ROBOTWEAX_SRT_TELEMETRY_KNOB_DIRECTION_DECREASE,
            bits_per_second > 0 ? round_up(bits_per_second, 1000.0) : 0.0};
}

Raw unavailable(const Reason reason, const std::uint64_t missing = 0)
{
    Raw raw;
    raw.applicable = true;
    raw.advice.reason = reason;
    raw.missing = missing;
    return raw;
}

Raw graded(const Evaluation &e, const std::size_t indicator, const double value, const Advice &advice)
{
    Raw raw;
    raw.applicable = true;
    raw.available = true;
    raw.value = value;
    raw.grade = grade(indicator, e.engine.thresholds[indicator], value);
    if (raw.grade != OK)
    {
        raw.advice = advice;
    }
    return raw;
}

double sum(const Engine &engine, const Count c)
{
    return engine.sums[c] > 0 ? engine.sums[c] : 0.0;
}

double receiver_loss_ratio(const Engine &engine)
{
    const auto unique = sum(engine, C_UNIQUE_RECEIVED_PACKETS);
    const auto loss = sum(engine, C_RECEIVER_LOSS);
    return unique + loss > 0 ? loss / (unique + loss) : 0.0;
}

/* Advice for drops that are not decryption failures. */
/* Loss in most of the count window's samples is the path; a burst (an
 * outage) is not. */
bool sustained_loss(const Engine &engine, const Count c)
{
    std::size_t with_loss = 0;
    std::size_t with_count = 0;
    for (const auto &entry : engine.entries)
    {
        if ((entry.available & bit(count_metric[c])) != 0)
        {
            with_count += 1;
            with_loss += entry.counts[c] > 0 ? 1u : 0u;
        }
    }
    return with_count > 0 && 2 * with_loss >= with_count;
}

/* Any loss report in the last gauge window. */
bool recent_loss(const Engine &engine, const Count c, const std::uint64_t now)
{
    for (auto it = engine.entries.rbegin(); it != engine.entries.rend(); ++it)
    {
        if (it->monotonic + engine.config.gauge_window_nano <= now)
        {
            break;
        }
        if ((it->available & bit(count_metric[c])) != 0 && it->counts[c] > 0)
        {
            return true;
        }
    }
    return false;
}

/* Within the aftermath of the sender's latest acknowledgement stall (an
 * outage): its stale backlog, the burst of loss reports and the repairs
 * when the path returns are the outage's, not a setting's. */
bool after_ack_stall(const Engine &engine, const double latency, const std::uint64_t at_least = 0)
{
    if (!engine.ack_stall_seen || engine.entries.empty())
    {
        return false;
    }
    return engine.last_monotonic - engine.ack_stall_last <=
           std::max(at_least, aftermath_nano(latency, engine.entries.back().interval));
}

Advice drop_advice(const Evaluation &e, const Reason fallback)
{
    const auto &engine = e.engine;
    const auto loss_ratio = receiver_loss_ratio(engine);
    /* Arrivals stopped for longer than the latency (an outage): no
     * retransmission recovers that gap and no setting brings it back. The
     * drops that come with the first packets after the gap are the
     * outage's, even once the gap has left the count window; drops outside
     * that aftermath call for a setting. */
    if (e.has_latency && e.receiver)
    {
        double after_gap = 0;
        double other = 0;
        for (const auto &entry : engine.entries)
        {
            const auto drops = entry.counts[C_RECEIVER_DROP];
            if ((entry.available & bit(count_metric[C_RECEIVER_DROP])) == 0 || drops <= 0)
            {
                continue;
            }
            bool aftermath = false;
            for (const auto &g : engine.arrival_gaps)
            {
                if (static_cast<double>(g.length) / 1e9 > e.latency && entry.monotonic >= g.end &&
                    entry.monotonic <= g.end + aftermath_nano(e.latency, g.interval))
                {
                    aftermath = true;
                    break;
                }
            }
            (aftermath ? after_gap : other) += drops;
        }
        if (after_gap > 0 && other <= 0)
        {
            return {fallback, ROBOTWEAX_SRT_TELEMETRY_KNOB_NONE, ROBOTWEAX_SRT_TELEMETRY_KNOB_DIRECTION_NONE, 0.0};
        }
    }
    if (sum(engine, C_BELATED) > 0)
    {
        return latency_advice(e, ROBOTWEAX_SRT_TELEMETRY_REASON_LATE_REPAIRS, loss_ratio);
    }
    const bool sustained = sustained_loss(engine, C_RECEIVER_LOSS);
    if (sustained && e.has_latency && e.rtt > 0 && e.latency / e.rtt >= 8.0 && loss_ratio > 0.05)
    {
        /* Plenty of retransmission rounds and still losing: the path cannot
         * carry the stream plus its repairs. */
        const double seconds = static_cast<double>(e.engine.covered_nano) / 1e9;
        return bitrate_decrease(fallback, seconds > 0 ? 0.8 * sum(engine, C_UNIQUE_RECEIVED_BITS) / seconds : 0.0);
    }
    return latency_advice(e, fallback, loss_ratio);
}

Raw compute(const Evaluation &e, const std::size_t indicator)
{
    const auto &engine = e.engine;
    const auto &g = e.gauges;
    /* Rates over the counts use the time the window really covers: shorter
     * than the window at the start of a stream or after an outage. */
    const auto count_seconds = static_cast<double>(engine.covered_nano) / 1e9;
    switch (indicator)
    {
    case HEADROOM:
    {
        if (!g.available[G_HEADROOM])
        {
            const auto unique = sum(engine, C_UNIQUE_RECEIVED_PACKETS);
            return unique > 0 ? unavailable(ROBOTWEAX_SRT_TELEMETRY_REASON_MISSING_METRIC,
                                            bit(gauge_metric[G_HEADROOM]))
                              : unavailable(ROBOTWEAX_SRT_TELEMETRY_REASON_NO_DATA);
        }
        const auto warmup = std::max<std::uint64_t>(
            engine.config.gauge_window_nano,
            e.has_latency ? static_cast<std::uint64_t>(1.5 * std::clamp(e.latency, 0.0, 3600.0) * 1e9) : SECOND);
        if (engine.last_monotonic < engine.headroom_first + warmup)
        {
            return unavailable(ROBOTWEAX_SRT_TELEMETRY_REASON_NO_DATA);
        }
        return graded(e, indicator, g.min[G_HEADROOM],
                      latency_advice(e, ROBOTWEAX_SRT_TELEMETRY_REASON_RECOVERY_HEADROOM_LOW,
                                     receiver_loss_ratio(engine)));
    }
    case LATENCY:
    {
        const auto latency_gauge = e.receiver ? G_RECEIVER_LATENCY : G_SENDER_LATENCY;
        std::uint64_t missing = 0;
        if (!g.available[latency_gauge])
        {
            missing |= bit(gauge_metric[latency_gauge]);
        }
        if (!g.available[G_RTT])
        {
            missing |= bit(gauge_metric[G_RTT]);
        }
        if (missing != 0)
        {
            return unavailable(ROBOTWEAX_SRT_TELEMETRY_REASON_MISSING_METRIC, missing);
        }
        if (e.rtt <= 0)
        {
            return unavailable(ROBOTWEAX_SRT_TELEMETRY_REASON_NO_DATA);
        }
        return graded(e, indicator, e.latency / e.rtt,
                      latency_advice(e, ROBOTWEAX_SRT_TELEMETRY_REASON_LATENCY_LOW_FOR_RTT,
                                     e.receiver ? receiver_loss_ratio(engine) : 0.0));
    }
    case UNRECOVERED:
    {
        const auto missing = missing_counts(engine, {C_UNIQUE_RECEIVED_PACKETS, C_RECEIVER_DROP});
        if (missing != 0)
        {
            return unavailable(ROBOTWEAX_SRT_TELEMETRY_REASON_MISSING_METRIC, missing);
        }
        const auto unique = sum(engine, C_UNIQUE_RECEIVED_PACKETS);
        const auto drops = sum(engine, C_RECEIVER_DROP);
        /* Nothing arriving now: the counts describe the past, not the
         * stream; no grade (the sender may have paused, or the path died). */
        if (unique + drops <= 0 || g.received_packets <= 0)
        {
            return unavailable(ROBOTWEAX_SRT_TELEMETRY_REASON_NO_DATA);
        }
        Advice advice;
        if (sum(engine, C_UNDECRYPTABLE) > 0)
        {
            advice = {ROBOTWEAX_SRT_TELEMETRY_REASON_UNDECRYPTABLE, ROBOTWEAX_SRT_TELEMETRY_KNOB_PASSPHRASE,
                      ROBOTWEAX_SRT_TELEMETRY_KNOB_DIRECTION_CHECK, 0.0};
        }
        else
        {
            advice = drop_advice(e, ROBOTWEAX_SRT_TELEMETRY_REASON_UNRECOVERED_LOSS);
        }
        return graded(e, indicator, drops / (unique + drops), advice);
    }
    case EFFICIENCY:
    {
        const auto missing = missing_counts(engine, {C_RECEIVER_LOSS, C_RECEIVER_DROP});
        if (missing != 0)
        {
            return unavailable(ROBOTWEAX_SRT_TELEMETRY_REASON_MISSING_METRIC, missing);
        }
        if (sum(engine, C_UNIQUE_RECEIVED_PACKETS) + sum(engine, C_RECEIVER_DROP) <= 0)
        {
            return unavailable(ROBOTWEAX_SRT_TELEMETRY_REASON_NO_DATA);
        }
        const auto loss = sum(engine, C_RECEIVER_LOSS);
        /* Too little loss to judge repair; the unrecovered ratio still
         * reports any drop. */
        constexpr double minimum_loss = 10.0;
        if (loss < minimum_loss)
        {
            return unavailable(ROBOTWEAX_SRT_TELEMETRY_REASON_NONE);
        }
        const auto drops = std::max(0.0, sum(engine, C_RECEIVER_DROP) - sum(engine, C_UNDECRYPTABLE));
        const auto denominator = std::max(loss, drops);
        return graded(e, indicator, 1.0 - std::min(drops, denominator) / denominator,
                      drop_advice(e, ROBOTWEAX_SRT_TELEMETRY_REASON_RECOVERY_INEFFICIENT));
    }
    case OVERHEAD:
    {
        const auto &context = engine.context;
        if ((context.flags & ROBOTWEAX_SRT_TELEMETRY_HEALTH_CONTEXT_BANDWIDTH_SETTINGS) == 0)
        {
            return unavailable(ROBOTWEAX_SRT_TELEMETRY_REASON_MISSING_SETTING);
        }
        if (context.max_bandwidth_bytes_per_second < 0)
        {
            Raw raw; /* unlimited: no budget to exhaust */
            return raw;
        }
        const auto missing = missing_counts(engine, {C_UNIQUE_SENT_BITS, C_RETRANSMITTED_BITS});
        if (missing != 0)
        {
            return unavailable(ROBOTWEAX_SRT_TELEMETRY_REASON_MISSING_METRIC, missing);
        }
        const auto unique = sum(engine, C_UNIQUE_SENT_BITS);
        const auto retransmitted = sum(engine, C_RETRANSMITTED_BITS);
        if (unique <= 0 || engine.covered_nano < engine.config.gauge_window_nano)
        {
            return unavailable(ROBOTWEAX_SRT_TELEMETRY_REASON_NO_DATA);
        }
        Advice advice;
        double allowed = 0;
        if (context.max_bandwidth_bytes_per_second > 0)
        {
            const auto cap = static_cast<double>(context.max_bandwidth_bytes_per_second) * 8.0 * count_seconds;
            allowed = cap - unique;
            advice = {ROBOTWEAX_SRT_TELEMETRY_REASON_OVERHEAD_BUDGET_HIGH,
                      ROBOTWEAX_SRT_TELEMETRY_KNOB_MAX_BANDWIDTH, ROBOTWEAX_SRT_TELEMETRY_KNOB_DIRECTION_INCREASE,
                      round_up(1.25 * (unique + retransmitted) / count_seconds / 8.0, 1000.0)};
        }
        else
        {
            const auto percent = static_cast<double>(std::max<std::int64_t>(context.overhead_percent, 0));
            allowed = unique * percent / 100.0;
            /* SRTO_OHEADBW accepts 5..100 percent. */
            const auto needed = std::max(round_up(1.5 * 100.0 * retransmitted / unique, 5.0), percent + 5.0);
            if (needed <= 100.0)
            {
                advice = {ROBOTWEAX_SRT_TELEMETRY_REASON_OVERHEAD_BUDGET_HIGH,
                          ROBOTWEAX_SRT_TELEMETRY_KNOB_OVERHEAD_BANDWIDTH,
                          ROBOTWEAX_SRT_TELEMETRY_KNOB_DIRECTION_INCREASE, needed};
            }
            else
            {
                advice = bitrate_decrease(ROBOTWEAX_SRT_TELEMETRY_REASON_OVERHEAD_BUDGET_HIGH,
                                          0.8 * unique / count_seconds);
            }
        }
        const auto value = allowed > 0 ? retransmitted / allowed
                                       : (retransmitted > 0 ? std::numeric_limits<double>::infinity() : 0.0);
        /* Repairs of an outage's backlog are a burst while the count
         * window (or the outage's aftermath, if longer) holds them: the
         * budget did not run short, the path was gone. Graded, but no
         * setting to change (srt_telemetry#27). */
        if (after_ack_stall(engine, e.has_latency ? e.latency : 0.0, engine.config.count_window_nano))
        {
            advice = {ROBOTWEAX_SRT_TELEMETRY_REASON_ACKNOWLEDGEMENTS_STALLED, ROBOTWEAX_SRT_TELEMETRY_KNOB_NONE,
                      ROBOTWEAX_SRT_TELEMETRY_KNOB_DIRECTION_NONE, 0.0, true};
        }
        auto raw = graded(e, indicator, std::isfinite(value) ? value : 1e9, advice);
        return raw;
    }
    case PATH:
    {
        if (!g.available[G_LINK_CAPACITY])
        {
            return unavailable(ROBOTWEAX_SRT_TELEMETRY_REASON_MISSING_METRIC, bit(gauge_metric[G_LINK_CAPACITY]));
        }
        const auto capacity = g.sum[G_LINK_CAPACITY] / static_cast<double>(g.samples[G_LINK_CAPACITY]);
        if (g.sent_interval <= 0 || capacity <= 0)
        {
            return unavailable(ROBOTWEAX_SRT_TELEMETRY_REASON_NO_DATA);
        }
        const auto rate = g.sent_bits / g.sent_interval;
        if (rate <= 0)
        {
            return unavailable(ROBOTWEAX_SRT_TELEMETRY_REASON_NO_DATA);
        }
        auto raw = graded(e, indicator, rate / capacity,
                          bitrate_decrease(ROBOTWEAX_SRT_TELEMETRY_REASON_PATH_CAPACITY_EXCEEDED, 0.7 * capacity));
        /* The estimate is not evidence on its own: it swings by about 25 %
         * on a clean path and jitter pulls it far down (fault lab). The
         * ratio is graded only with congestion evidence at this end: a
         * filling send buffer or sender drops. */
        const bool congested =
            e.congested ||
            (g.available[G_SEND_BUFFER_RATIO] && g.max[G_SEND_BUFFER_RATIO] > engine.thresholds[BACKLOG].watch) ||
            sum(engine, C_SENDER_DROP) > 0;
        if (!congested)
        {
            raw.grade = OK;
            raw.advice = {};
        }
        return raw;
    }
    case BACKLOG:
    {
        const bool has_ratio = g.available[G_SEND_BUFFER_RATIO];
        const bool has_drops = count_available(engine, C_SENDER_DROP) && count_available(engine, C_UNIQUE_SENT_PACKETS);
        if (!has_ratio && !has_drops)
        {
            return unavailable(ROBOTWEAX_SRT_TELEMETRY_REASON_MISSING_METRIC,
                               bit(gauge_metric[G_SEND_BUFFER_RATIO]) |
                                   missing_counts(engine, {C_SENDER_DROP, C_UNIQUE_SENT_PACKETS}));
        }
        const auto drops = sum(engine, C_SENDER_DROP);
        const auto sent = sum(engine, C_UNIQUE_SENT_PACKETS);
        if (!has_ratio && (drops + sent <= 0 || g.sent_packets <= 0))
        {
            return unavailable(ROBOTWEAX_SRT_TELEMETRY_REASON_NO_DATA);
        }
        /* Advice follows the evidence (fault lab):
         * 1. flow window shrunk: the receiving application is not reading;
         * 2. sending at an absolute MAXBW: the cap is too low;
         * 3. sending clearly above the estimated capacity: lower the bitrate;
         * 4. latency short for the RTT: raise the latency;
         * 5. heavy retransmission (congestion): lower the bitrate;
         * 6. otherwise data waits without loss reports: acknowledgements
         *    stopped (path or peer unresponsive); no knob helps. */
        Advice advice;
        const auto reason = drops > 0 ? ROBOTWEAX_SRT_TELEMETRY_REASON_SENDER_DROPS
                                      : ROBOTWEAX_SRT_TELEMETRY_REASON_SENDER_BACKLOG;
        const bool short_latency = e.has_latency && e.rtt > 0 &&
                                   e.latency / e.rtt < engine.thresholds[LATENCY].watch;
        const double capacity = g.available[G_LINK_CAPACITY]
                                    ? g.sum[G_LINK_CAPACITY] / static_cast<double>(g.samples[G_LINK_CAPACITY])
                                    : 0.0;
        const double rate = g.sent_interval > 0 ? g.sent_bits / g.sent_interval : 0.0;
        const double unique_rate = rate - (g.sent_interval > 0 ? g.retransmitted_bits / g.sent_interval : 0.0);
        const bool shrunk = flow_window_shrunk(engine);
        const auto max_bandwidth = engine.context.max_bandwidth_bytes_per_second;
        const bool bandwidth_known =
            (engine.context.flags & ROBOTWEAX_SRT_TELEMETRY_HEALTH_CONTEXT_BANDWIDTH_SETTINGS) != 0;
        if (shrunk)
        {
            advice = {ROBOTWEAX_SRT_TELEMETRY_REASON_PEER_READ_STALL,
                      ROBOTWEAX_SRT_TELEMETRY_KNOB_RECEIVING_APPLICATION,
                      ROBOTWEAX_SRT_TELEMETRY_KNOB_DIRECTION_CHECK, 0.0};
        }
        else if (bandwidth_known && max_bandwidth > 0 && rate >= 0.9 * static_cast<double>(max_bandwidth) * 8.0)
        {
            /* What the application offers: what went out plus what the
             * sender dropped, at the average packet size. */
            const auto packets = sum(engine, C_UNIQUE_SENT_PACKETS);
            const double packet_bits = packets > 0 ? sum(engine, C_UNIQUE_SENT_BITS) / packets : 0.0;
            const double dropped_rate = count_seconds > 0 ? drops * packet_bits / count_seconds : 0.0;
            advice = {reason, ROBOTWEAX_SRT_TELEMETRY_KNOB_MAX_BANDWIDTH,
                      ROBOTWEAX_SRT_TELEMETRY_KNOB_DIRECTION_INCREASE,
                      round_up(1.25 * (unique_rate + dropped_rate) / 8.0, 1000.0)};
        }
        else if (capacity > 0 && rate / capacity > engine.thresholds[PATH].watch &&
                 (e.congested || recent_loss(engine, C_SENDER_LOSS, engine.last_monotonic) ||
                  (engine.has_baseline_rtt && e.rtt > 1.5 * engine.baseline_rtt + 0.005)) &&
                 !after_ack_stall(engine, e.has_latency ? e.latency : 0.0))
        {
            /* Loss reports count only while they come (a capped path keeps
             * sending them; an outage's burst stays in the count window long
             * after the path is back). Not in an outage's aftermath: the
             * repair burst inflates the rate, and the estimate is stale. */
            /* The estimate follows the arrival rate and goes stale when
             * nothing is acknowledged: above it only counts with evidence
             * that the path is busy, either loss reports or an RTT that
             * queues. Without either, data waiting is an outage (below). */
            advice = bitrate_decrease(reason, 0.7 * capacity);
        }
        else if (short_latency)
        {
            advice = latency_advice(e, reason, 0.0);
        }
        else if (unique_rate > 0 && rate - unique_rate > 0.1 * unique_rate &&
                 recent_loss(engine, C_SENDER_LOSS, engine.last_monotonic) &&
                 !after_ack_stall(engine, e.has_latency ? e.latency : 0.0))
        {
            /* Repairs the receiver asks for. Retransmissions without any
             * loss report are the sender's timers re-sending what nobody
             * acknowledges (an outage), and the burst of reports when the
             * path returns is the outage's aftermath: neither is a bitrate
             * problem. */
            advice = bitrate_decrease(reason, 0.8 * unique_rate);
            advice.weak = true;
        }
        else
        {
            advice = {ROBOTWEAX_SRT_TELEMETRY_REASON_ACKNOWLEDGEMENTS_STALLED, ROBOTWEAX_SRT_TELEMETRY_KNOB_NONE,
                      ROBOTWEAX_SRT_TELEMETRY_KNOB_DIRECTION_NONE, 0.0, true};
        }
        const auto value = has_ratio ? g.max[G_SEND_BUFFER_RATIO] : 0.0;
        auto raw = graded(e, indicator, value, advice);
        /* Packets the sender gave up on never reach the viewer. */
        if (drops > 0)
        {
            const auto ratio = drops / (drops + sent);
            const auto floor = ratio > 1e-3 ? FAILING : AT_RISK;
            if (raw.grade < floor)
            {
                raw.grade = floor;
            }
            raw.advice = advice;
        }
        return raw;
    }
    case STALL:
    {
        if (e.receiver)
        {
            /* Data waiting longer than the latency: the application is not
             * taking delivered packets. */
            if (!g.available[G_HEADROOM])
            {
                return unavailable(ROBOTWEAX_SRT_TELEMETRY_REASON_NO_DATA);
            }
            return graded(e, indicator, g.max[G_HEADROOM],
                          {ROBOTWEAX_SRT_TELEMETRY_REASON_APPLICATION_READ_STALL,
                           ROBOTWEAX_SRT_TELEMETRY_KNOB_RECEIVING_APPLICATION,
                           ROBOTWEAX_SRT_TELEMETRY_KNOB_DIRECTION_CHECK, 0.0});
        }
        /* A receiver that stops reading shrinks the flow window it
         * advertises (its free buffer) long before the flight fills it. The
         * part of the buffer held beyond about 1.5 latency periods of the
         * stream is the stall. */
        const bool has_window = g.available[G_FLOW_WINDOW] && engine.max_flow_window > 0;
        if (!g.available[G_FLIGHT_TO_FLOW] && !has_window)
        {
            return unavailable(ROBOTWEAX_SRT_TELEMETRY_REASON_MISSING_METRIC,
                               bit(gauge_metric[G_FLIGHT_TO_FLOW]) | bit(gauge_metric[G_FLOW_WINDOW]));
        }
        double occupancy = 0;
        /* Without the latency the normal buffering is unknown: no grade. */
        if (has_window && e.has_latency)
        {
            const double pps = g.sent_interval > 0 ? g.sent_packets / g.sent_interval : 0.0;
            const double expected = e.has_latency ? 1.5 * (e.latency + e.rtt) * pps : 0.0;
            occupancy = std::max(0.0, engine.max_flow_window - std::max(g.min[G_FLOW_WINDOW], 0.0) - expected) /
                        engine.max_flow_window;
        }
        const double flight = g.available[G_FLIGHT_TO_FLOW] ? g.max[G_FLIGHT_TO_FLOW] : 0.0;
        /* A full flight with an unchanged window means the
         * acknowledgements stopped (path or peer unresponsive). */
        const bool shrunk = flow_window_shrunk(engine);
        const Advice advice = shrunk ? Advice{ROBOTWEAX_SRT_TELEMETRY_REASON_PEER_READ_STALL,
                                              ROBOTWEAX_SRT_TELEMETRY_KNOB_RECEIVING_APPLICATION,
                                              ROBOTWEAX_SRT_TELEMETRY_KNOB_DIRECTION_CHECK, 0.0}
                                     : Advice{ROBOTWEAX_SRT_TELEMETRY_REASON_ACKNOWLEDGEMENTS_STALLED,
                                              ROBOTWEAX_SRT_TELEMETRY_KNOB_NONE,
                                              ROBOTWEAX_SRT_TELEMETRY_KNOB_DIRECTION_NONE, 0.0};
        return graded(e, indicator, std::max(flight, occupancy), advice);
    }
    default:
        return {};
    }
}

void update_hysteresis(const Engine &engine, Hysteresis &h, const Raw &raw, const std::uint64_t now)
{
    if (!raw.available)
    {
        /* No evidence: hold the state, and give it up after leave_hold. A
         * pending escalation survives gaps shorter than enter_hold; a
         * pending improvement needs unbroken evidence. */
        if (h.up_pending && now - h.last_known >= engine.config.enter_hold_nano)
        {
            h.up_pending = false;
        }
        h.down_pending = false;
        if (!h.ever_known || now - h.last_known >= engine.config.leave_hold_nano)
        {
            h.state = UNKNOWN;
            h.latched = {};
        }
        return;
    }
    h.ever_known = true;
    h.last_known = now;
    if (h.state == UNKNOWN && raw.grade == OK)
    {
        h.state = OK;
        h.up_pending = false;
        h.down_pending = false;
    }
    else if (raw.grade > h.state)
    {
        h.down_pending = false;
        if (!h.up_pending)
        {
            h.up_pending = true;
            h.up_since = now;
            h.up_level = raw.grade;
        }
        else
        {
            h.up_level = std::min(h.up_level, raw.grade);
        }
        if (now - h.up_since >= engine.config.enter_hold_nano)
        {
            h.state = h.up_level;
            h.up_pending = false;
        }
    }
    else if (raw.grade < h.state)
    {
        h.up_pending = false;
        if (!h.down_pending)
        {
            h.down_pending = true;
            h.down_since = now;
            h.down_level = raw.grade;
        }
        else
        {
            h.down_level = std::max(h.down_level, raw.grade);
        }
        if (now - h.down_since >= engine.config.leave_hold_nano)
        {
            h.state = h.down_level;
            h.down_pending = false;
        }
    }
    else
    {
        h.up_pending = false;
        h.down_pending = false;
    }
    if (h.state <= OK)
    {
        h.latched = {};
    }
    else if (raw.grade >= h.state)
    {
        h.latched = raw.advice;
    }
}

void set_verdict(Engine &engine, const Verdict verdict, const std::uint64_t now)
{
    if (verdict != engine.verdict || engine.verdict_since == 0)
    {
        engine.verdict = verdict;
        engine.verdict_since = now;
    }
}

} // namespace

extern "C" void
robotweax_srt_telemetry_health_config_default(const robotweax_srt_telemetry_endpoint_role role,
                                              robotweax_srt_telemetry_health_config_v1 *config)
{
    if (config == nullptr)
    {
        return;
    }
    std::memset(config, 0, sizeof(*config));
    config->struct_size = sizeof(*config);
    config->abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
    config->role = role;
    config->count_window_nano = 10 * SECOND;
    config->gauge_window_nano = SECOND;
    config->enter_hold_nano = 2 * SECOND;
    config->leave_hold_nano = 10 * SECOND;
}

extern "C" void
robotweax_srt_telemetry_health_context_init(robotweax_srt_telemetry_health_context_v1 *context)
{
    if (context == nullptr)
    {
        return;
    }
    std::memset(context, 0, sizeof(*context));
    context->struct_size = sizeof(*context);
    context->abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
}

extern "C" robotweax_srt_telemetry_error
robotweax_srt_telemetry_health_engine_create(const robotweax_srt_telemetry_health_config_v1 *config,
                                             robotweax_srt_telemetry_health_engine **engine)
{
    if (engine == nullptr)
    {
        return ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT;
    }
    *engine = nullptr;
    if (config == nullptr)
    {
        return ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT;
    }
    if (config->struct_size < sizeof(*config) || config->abi_version != ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION)
    {
        return ROBOTWEAX_SRT_TELEMETRY_ABI_MISMATCH;
    }
    const auto role = static_cast<std::int64_t>(config->role);
    if (role < ROBOTWEAX_SRT_TELEMETRY_ENDPOINT_ROLE_UNKNOWN || role > ROBOTWEAX_SRT_TELEMETRY_ENDPOINT_ROLE_RECEIVER ||
        config->reserved != 0 || config->gauge_window_nano == 0 ||
        config->count_window_nano < config->gauge_window_nano || config->count_window_nano > 3600 * SECOND ||
        config->enter_hold_nano > 3600 * SECOND || config->leave_hold_nano > 3600 * SECOND)
    {
        return ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT;
    }
    auto *created = new (std::nothrow) robotweax_srt_telemetry_health_engine{};
    if (created == nullptr)
    {
        return ROBOTWEAX_SRT_TELEMETRY_ALLOCATION_FAILURE;
    }
    created->config = *config;
    created->config.struct_size = sizeof(created->config);
    robotweax_srt_telemetry_health_context_init(&created->context);
    for (std::size_t i = 0; i < INDICATORS; ++i)
    {
        created->role_applies[i] = default_thresholds(config->role, i, created->thresholds[i]);
    }
    *engine = created;
    return ROBOTWEAX_SRT_TELEMETRY_OK;
}

extern "C" void
robotweax_srt_telemetry_health_engine_destroy(robotweax_srt_telemetry_health_engine *engine)
{
    delete engine;
}

extern "C" robotweax_srt_telemetry_error
robotweax_srt_telemetry_health_engine_set_context(robotweax_srt_telemetry_health_engine *engine,
                                                  const robotweax_srt_telemetry_health_context_v1 *context)
{
    if (engine == nullptr || context == nullptr)
    {
        return ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT;
    }
    if (context->struct_size < sizeof(*context) || context->abi_version != ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION)
    {
        return ROBOTWEAX_SRT_TELEMETRY_ABI_MISMATCH;
    }
    const auto connection = static_cast<std::int64_t>(context->connection);
    if (connection < ROBOTWEAX_SRT_TELEMETRY_CONNECTION_UNREPORTED ||
        connection > ROBOTWEAX_SRT_TELEMETRY_CONNECTION_LOST ||
        (context->flags & ~ROBOTWEAX_SRT_TELEMETRY_HEALTH_CONTEXT_BANDWIDTH_SETTINGS) != 0)
    {
        return ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT;
    }
    engine->context = *context;
    engine->context.struct_size = sizeof(engine->context);
    return ROBOTWEAX_SRT_TELEMETRY_OK;
}

extern "C" robotweax_srt_telemetry_error robotweax_srt_telemetry_health_engine_set_thresholds(
    robotweax_srt_telemetry_health_engine *engine, const robotweax_srt_telemetry_health_indicator indicator,
    const double watch, const double at_risk, const double failing)
{
    const auto index = static_cast<std::int64_t>(indicator);
    if (engine == nullptr || index < 0 || index >= static_cast<std::int64_t>(INDICATORS))
    {
        return ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT;
    }
    const Thresholds t{watch, at_risk, failing};
    if (!ordered(static_cast<std::size_t>(index), t))
    {
        return ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT;
    }
    engine->thresholds[static_cast<std::size_t>(index)] = t;
    return ROBOTWEAX_SRT_TELEMETRY_OK;
}

extern "C" robotweax_srt_telemetry_error robotweax_srt_telemetry_health_default_thresholds(
    const robotweax_srt_telemetry_endpoint_role role, const robotweax_srt_telemetry_health_indicator indicator,
    double *watch, double *at_risk, double *failing)
{
    const auto index = static_cast<std::int64_t>(indicator);
    if (watch == nullptr || at_risk == nullptr || failing == nullptr || index < 0 ||
        index >= static_cast<std::int64_t>(INDICATORS))
    {
        return ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT;
    }
    Thresholds t;
    if (!default_thresholds(role, static_cast<std::size_t>(index), t))
    {
        return ROBOTWEAX_SRT_TELEMETRY_NOT_FOUND;
    }
    *watch = t.watch;
    *at_risk = t.at_risk;
    *failing = t.failing;
    return ROBOTWEAX_SRT_TELEMETRY_OK;
}

extern "C" robotweax_srt_telemetry_error
robotweax_srt_telemetry_health_engine_push(robotweax_srt_telemetry_health_engine *engine,
                                           const robotweax_srt_telemetry_metric_sample_v1 *sample,
                                           robotweax_srt_telemetry_health_status_v1 *status)
{
    if (engine == nullptr || sample == nullptr || status == nullptr)
    {
        return ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT;
    }
    if (sample->struct_size < sizeof(*sample) || sample->abi_version != ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION)
    {
        return ROBOTWEAX_SRT_TELEMETRY_ABI_MISMATCH;
    }
    const auto now = sample->sample_monotonic_nano;
    if (engine->has_last && now < engine->last_monotonic)
    {
        return ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT;
    }
    engine->has_last = true;
    engine->last_monotonic = now;

    std::memset(status, 0, sizeof(*status));
    status->struct_size = sizeof(*status);
    status->abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
    status->observed_at_unix_nano = sample->observed_at_unix_nano;
    status->sample_monotonic_nano = now;
    status->primary_indicator = -1;

    const bool source_error = (sample->quality_flags & ROBOTWEAX_SRT_TELEMETRY_METRIC_SAMPLE_SOURCE_ERROR) != 0;
    const auto role = engine->config.role;
    const bool receiver = role == ROBOTWEAX_SRT_TELEMETRY_ENDPOINT_ROLE_RECEIVER;

    std::array<Raw, INDICATORS> raws{};
    if (!source_error)
    {
        Entry entry;
        entry.monotonic = now;
        entry.interval = sample->interval_nano;
        for (std::size_t c = 0; c < COUNTS; ++c)
        {
            const auto metric = count_metric[c];
            const auto slot = static_cast<std::size_t>(metric);
            if (slot >= ROBOTWEAX_SRT_TELEMETRY_METRIC_SLOTS_V1)
            {
                continue;
            }
            const auto v = sample->rate_numerators[slot];
            if ((sample->available_metrics & bit(metric)) != 0 && std::isfinite(v))
            {
                entry.available |= bit(metric);
                entry.counts[c] = v;
            }
        }
        for (std::size_t g = 0; g < GAUGES; ++g)
        {
            const auto metric = gauge_metric[g];
            const auto slot = static_cast<std::size_t>(metric);
            if (slot >= ROBOTWEAX_SRT_TELEMETRY_METRIC_SLOTS_V1)
            {
                continue;
            }
            const auto v = sample->values[slot];
            if ((sample->available_metrics & bit(metric)) != 0 && std::isfinite(v))
            {
                entry.available |= bit(metric);
                entry.gauges[g] = v;
            }
        }
        if ((entry.available & bit(gauge_metric[G_HEADROOM])) != 0)
        {
            /* A gap of a second restarts the warm-up (new stream). */
            const auto restart_gap = std::max<std::uint64_t>(2 * SECOND, 3 * entry.interval);
            if (!engine->headroom_seen || now - engine->headroom_last > restart_gap)
            {
                engine->headroom_first = now;
            }
            engine->headroom_seen = true;
            engine->headroom_last = now;
        }
        /* Libsrt grows the flow window by the packets the sender drops, so
         * samples with sender drops do not raise the reference. */
        const bool sender_dropped = (entry.available & bit(count_metric[C_SENDER_DROP])) != 0 &&
                                    entry.counts[C_SENDER_DROP] > 0;
        if ((entry.available & bit(gauge_metric[G_FLOW_WINDOW])) != 0 && !sender_dropped)
        {
            engine->max_flow_window = std::max(engine->max_flow_window, entry.gauges[G_FLOW_WINDOW]);
        }
        const auto loss_count = receiver ? C_RECEIVER_LOSS : C_SENDER_LOSS;
        const bool lossy = (entry.available & bit(count_metric[loss_count])) != 0 && entry.counts[loss_count] > 0;
        if ((entry.available & bit(gauge_metric[G_RTT])) != 0 && entry.gauges[G_RTT] > 0 && !lossy)
        {
            const auto rtt = entry.gauges[G_RTT];
            constexpr std::uint64_t bucket_length = 10 * SECOND;
            constexpr std::uint64_t horizon = 300 * SECOND;
            if (engine->has_rtt_bucket && now - engine->rtt_bucket.start >= bucket_length)
            {
                engine->rtt_buckets.push_back(engine->rtt_bucket);
                engine->has_rtt_bucket = false;
            }
            if (!engine->has_rtt_bucket)
            {
                engine->rtt_bucket = {now, rtt};
                engine->has_rtt_bucket = true;
            }
            engine->rtt_bucket.min = std::min(engine->rtt_bucket.min, rtt);
            while (!engine->rtt_buckets.empty() && engine->rtt_buckets.front().start + horizon < now)
            {
                engine->rtt_buckets.pop_front();
            }
            double baseline = engine->rtt_bucket.min;
            for (const auto &bucket : engine->rtt_buckets)
            {
                baseline = std::min(baseline, bucket.min);
            }
            engine->baseline_rtt = baseline;
            engine->has_baseline_rtt = true;
        }
        add_entry(*engine, entry);
        engine->has_good = true;
        engine->last_good = now;

        Evaluation e{*engine, gauge_view(*engine, now), receiver, false, 0.0, 0.0};
        const auto latency_gauge = receiver ? G_RECEIVER_LATENCY : G_SENDER_LATENCY;
        if (e.gauges.available[latency_gauge] && e.gauges.available[G_RTT])
        {
            e.has_latency = true;
            e.latency = e.gauges.last[latency_gauge];
            e.rtt = e.gauges.max[G_RTT];
        }
        if (e.gauges.available[G_RTT] && engine->has_baseline_rtt &&
            e.gauges.max[G_RTT] > 2.0 * engine->baseline_rtt + 0.01 && engine->covered_nano > 0)
        {
            const double seconds = static_cast<double>(engine->covered_nano) / 1e9;
            if (receiver)
            {
                const auto unique = engine->sums[C_UNIQUE_RECEIVED_PACKETS];
                const auto loss = engine->sums[C_RECEIVER_LOSS];
                if (sustained_loss(*engine, C_RECEIVER_LOSS) && unique + loss > 0 && loss / (unique + loss) > 0.05)
                {
                    e.congested = true;
                    e.congestion_rate = std::max(0.0, engine->sums[C_UNIQUE_RECEIVED_BITS]) / seconds;
                }
            }
            else
            {
                const auto unique = engine->sums[C_UNIQUE_SENT_PACKETS];
                const auto loss = engine->sums[C_SENDER_LOSS];
                if (sustained_loss(*engine, C_SENDER_LOSS) && unique > 0 && loss / unique > 0.05)
                {
                    e.congested = true;
                    e.congestion_rate = std::max(0.0, engine->sums[C_UNIQUE_SENT_BITS]) / seconds *
                                        std::clamp(1.0 - loss / unique, 0.1, 1.0);
                }
            }
        }
        if (!e.congested)
        {
            const auto delivered = queue_filling(*engine, receiver, now);
            if (delivered >= 0)
            {
                /* No loss yet, so the delivered rate is known only at the
                 * receiver (what arrives) or from the capacity estimate. A
                 * sender without one advises a lower bitrate, no value. */
                e.congested = true;
                const bool estimate = e.gauges.available[G_LINK_CAPACITY] && e.gauges.samples[G_LINK_CAPACITY] > 0;
                const double capacity =
                    estimate ? e.gauges.sum[G_LINK_CAPACITY] / static_cast<double>(e.gauges.samples[G_LINK_CAPACITY])
                             : 0.0;
                e.congestion_rate = receiver ? (capacity > 0 ? std::min(delivered, capacity) : delivered) : capacity;
            }
        }
        if (e.congested)
        {
            engine->congestion_seen = true;
            engine->congestion_last = now;
            engine->congestion_rate = e.congestion_rate;
        }
        else if (engine->congestion_seen &&
                 now - engine->congestion_last <=
                     std::min(aftermath_nano(e.has_latency ? e.latency : 0.0, sample->interval_nano),
                              engine->config.count_window_nano +
                                  static_cast<std::uint64_t>(
                                      std::max(e.has_latency ? std::clamp(e.latency, 0.0, 3600.0) : 0.0, 1.0) * 1e9)))
        {
            e.congested = true;
            e.congestion_rate = engine->congestion_rate;
        }
        for (std::size_t i = 0; i < INDICATORS; ++i)
        {
            if (engine->role_applies[i])
            {
                raws[i] = compute(e, i);
            }
        }
        if (!engine->entries.empty())
        {
            const auto &latest = engine->entries.back();
            const auto drop_count = receiver ? C_RECEIVER_DROP : C_SENDER_DROP;
            const bool new_drops = (latest.available & bit(count_metric[drop_count])) != 0 &&
                                   latest.counts[drop_count] > 0;
            const bool counted = engine->sums[drop_count] > 0;
            const auto &backlog = raws[BACKLOG];
            /* The sender's own diagnosis says data waits without
             * acknowledgements (with or without drops yet): an outage. */
            /* A stall is a filling send buffer (its gauge, not only drops
             * counted in the window). It starts drop-free (the buffer fills
             * for up to the drop threshold before anything is dropped) and
             * continues
             * through the drops while the diagnosis holds. Drops with no
             * cause from the start are not a stall (e.g. the backlog a
             * removed MAXBW left behind). */
            const bool continuing =
                engine->ack_stall_seen && now - engine->ack_stall_last <= 2 * latest.interval + SECOND;
            /* A buffer still full from drops a moment ago is draining, not
             * stalling: a stall starts after a gauge window without drops. */
            const bool drop_free = engine->last_sender_drop == 0 ||
                                   now - engine->last_sender_drop > engine->config.gauge_window_nano + latest.interval;
            if (!receiver && backlog.applicable && backlog.available &&
                grade(BACKLOG, engine->thresholds[BACKLOG], backlog.value) > OK &&
                backlog.advice.reason == ROBOTWEAX_SRT_TELEMETRY_REASON_ACKNOWLEDGEMENTS_STALLED &&
                /* Loss reports arrive: the path acknowledges (its aftermath
                 * does not renew the stall it follows). */
                !recent_loss(*engine, C_SENDER_LOSS, now) &&
                (continuing || (!new_drops && drop_free)))
            {
                engine->ack_stall_seen = true;
                engine->ack_stall_last = now;
            }
            const auto aftermath = aftermath_nano(e.has_latency ? e.latency : 0.0, latest.interval);
            if (!receiver && new_drops)
            {
                engine->last_sender_drop = now;
            }
            for (const auto i : {UNRECOVERED, EFFICIENCY, BACKLOG})
            {
                auto &memo = engine->drop_diagnosis[i];
                auto &raw = raws[i];
                if (!counted || !raw.applicable || !raw.available)
                {
                    memo.valid = false;
                    continue;
                }
                if (raw.grade <= OK)
                {
                    continue;
                }
                const bool memo_fresh = memo.valid && !memo.advice.weak &&
                                        now - memo.at <= engine->config.count_window_nano;
                if (new_drops)
                {
                    if (i == BACKLOG && engine->ack_stall_seen && now - engine->ack_stall_last <= aftermath &&
                             raw.advice.knob != ROBOTWEAX_SRT_TELEMETRY_KNOB_RECEIVING_APPLICATION &&
                             raw.advice.knob != ROBOTWEAX_SRT_TELEMETRY_KNOB_MAX_BANDWIDTH)
                    {
                        /* Drops right after an acknowledgement stall: the
                         * stale backlog and the burst of loss reports when
                         * the path returns are the outage's (but not a
                         * shrunk flow window or a MAXBW cap, which are
                         * direct evidence of their own). Bounded: only a
                         * fresh stall diagnosis renews the aftermath. */
                        raw.advice = {ROBOTWEAX_SRT_TELEMETRY_REASON_ACKNOWLEDGEMENTS_STALLED,
                                      ROBOTWEAX_SRT_TELEMETRY_KNOB_NONE, ROBOTWEAX_SRT_TELEMETRY_KNOB_DIRECTION_NONE,
                                      0.0, true};
                        memo = {true, raw.advice, now};
                    }
                    else if (!raw.advice.weak)
                    {
                        /* Direct evidence: a new diagnosis. */
                        memo = {true, raw.advice, now};
                    }
                    else if (memo_fresh)
                    {
                        /* No evidence now (the cause was removed, e.g. MAXBW
                         * raised, and the backlog it left is dropped): the
                         * last concrete diagnosis, within the count window
                         * of when it was made, still explains them. */
                        raw.advice = resolved(*engine, memo.advice, e.has_latency, e.latency);
                    }
                    else
                    {
                        memo = {true, raw.advice, now};
                    }
                }
                else if (memo.valid && (i != BACKLOG || (raw.advice.weak && !(engine->ack_stall_seen &&
                                                                              engine->ack_stall_last == now))))
                {
                    /* No new drops. The receiver's drop indicators judge
                     * only counted drops: they keep the diagnosis they had.
                     * The sender's backlog diagnosis rests on live gauges
                     * (flow window, rate, RTT): a concrete one is current,
                     * and so is a stall that starts now (an outage after
                     * earlier drops); only a weak one otherwise gives way
                     * to the drops' diagnosis. */
                    raw.advice = resolved(*engine, memo.advice, e.has_latency, e.latency);
                }
            }
        }
        for (std::size_t i = 0; i < INDICATORS; ++i)
        {
            if (raws[i].applicable)
            {
                update_hysteresis(*engine, engine->hysteresis[i], raws[i], now);
            }
            else
            {
                /* e.g. MAXBW became unlimited: no stale state comes back later. */
                engine->hysteresis[i] = {};
            }
        }
    }

    /* Indicators. */
    for (std::size_t i = 0; i < INDICATORS; ++i)
    {
        auto &out = status->indicators[i];
        const auto &h = engine->hysteresis[i];
        const auto &raw = raws[i];
        const bool applicable = source_error ? engine->role_applies[i] : raw.applicable;
        if (!applicable)
        {
            continue;
        }
        out.flags = ROBOTWEAX_SRT_TELEMETRY_INDICATOR_APPLICABLE;
        if ((receiver && i == UNRECOVERED) ||
            (role == ROBOTWEAX_SRT_TELEMETRY_ENDPOINT_ROLE_SENDER && i == BACKLOG))
        {
            out.flags |= ROBOTWEAX_SRT_TELEMETRY_INDICATOR_REQUIRED;
        }
        out.state = h.state;
        if (raw.available)
        {
            out.flags |= ROBOTWEAX_SRT_TELEMETRY_INDICATOR_AVAILABLE;
            out.value = raw.value;
            out.raw_state = raw.grade;
        }
        out.reason = h.state > OK ? h.latched.reason : (raw.available ? ROBOTWEAX_SRT_TELEMETRY_REASON_NONE
                                                                       : raw.advice.reason);
        if (source_error)
        {
            out.reason = ROBOTWEAX_SRT_TELEMETRY_REASON_SOURCE_ERROR;
        }
        status->missing_metrics |= raw.missing;
    }

    /* Verdict. */
    const auto finish = [&](const Verdict verdict, const Reason reason)
    {
        set_verdict(*engine, verdict, now);
        status->verdict = verdict;
        status->reason = reason;
        status->verdict_since_monotonic_nano = engine->verdict_since;
        return ROBOTWEAX_SRT_TELEMETRY_OK;
    };
    if (role == ROBOTWEAX_SRT_TELEMETRY_ENDPOINT_ROLE_UNKNOWN)
    {
        return finish(UNKNOWN, ROBOTWEAX_SRT_TELEMETRY_REASON_ROLE_UNKNOWN);
    }
    if (engine->context.connection == ROBOTWEAX_SRT_TELEMETRY_CONNECTION_LOST)
    {
        return finish(FAILING, ROBOTWEAX_SRT_TELEMETRY_REASON_CONNECTION_LOST);
    }
    if (engine->context.connection == ROBOTWEAX_SRT_TELEMETRY_CONNECTION_CONNECTING)
    {
        return finish(UNKNOWN, ROBOTWEAX_SRT_TELEMETRY_REASON_CONNECTING);
    }
    if (source_error)
    {
        /* A failed read or two keeps the verdict; after enter_hold without
         * a good sample it is UNKNOWN. */
        if (engine->has_good && now - engine->last_good < engine->config.enter_hold_nano &&
            engine->verdict != UNKNOWN)
        {
            status->primary_indicator = -1;
            return finish(engine->verdict, ROBOTWEAX_SRT_TELEMETRY_REASON_SOURCE_ERROR);
        }
        return finish(UNKNOWN, ROBOTWEAX_SRT_TELEMETRY_REASON_SOURCE_ERROR);
    }

    Verdict worst = UNKNOWN;
    std::size_t primary = INDICATORS;
    for (const auto i : priority)
    {
        if (raws[i].applicable && engine->hysteresis[i].state > worst)
        {
            worst = engine->hysteresis[i].state;
            primary = i;
        }
    }
    if (worst > OK)
    {
        const auto &advice = engine->hysteresis[primary].latched;
        status->primary_indicator = static_cast<std::int32_t>(primary);
        status->knob = advice.knob;
        status->knob_direction = advice.direction;
        status->suggested_value = advice.value;
        return finish(worst, advice.reason);
    }
    /* Never a wrong OK: a required indicator not yet graded, or without a
     * grade for enter_hold (stream stopped), means UNKNOWN. A shorter gap,
     * such as a trickle stream between packets, keeps the verdict. */
    for (std::size_t i = 0; i < INDICATORS; ++i)
    {
        const auto &out = status->indicators[i];
        if ((out.flags & ROBOTWEAX_SRT_TELEMETRY_INDICATOR_REQUIRED) == 0)
        {
            continue;
        }
        if (raws[i].available)
        {
            if (engine->required_missing || !engine->required_seen)
            {
                engine->required_available_since = now;
            }
            engine->required_missing = false;
            engine->required_seen = true;
        }
        else if (!engine->required_missing)
        {
            engine->required_missing = true;
            engine->required_missing_since = now;
        }
        const bool missing_too_long =
            engine->required_missing && now - engine->required_missing_since >= engine->config.enter_hold_nano;
        /* After a stream start or a gap, the counts may not yet show what
         * the gap cost (drops arrive with the first packets after it). */
        bool settling =
            !engine->required_missing && now - engine->required_available_since < engine->config.enter_hold_nano;
        /* An escalation that began while settling finishes first. */
        for (std::size_t j = 0; j < INDICATORS && !settling; ++j)
        {
            const auto &h = engine->hysteresis[j];
            settling = raws[j].applicable && h.up_pending &&
                       h.up_since < engine->required_available_since + engine->config.enter_hold_nano;
        }
        if (out.state == UNKNOWN || missing_too_long || settling)
        {
            status->primary_indicator = static_cast<std::int32_t>(i);
            return finish(UNKNOWN, raws[i].available ? ROBOTWEAX_SRT_TELEMETRY_REASON_NONE : raws[i].advice.reason);
        }
    }
    return finish(worst, ROBOTWEAX_SRT_TELEMETRY_REASON_NONE);
}

extern "C" const char *
robotweax_srt_telemetry_health_verdict_name(const robotweax_srt_telemetry_health_verdict verdict)
{
    switch (verdict)
    {
    case ROBOTWEAX_SRT_TELEMETRY_HEALTH_UNKNOWN:
        return "unknown";
    case ROBOTWEAX_SRT_TELEMETRY_HEALTH_OK:
        return "ok";
    case ROBOTWEAX_SRT_TELEMETRY_HEALTH_WATCH:
        return "watch";
    case ROBOTWEAX_SRT_TELEMETRY_HEALTH_AT_RISK:
        return "at_risk";
    case ROBOTWEAX_SRT_TELEMETRY_HEALTH_FAILING:
        return "failing";
    }
    return "invalid";
}

extern "C" const char *
robotweax_srt_telemetry_health_indicator_name(const robotweax_srt_telemetry_health_indicator indicator)
{
    switch (indicator)
    {
    case ROBOTWEAX_SRT_TELEMETRY_INDICATOR_RECOVERY_HEADROOM:
        return "recovery_headroom";
    case ROBOTWEAX_SRT_TELEMETRY_INDICATOR_LATENCY_SUFFICIENCY:
        return "latency_sufficiency";
    case ROBOTWEAX_SRT_TELEMETRY_INDICATOR_UNRECOVERED_RATIO:
        return "unrecovered_ratio";
    case ROBOTWEAX_SRT_TELEMETRY_INDICATOR_RECOVERY_EFFICIENCY:
        return "recovery_efficiency";
    case ROBOTWEAX_SRT_TELEMETRY_INDICATOR_OVERHEAD_BUDGET:
        return "overhead_budget";
    case ROBOTWEAX_SRT_TELEMETRY_INDICATOR_PATH_HEADROOM:
        return "path_headroom";
    case ROBOTWEAX_SRT_TELEMETRY_INDICATOR_SENDER_BACKLOG:
        return "sender_backlog";
    case ROBOTWEAX_SRT_TELEMETRY_INDICATOR_PEER_READ_STALL:
        return "peer_read_stall";
    case ROBOTWEAX_SRT_TELEMETRY_INDICATOR_COUNT:
        break;
    }
    return "invalid";
}

extern "C" const char *
robotweax_srt_telemetry_health_reason_name(const robotweax_srt_telemetry_health_reason reason)
{
    switch (reason)
    {
    case ROBOTWEAX_SRT_TELEMETRY_REASON_NONE:
        return "none";
    case ROBOTWEAX_SRT_TELEMETRY_REASON_NO_DATA:
        return "no_data";
    case ROBOTWEAX_SRT_TELEMETRY_REASON_MISSING_METRIC:
        return "missing_metric";
    case ROBOTWEAX_SRT_TELEMETRY_REASON_MISSING_SETTING:
        return "missing_setting";
    case ROBOTWEAX_SRT_TELEMETRY_REASON_SOURCE_ERROR:
        return "source_error";
    case ROBOTWEAX_SRT_TELEMETRY_REASON_CONNECTING:
        return "connecting";
    case ROBOTWEAX_SRT_TELEMETRY_REASON_CONNECTION_LOST:
        return "connection_lost";
    case ROBOTWEAX_SRT_TELEMETRY_REASON_ROLE_UNKNOWN:
        return "role_unknown";
    case ROBOTWEAX_SRT_TELEMETRY_REASON_RECOVERY_HEADROOM_LOW:
        return "recovery_headroom_low";
    case ROBOTWEAX_SRT_TELEMETRY_REASON_LATENCY_LOW_FOR_RTT:
        return "latency_low_for_rtt";
    case ROBOTWEAX_SRT_TELEMETRY_REASON_UNRECOVERED_LOSS:
        return "unrecovered_loss";
    case ROBOTWEAX_SRT_TELEMETRY_REASON_UNDECRYPTABLE:
        return "undecryptable";
    case ROBOTWEAX_SRT_TELEMETRY_REASON_LATE_REPAIRS:
        return "late_repairs";
    case ROBOTWEAX_SRT_TELEMETRY_REASON_RECOVERY_INEFFICIENT:
        return "recovery_inefficient";
    case ROBOTWEAX_SRT_TELEMETRY_REASON_OVERHEAD_BUDGET_HIGH:
        return "overhead_budget_high";
    case ROBOTWEAX_SRT_TELEMETRY_REASON_PATH_CAPACITY_EXCEEDED:
        return "path_capacity_exceeded";
    case ROBOTWEAX_SRT_TELEMETRY_REASON_SENDER_BACKLOG:
        return "sender_backlog";
    case ROBOTWEAX_SRT_TELEMETRY_REASON_SENDER_DROPS:
        return "sender_drops";
    case ROBOTWEAX_SRT_TELEMETRY_REASON_PEER_READ_STALL:
        return "peer_read_stall";
    case ROBOTWEAX_SRT_TELEMETRY_REASON_APPLICATION_READ_STALL:
        return "application_read_stall";
    case ROBOTWEAX_SRT_TELEMETRY_REASON_ACKNOWLEDGEMENTS_STALLED:
        return "acknowledgements_stalled";
    case ROBOTWEAX_SRT_TELEMETRY_REASON_PATH_CONGESTED:
        return "path_congested";
    }
    return "invalid";
}

extern "C" const char *robotweax_srt_telemetry_health_knob_name(const robotweax_srt_telemetry_health_knob knob)
{
    switch (knob)
    {
    case ROBOTWEAX_SRT_TELEMETRY_KNOB_NONE:
        return "none";
    case ROBOTWEAX_SRT_TELEMETRY_KNOB_LATENCY:
        return "latency";
    case ROBOTWEAX_SRT_TELEMETRY_KNOB_BITRATE:
        return "bitrate";
    case ROBOTWEAX_SRT_TELEMETRY_KNOB_OVERHEAD_BANDWIDTH:
        return "overhead_bandwidth";
    case ROBOTWEAX_SRT_TELEMETRY_KNOB_MAX_BANDWIDTH:
        return "max_bandwidth";
    case ROBOTWEAX_SRT_TELEMETRY_KNOB_PASSPHRASE:
        return "passphrase";
    case ROBOTWEAX_SRT_TELEMETRY_KNOB_RECEIVING_APPLICATION:
        return "receiving_application";
    }
    return "invalid";
}
