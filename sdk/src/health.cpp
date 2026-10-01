#include <robotweax/srt_telemetry_health.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <limits>
#include <new>

static_assert(sizeof(robotweax_srt_telemetry_health_indicator_status_v1) == 24,
              "indicator status layout is part of the ABI");
static_assert(sizeof(robotweax_srt_telemetry_health_status_v1) == 72 + 16 * 24,
              "health status layout is part of the ABI");

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
        out = {0.75, 0.9, 1.05};
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
        out = {0.6, 0.85, 0.98};
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
    std::uint64_t last_monotonic = 0;
    bool has_last = false;

    /* Recovery headroom is low for about one latency period after data
     * starts; it is not graded until then. */
    bool headroom_seen = false;
    std::uint64_t headroom_first = 0;
    std::uint64_t headroom_last = 0;

    Verdict verdict = UNKNOWN;
    std::uint64_t verdict_since = 0;
};

namespace
{

using Engine = robotweax_srt_telemetry_health_engine;

void add_entry(Engine &engine, const Entry &entry)
{
    engine.entries.push_back(entry);
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
            }
            view.sent_interval += static_cast<double>(it->interval) / 1e9;
        }
    }
    return view;
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
};

Advice latency_advice(const Evaluation &e, const Reason reason, const double loss_ratio)
{
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
Advice drop_advice(const Evaluation &e, const Reason fallback)
{
    const auto &engine = e.engine;
    const auto loss_ratio = receiver_loss_ratio(engine);
    if (sum(engine, C_BELATED) > 0)
    {
        return latency_advice(e, ROBOTWEAX_SRT_TELEMETRY_REASON_LATE_REPAIRS, loss_ratio);
    }
    if (e.has_latency && e.rtt > 0 && e.latency / e.rtt >= 8.0 && loss_ratio > 0.05)
    {
        /* Plenty of retransmission rounds and still losing: the path cannot
         * carry the stream plus its repairs. */
        const double seconds = static_cast<double>(e.engine.config.count_window_nano) / 1e9;
        return bitrate_decrease(fallback, 0.8 * sum(engine, C_UNIQUE_RECEIVED_BITS) / seconds);
    }
    return latency_advice(e, fallback, loss_ratio);
}

Raw compute(const Evaluation &e, const std::size_t indicator)
{
    const auto &engine = e.engine;
    const auto &g = e.gauges;
    const auto count_seconds = static_cast<double>(engine.config.count_window_nano) / 1e9;
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
            e.has_latency ? static_cast<std::uint64_t>(1.5 * e.latency * 1e9) : SECOND);
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
        if (unique <= 0)
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
                      round_up(1.25 * (unique + retransmitted) / count_seconds, 1000.0)};
        }
        else
        {
            const auto percent = static_cast<double>(std::max<std::int64_t>(context.overhead_percent, 0));
            allowed = unique * percent / 100.0;
            const auto needed = round_up(1.5 * 100.0 * retransmitted / unique, 5.0);
            if (needed <= 100.0)
            {
                advice = {ROBOTWEAX_SRT_TELEMETRY_REASON_OVERHEAD_BUDGET_HIGH,
                          ROBOTWEAX_SRT_TELEMETRY_KNOB_OVERHEAD_BANDWIDTH,
                          ROBOTWEAX_SRT_TELEMETRY_KNOB_DIRECTION_INCREASE, std::max(needed, percent + 5.0)};
            }
            else
            {
                advice = bitrate_decrease(ROBOTWEAX_SRT_TELEMETRY_REASON_OVERHEAD_BUDGET_HIGH,
                                          0.8 * unique / count_seconds);
            }
        }
        const auto value = allowed > 0 ? retransmitted / allowed
                                       : (retransmitted > 0 ? std::numeric_limits<double>::infinity() : 0.0);
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
        return graded(e, indicator, rate / capacity,
                      bitrate_decrease(ROBOTWEAX_SRT_TELEMETRY_REASON_PATH_CAPACITY_EXCEEDED, 0.7 * capacity));
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
        /* Advice: a path near capacity needs a lower bitrate; short latency
         * for the RTT needs more latency; otherwise lower the bitrate. */
        Advice advice;
        const auto reason = drops > 0 ? ROBOTWEAX_SRT_TELEMETRY_REASON_SENDER_DROPS
                                      : ROBOTWEAX_SRT_TELEMETRY_REASON_SENDER_BACKLOG;
        const bool short_latency = e.has_latency && e.rtt > 0 &&
                                   e.latency / e.rtt < engine.thresholds[LATENCY].watch;
        const double capacity = g.available[G_LINK_CAPACITY]
                                    ? g.sum[G_LINK_CAPACITY] / static_cast<double>(g.samples[G_LINK_CAPACITY])
                                    : 0.0;
        const double rate = g.sent_interval > 0 ? g.sent_bits / g.sent_interval : 0.0;
        if (capacity > 0 && rate / capacity > engine.thresholds[PATH].watch)
        {
            advice = bitrate_decrease(reason, 0.7 * capacity);
        }
        else if (short_latency)
        {
            advice = latency_advice(e, reason, 0.0);
        }
        else
        {
            advice = bitrate_decrease(reason, 0.8 * sum(engine, C_UNIQUE_SENT_BITS) / count_seconds);
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
        if (!g.available[G_FLIGHT_TO_FLOW])
        {
            return unavailable(ROBOTWEAX_SRT_TELEMETRY_REASON_MISSING_METRIC, bit(gauge_metric[G_FLIGHT_TO_FLOW]));
        }
        return graded(e, indicator, g.max[G_FLIGHT_TO_FLOW],
                      {ROBOTWEAX_SRT_TELEMETRY_REASON_PEER_READ_STALL,
                       ROBOTWEAX_SRT_TELEMETRY_KNOB_RECEIVING_APPLICATION,
                       ROBOTWEAX_SRT_TELEMETRY_KNOB_DIRECTION_CHECK, 0.0});
    }
    default:
        return {};
    }
}

void update_hysteresis(const Engine &engine, Hysteresis &h, const Raw &raw, const std::uint64_t now)
{
    if (!raw.available)
    {
        /* No evidence: hold the state, and give it up after leave_hold. */
        h.up_pending = false;
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
            const auto v = sample->rate_numerators[metric];
            if ((sample->available_metrics & bit(metric)) != 0 && std::isfinite(v))
            {
                entry.available |= bit(metric);
                entry.counts[c] = v;
            }
        }
        for (std::size_t g = 0; g < GAUGES; ++g)
        {
            const auto metric = gauge_metric[g];
            const auto v = sample->values[metric];
            if ((sample->available_metrics & bit(metric)) != 0 && std::isfinite(v))
            {
                entry.available |= bit(metric);
                entry.gauges[g] = v;
            }
        }
        if ((entry.available & bit(gauge_metric[G_HEADROOM])) != 0)
        {
            /* A gap of a second restarts the warm-up (new stream). */
            if (!engine->headroom_seen || now - engine->headroom_last > SECOND)
            {
                engine->headroom_first = now;
            }
            engine->headroom_seen = true;
            engine->headroom_last = now;
        }
        add_entry(*engine, entry);

        Evaluation e{*engine, gauge_view(*engine, now), receiver, false, 0.0, 0.0};
        const auto latency_gauge = receiver ? G_RECEIVER_LATENCY : G_SENDER_LATENCY;
        if (e.gauges.available[latency_gauge] && e.gauges.available[G_RTT])
        {
            e.has_latency = true;
            e.latency = e.gauges.last[latency_gauge];
            e.rtt = e.gauges.max[G_RTT];
        }
        for (std::size_t i = 0; i < INDICATORS; ++i)
        {
            if (engine->role_applies[i])
            {
                raws[i] = compute(e, i);
            }
        }
        for (std::size_t i = 0; i < INDICATORS; ++i)
        {
            if (raws[i].applicable)
            {
                update_hysteresis(*engine, engine->hysteresis[i], raws[i], now);
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
    /* Never a wrong OK: a required indicator without a grade now (or not
     * yet out of UNKNOWN) means UNKNOWN. */
    for (std::size_t i = 0; i < INDICATORS; ++i)
    {
        const auto &out = status->indicators[i];
        if ((out.flags & ROBOTWEAX_SRT_TELEMETRY_INDICATOR_REQUIRED) != 0 &&
            (out.state == UNKNOWN || !raws[i].available))
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
