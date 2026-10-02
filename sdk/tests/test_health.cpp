#include <robotweax/srt_telemetry_health.h>
#include <robotweax/srt_telemetry_sampler.h>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace
{

int failures = 0;

void check(const bool condition, const std::string_view message)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

constexpr std::uint64_t second = UINT64_C(1'000'000'000);
constexpr std::uint64_t period = second / 10;
constexpr double packet_bits = 1316.0 * 8.0;

using Verdict = robotweax_srt_telemetry_health_verdict;

/* One synthetic stream condition; per-second rates and gauges. */
struct Condition
{
    double packets_per_second = 300;
    double rtt = 0.03;
    double latency = 0.12;
    /* receiver */
    double loss_per_second = 0;
    double drops_per_second = 0;
    double undecryptable_per_second = 0;
    double belated_per_second = 0;
    double headroom = 1.0;
    bool receiving = true;
    /* sender */
    double retransmitted_per_second = 0;
    double sender_drops_per_second = 0;
    double send_buffer_ratio = 0.1;
    double flight_to_flow = 0.05;
    double flow_window = 8192;
    double capacity_bps = 0; /* 0: provider gives no estimate */
    bool sending = true;
};

void set(robotweax_srt_telemetry_metric_sample_v1 &s, const robotweax_srt_telemetry_metric metric,
         const double value, const double numerator = 0)
{
    s.available_metrics |= ROBOTWEAX_SRT_TELEMETRY_METRIC_BIT(metric);
    s.values[metric] = value;
    s.rate_numerators[metric] = numerator;
}

robotweax_srt_telemetry_metric_sample_v1 sample(const robotweax_srt_telemetry_endpoint_role role,
                                                const std::uint64_t now, const Condition &c)
{
    robotweax_srt_telemetry_metric_sample_v1 s{};
    s.struct_size = sizeof(s);
    s.abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
    s.observed_at_unix_nano = UINT64_C(1'800'000'000) * second + now;
    s.sample_monotonic_nano = now;
    s.interval_nano = period;
    const double dt = 0.1;
    const auto rate = [&](const robotweax_srt_telemetry_metric metric, const double per_second)
    { set(s, metric, per_second, per_second * dt); };
    set(s, ROBOTWEAX_SRT_TELEMETRY_METRIC_RTT_SECONDS, c.rtt);
    if (role == ROBOTWEAX_SRT_TELEMETRY_ENDPOINT_ROLE_RECEIVER)
    {
        const double unique = c.receiving ? c.packets_per_second : 0.0;
        rate(ROBOTWEAX_SRT_TELEMETRY_METRIC_UNIQUE_RECEIVED_PACKETS_PER_SECOND, unique);
        rate(ROBOTWEAX_SRT_TELEMETRY_METRIC_UNIQUE_RECEIVED_BITS_PER_SECOND, unique * packet_bits);
        rate(ROBOTWEAX_SRT_TELEMETRY_METRIC_RECEIVER_LOSS_PACKETS_PER_SECOND, c.loss_per_second);
        rate(ROBOTWEAX_SRT_TELEMETRY_METRIC_RECEIVER_DROP_PACKETS_PER_SECOND, c.drops_per_second);
        rate(ROBOTWEAX_SRT_TELEMETRY_METRIC_UNDECRYPTABLE_PACKETS_PER_SECOND, c.undecryptable_per_second);
        rate(ROBOTWEAX_SRT_TELEMETRY_METRIC_BELATED_PACKETS_PER_SECOND, c.belated_per_second);
        set(s, ROBOTWEAX_SRT_TELEMETRY_METRIC_RECEIVER_TSBPD_DELAY_SECONDS, c.latency);
        if (c.receiving)
        {
            set(s, ROBOTWEAX_SRT_TELEMETRY_METRIC_RECOVERY_HEADROOM_RATIO, c.headroom);
        }
    }
    else
    {
        const double unique = c.sending ? c.packets_per_second : 0.0;
        rate(ROBOTWEAX_SRT_TELEMETRY_METRIC_UNIQUE_SENT_PACKETS_PER_SECOND, unique);
        rate(ROBOTWEAX_SRT_TELEMETRY_METRIC_UNIQUE_SENT_BITS_PER_SECOND, unique * packet_bits);
        rate(ROBOTWEAX_SRT_TELEMETRY_METRIC_RETRANSMITTED_BITS_PER_SECOND,
             c.retransmitted_per_second * packet_bits);
        rate(ROBOTWEAX_SRT_TELEMETRY_METRIC_SENDER_DROP_PACKETS_PER_SECOND, c.sender_drops_per_second);
        set(s, ROBOTWEAX_SRT_TELEMETRY_METRIC_SENDER_TSBPD_DELAY_SECONDS, c.latency);
        set(s, ROBOTWEAX_SRT_TELEMETRY_METRIC_FLIGHT_TO_FLOW_WINDOW_RATIO, c.flight_to_flow);
        set(s, ROBOTWEAX_SRT_TELEMETRY_METRIC_FLOW_WINDOW_PACKETS, c.flow_window);
        if (c.sending)
        {
            set(s, ROBOTWEAX_SRT_TELEMETRY_METRIC_SEND_BUFFER_TO_DROP_THRESHOLD_RATIO, c.send_buffer_ratio);
        }
        if (c.capacity_bps > 0)
        {
            set(s, ROBOTWEAX_SRT_TELEMETRY_METRIC_LINK_CAPACITY_BITS_PER_SECOND, c.capacity_bps);
        }
    }
    return s;
}

struct Run
{
    robotweax_srt_telemetry_endpoint_role role;
    robotweax_srt_telemetry_health_engine *engine = nullptr;
    std::uint64_t now = second;
    robotweax_srt_telemetry_health_status_v1 status{};

    explicit Run(const robotweax_srt_telemetry_endpoint_role r) : role(r)
    {
        robotweax_srt_telemetry_health_config_v1 config;
        robotweax_srt_telemetry_health_config_default(role, &config);
        static_cast<void>(robotweax_srt_telemetry_health_engine_create(&config, &engine));
        robotweax_srt_telemetry_health_context_v1 context;
        robotweax_srt_telemetry_health_context_init(&context);
        context.connection = ROBOTWEAX_SRT_TELEMETRY_CONNECTION_CONNECTED;
        context.flags = ROBOTWEAX_SRT_TELEMETRY_HEALTH_CONTEXT_BANDWIDTH_SETTINGS;
        context.overhead_percent = 25;
        context.max_bandwidth_bytes_per_second = 0;
        static_cast<void>(robotweax_srt_telemetry_health_engine_set_context(engine, &context));
    }
    ~Run() { robotweax_srt_telemetry_health_engine_destroy(engine); }
    Run(const Run &) = delete;
    Run &operator=(const Run &) = delete;

    /* Feeds `seconds` of a condition; returns the final verdict. */
    Verdict feed(const Condition &c, const double seconds)
    {
        const auto steps = static_cast<int>(std::lround(seconds * 10));
        for (int i = 0; i < steps; ++i)
        {
            now += period;
            const auto s = sample(role, now, c);
            static_cast<void>(robotweax_srt_telemetry_health_engine_push(engine, &s, &status));
        }
        return status.verdict;
    }

    /* Seconds until the verdict first equals `target`, or -1. */
    double until(const Condition &c, const Verdict target, const double limit)
    {
        for (int i = 1; i <= static_cast<int>(limit * 10); ++i)
        {
            if (feed(c, 0.1) == target)
            {
                return i / 10.0;
            }
        }
        return -1;
    }
};

std::string describe(const robotweax_srt_telemetry_health_status_v1 &s)
{
    std::string text = robotweax_srt_telemetry_health_verdict_name(s.verdict);
    text += '/';
    text += robotweax_srt_telemetry_health_reason_name(s.reason);
    text += '/';
    text += robotweax_srt_telemetry_health_knob_name(s.knob);
    return text;
}

constexpr auto SENDER = ROBOTWEAX_SRT_TELEMETRY_ENDPOINT_ROLE_SENDER;
constexpr auto RECEIVER = ROBOTWEAX_SRT_TELEMETRY_ENDPOINT_ROLE_RECEIVER;

void test_api()
{
    robotweax_srt_telemetry_health_engine *engine = nullptr;
    robotweax_srt_telemetry_health_config_v1 config;
    robotweax_srt_telemetry_health_config_default(RECEIVER, &config);
    check(robotweax_srt_telemetry_health_engine_create(nullptr, &engine) ==
              ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT,
          "null config");
    auto bad = config;
    bad.abi_version += 1;
    check(robotweax_srt_telemetry_health_engine_create(&bad, &engine) == ROBOTWEAX_SRT_TELEMETRY_ABI_MISMATCH,
          "ABI mismatch");
    bad = config;
    bad.count_window_nano = second / 2; /* shorter than the gauge window */
    check(robotweax_srt_telemetry_health_engine_create(&bad, &engine) ==
              ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT,
          "count window shorter than gauge window");
    check(robotweax_srt_telemetry_health_engine_create(&config, &engine) == ROBOTWEAX_SRT_TELEMETRY_OK,
          "create");
    check(robotweax_srt_telemetry_health_engine_set_thresholds(
              engine, ROBOTWEAX_SRT_TELEMETRY_INDICATOR_RECOVERY_HEADROOM, 0.3, 0.5, 0.1) ==
              ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT,
          "unordered thresholds rejected (lower is worse)");
    check(robotweax_srt_telemetry_health_engine_set_thresholds(
              engine, ROBOTWEAX_SRT_TELEMETRY_INDICATOR_UNRECOVERED_RATIO, 0.0, 1e-4, 1e-3) ==
              ROBOTWEAX_SRT_TELEMETRY_OK,
          "thresholds accepted");
    check(robotweax_srt_telemetry_health_engine_set_thresholds(
              engine, static_cast<robotweax_srt_telemetry_health_indicator>(99), 0, 0, 0) ==
              ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT,
          "unknown indicator");
    robotweax_srt_telemetry_health_context_v1 context;
    robotweax_srt_telemetry_health_context_init(&context);
    context.flags = 0x80;
    check(robotweax_srt_telemetry_health_engine_set_context(engine, &context) ==
              ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT,
          "unknown context flags");
    double w = 0, r = 0, f = 0;
    check(robotweax_srt_telemetry_health_default_thresholds(
              SENDER, ROBOTWEAX_SRT_TELEMETRY_INDICATOR_RECOVERY_HEADROOM, &w, &r, &f) ==
              ROBOTWEAX_SRT_TELEMETRY_NOT_FOUND,
          "headroom does not apply to a sender");
    check(robotweax_srt_telemetry_health_default_thresholds(
              RECEIVER, ROBOTWEAX_SRT_TELEMETRY_INDICATOR_LATENCY_SUFFICIENCY, &w, &r, &f) ==
                  ROBOTWEAX_SRT_TELEMETRY_OK &&
              w == 4.0 && r == 2.5 && f == 1.5,
          "latency defaults");

    /* Out-of-order samples are rejected. */
    robotweax_srt_telemetry_health_status_v1 status{};
    const auto later = sample(RECEIVER, 5 * second, Condition{});
    const auto earlier = sample(RECEIVER, 4 * second, Condition{});
    static_cast<void>(robotweax_srt_telemetry_health_engine_push(engine, &later, &status));
    check(robotweax_srt_telemetry_health_engine_push(engine, &earlier, &status) ==
              ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT,
          "monotonic order enforced");
    robotweax_srt_telemetry_health_engine_destroy(engine);

    check(std::string_view(robotweax_srt_telemetry_health_indicator_name(
              ROBOTWEAX_SRT_TELEMETRY_INDICATOR_PEER_READ_STALL)) == "peer_read_stall" &&
              std::string_view(robotweax_srt_telemetry_health_reason_name(
                  ROBOTWEAX_SRT_TELEMETRY_REASON_LATE_REPAIRS)) == "late_repairs" &&
              std::string_view(robotweax_srt_telemetry_health_verdict_name(
                  ROBOTWEAX_SRT_TELEMETRY_HEALTH_AT_RISK)) == "at_risk",
          "names");
}

void test_clean_streams()
{
    for (const auto role : {SENDER, RECEIVER})
    {
        Run run(role);
        Condition c;
        const auto first_ok = run.until(c, ROBOTWEAX_SRT_TELEMETRY_HEALTH_OK, 5);
        /* OK needs 2 s of evidence: never OK before the counts can show
         * what the start cost. */
        check(first_ok >= 2.0 && first_ok <= 2.5, "clean stream is OK after 2 s of evidence");
        check(run.feed(c, 60) == ROBOTWEAX_SRT_TELEMETRY_HEALTH_OK && run.status.knob == 0,
              "clean stream stays OK without advice");
        check(run.status.indicators[ROBOTWEAX_SRT_TELEMETRY_INDICATOR_PATH_HEADROOM].flags == 0 ||
                  role == SENDER,
              "path headroom does not apply to a receiver");
    }
}

void test_hysteresis()
{
    Run run(RECEIVER);
    Condition clean;
    run.feed(clean, 3);
    Condition low = clean;
    low.headroom = 0.3; /* AT_RISK band */
    /* A one-second dip is not reported. */
    check(run.feed(low, 1) == ROBOTWEAX_SRT_TELEMETRY_HEALTH_OK, "1 s dip ignored");
    check(run.feed(clean, 2) == ROBOTWEAX_SRT_TELEMETRY_HEALTH_OK, "still OK after the dip");
    /* A sustained dip enters after 2 s (the gauge window adds up to 1 s). */
    const auto entered = run.until(low, ROBOTWEAX_SRT_TELEMETRY_HEALTH_AT_RISK, 10);
    check(entered >= 2.0 && entered <= 2.2, "sustained low headroom: AT_RISK after the 2 s hold");
    check(run.status.reason == ROBOTWEAX_SRT_TELEMETRY_REASON_RECOVERY_HEADROOM_LOW &&
              run.status.knob == ROBOTWEAX_SRT_TELEMETRY_KNOB_LATENCY &&
              run.status.knob_direction == ROBOTWEAX_SRT_TELEMETRY_KNOB_DIRECTION_INCREASE &&
              std::abs(run.status.suggested_value - 0.18) < 1e-9,
          "advice: raise latency to 180 ms (1.5 x 120 ms)");
    /* Recovery: stays until 10 s clean (plus the gauge window). */
    const auto left = run.until(clean, ROBOTWEAX_SRT_TELEMETRY_HEALTH_OK, 20);
    check(left >= 10.0 && left <= 11.2, "back to OK after 10 s clean");
    /* Flicker never accumulates into a state change. */
    Run flicker(RECEIVER);
    flicker.feed(clean, 3);
    bool stayed_ok = true;
    for (int i = 0; i < 30; ++i)
    {
        stayed_ok = stayed_ok && flicker.feed(low, 0.5) == ROBOTWEAX_SRT_TELEMETRY_HEALTH_OK;
        stayed_ok = stayed_ok && flicker.feed(clean, 1.5) == ROBOTWEAX_SRT_TELEMETRY_HEALTH_OK;
    }
    check(stayed_ok, "half-second dips every two seconds do not flip the verdict");
}

void test_receiver_classes()
{
    Condition clean;
    {
        /* Loss fully repaired within latency: OK. */
        Run run(RECEIVER);
        Condition c = clean;
        c.loss_per_second = 6; /* 2 % */
        c.headroom = 0.75;
        check(run.feed(c, 30) == ROBOTWEAX_SRT_TELEMETRY_HEALTH_OK, "repaired loss is OK");
    }
    {
        /* One dropped packet: WATCH... no, 1 of 3000 is above 1e-3? No: 3.3e-4
         * is in the WATCH band; it is visible and then clears. */
        Run run(RECEIVER);
        run.feed(clean, 5);
        Condition c = clean;
        c.drops_per_second = 10; /* one packet in this 100 ms sample */
        c.loss_per_second = 10;
        run.feed(c, 0.1);
        const auto entered = run.until(clean, ROBOTWEAX_SRT_TELEMETRY_HEALTH_WATCH, 5);
        check(entered > 0 && entered <= 2.1, "a single drop shows as WATCH");
        check(run.status.reason == ROBOTWEAX_SRT_TELEMETRY_REASON_UNRECOVERED_LOSS &&
                  run.status.primary_indicator == ROBOTWEAX_SRT_TELEMETRY_INDICATOR_UNRECOVERED_RATIO,
              "single drop reason");
        check(run.until(clean, ROBOTWEAX_SRT_TELEMETRY_HEALTH_OK, 30) > 0, "and clears afterwards");
    }
    {
        /* Loss beyond repair with tight latency: FAILING, raise latency. */
        Run run(RECEIVER);
        Condition c = clean;
        c.rtt = 0.06; /* latency 120 ms = 2 x RTT */
        c.loss_per_second = 30;
        c.drops_per_second = 9; /* 3 % */
        c.belated_per_second = 3;
        c.headroom = 0.4;
        check(run.feed(c, 5) == ROBOTWEAX_SRT_TELEMETRY_HEALTH_FAILING, "unrepairable loss is FAILING");
        check(run.status.reason == ROBOTWEAX_SRT_TELEMETRY_REASON_LATE_REPAIRS &&
                  run.status.knob == ROBOTWEAX_SRT_TELEMETRY_KNOB_LATENCY &&
                  std::abs(run.status.suggested_value - 0.36) < 1e-9,
              "late repairs: raise latency to 6 x RTT (360 ms)");
    }
    {
        /* Congestion: RTT far above its baseline with sustained loss. More
         * latency does not help; a lower bitrate does. */
        Run run(RECEIVER);
        Condition base = clean;
        base.latency = 0.2;
        run.feed(base, 5);
        Condition c = base;
        c.rtt = 0.28;
        c.packets_per_second = 280;
        c.loss_per_second = 220;
        c.drops_per_second = 220;
        c.belated_per_second = 2;
        c.headroom = 0.0;
        check(run.feed(c, 6) == ROBOTWEAX_SRT_TELEMETRY_HEALTH_FAILING &&
                  run.status.reason == ROBOTWEAX_SRT_TELEMETRY_REASON_PATH_CONGESTED &&
                  run.status.knob == ROBOTWEAX_SRT_TELEMETRY_KNOB_BITRATE &&
                  run.status.knob_direction == ROBOTWEAX_SRT_TELEMETRY_KNOB_DIRECTION_DECREASE,
              "congested path: lower bitrate, got " + describe(run.status));
        /* 0.8 x what the path delivers (280 packets/s, mixed with 5 s at 300). */
        check(run.status.suggested_value > 0.7 * 280 * packet_bits && run.status.suggested_value < 0.8 * 300 * packet_bits,
              "congestion advice: 80 % of the delivered rate");
        /* The path recovers: the drops stay counted for a while, and keep
         * the diagnosis they had (not "raise the latency"). */
        bool changed = false;
        for (int i = 0; i < 150; ++i)
        {
            const auto verdict = run.feed(base, 0.1);
            changed = changed || (verdict > ROBOTWEAX_SRT_TELEMETRY_HEALTH_WATCH &&
                                  run.status.knob != ROBOTWEAX_SRT_TELEMETRY_KNOB_BITRATE);
        }
        check(!changed, "after congestion: the drops keep their diagnosis, got " + describe(run.status));
    }
    {
        /* RTT step: latency only 2 x RTT, no loss yet: AT_RISK, raise latency. */
        Run run(RECEIVER);
        Condition c = clean;
        c.rtt = 0.06;
        check(run.feed(c, 5) == ROBOTWEAX_SRT_TELEMETRY_HEALTH_AT_RISK &&
                  run.status.reason == ROBOTWEAX_SRT_TELEMETRY_REASON_LATENCY_LOW_FOR_RTT &&
                  std::abs(run.status.suggested_value - 0.24) < 1e-9,
              "RTT step: AT_RISK, latency 4 x RTT");
    }
    {
        /* Wrong key mid-stream: undecryptable drops, check passphrase. */
        Run run(RECEIVER);
        Condition c = clean;
        c.drops_per_second = 300;
        c.undecryptable_per_second = 300;
        c.receiving = true;
        check(run.feed(c, 5) == ROBOTWEAX_SRT_TELEMETRY_HEALTH_FAILING &&
                  run.status.reason == ROBOTWEAX_SRT_TELEMETRY_REASON_UNDECRYPTABLE &&
                  run.status.knob == ROBOTWEAX_SRT_TELEMETRY_KNOB_PASSPHRASE,
              "undecryptable: FAILING, check passphrase");
    }
    {
        /* Application stopped reading: data piles up beyond the latency. */
        Run run(RECEIVER);
        run.feed(clean, 3);
        Condition c = clean;
        c.headroom = 3.0;
        check(run.feed(c, 5) == ROBOTWEAX_SRT_TELEMETRY_HEALTH_AT_RISK &&
                  run.status.reason == ROBOTWEAX_SRT_TELEMETRY_REASON_APPLICATION_READ_STALL &&
                  run.status.knob == ROBOTWEAX_SRT_TELEMETRY_KNOB_RECEIVING_APPLICATION,
              "application read stall");
    }
    {
        /* Arrivals stop: never OK. The verdict holds for the count window,
         * then becomes UNKNOWN / NO_DATA. */
        Run run(RECEIVER);
        run.feed(clean, 5);
        Condition c = clean;
        c.receiving = false;
        bool never_wrong = true;
        for (int i = 0; i < 300; ++i)
        {
            run.feed(c, 0.1);
            /* After the gauge window plus the 2 s hold, OK is no longer allowed. */
            if (i >= 31)
            {
                never_wrong = never_wrong && run.status.verdict != ROBOTWEAX_SRT_TELEMETRY_HEALTH_OK;
            }
        }
        check(never_wrong && run.status.verdict == ROBOTWEAX_SRT_TELEMETRY_HEALTH_UNKNOWN &&
                  run.status.reason == ROBOTWEAX_SRT_TELEMETRY_REASON_NO_DATA,
              "no arrivals: UNKNOWN / NO_DATA, never OK");
    }
}

void test_sender_classes()
{
    Condition clean;
    {
        /* Bandwidth cap: send buffer fills, then drops. Capacity known. */
        Run run(SENDER);
        run.feed(clean, 3);
        Condition c = clean;
        c.capacity_bps = 2.0e6; /* stream is ~3.16 Mbit/s */
        c.send_buffer_ratio = 0.95;
        c.sender_drops_per_second = 20;
        check(run.feed(c, 5) == ROBOTWEAX_SRT_TELEMETRY_HEALTH_FAILING &&
                  run.status.reason == ROBOTWEAX_SRT_TELEMETRY_REASON_SENDER_DROPS &&
                  run.status.knob == ROBOTWEAX_SRT_TELEMETRY_KNOB_BITRATE &&
                  run.status.knob_direction == ROBOTWEAX_SRT_TELEMETRY_KNOB_DIRECTION_DECREASE &&
                  std::abs(run.status.suggested_value - 1.4e6) < 1.0,
              "bandwidth cap: FAILING, lower bitrate to 70 % of capacity");
    }
    {
        /* Drops without loss reports, capacity or latency trouble: the
         * acknowledgements stopped (outage); no knob. */
        Run run(SENDER);
        run.feed(clean, 3);
        Condition c = clean;
        c.send_buffer_ratio = 1.0;
        c.sender_drops_per_second = 300;
        check(run.feed(c, 4) == ROBOTWEAX_SRT_TELEMETRY_HEALTH_FAILING &&
                  run.status.reason == ROBOTWEAX_SRT_TELEMETRY_REASON_ACKNOWLEDGEMENTS_STALLED &&
                  run.status.knob == ROBOTWEAX_SRT_TELEMETRY_KNOB_NONE,
              "drops without evidence of a cause: acknowledgements stalled, no knob");
    }
    {
        /* Sending at an absolute MAXBW with drops: raise MAXBW. */
        Run run(SENDER);
        robotweax_srt_telemetry_health_context_v1 context;
        robotweax_srt_telemetry_health_context_init(&context);
        context.connection = ROBOTWEAX_SRT_TELEMETRY_CONNECTION_CONNECTED;
        context.flags = ROBOTWEAX_SRT_TELEMETRY_HEALTH_CONTEXT_BANDWIDTH_SETTINGS;
        context.overhead_percent = 25;
        context.max_bandwidth_bytes_per_second = 395'000; /* 3.16 Mbit/s: the stream is at the cap */
        static_cast<void>(robotweax_srt_telemetry_health_engine_set_context(run.engine, &context));
        Condition c = clean;
        c.send_buffer_ratio = 1.0;
        c.sender_drops_per_second = 100;
        check(run.feed(c, 5) == ROBOTWEAX_SRT_TELEMETRY_HEALTH_FAILING &&
                  run.status.knob == ROBOTWEAX_SRT_TELEMETRY_KNOB_MAX_BANDWIDTH &&
                  run.status.suggested_value > 395'000,
              "drops at the MAXBW cap: raise MAXBW, got " + describe(run.status));
        /* MAXBW lifted: the backlog the cap left is dropped, with no other
         * evidence. Those drops are the cap's, not "acknowledgements
         * stalled". */
        context.max_bandwidth_bytes_per_second = -1;
        static_cast<void>(robotweax_srt_telemetry_health_engine_set_context(run.engine, &context));
        Condition flush = clean;
        flush.send_buffer_ratio = 0.5;
        flush.sender_drops_per_second = 100;
        bool wrong = false;
        const auto watch = [&](const Condition &cond, const int steps)
        {
            for (int i = 0; i < steps; ++i)
            {
                const auto verdict = run.feed(cond, 0.1);
                wrong = wrong || (verdict > ROBOTWEAX_SRT_TELEMETRY_HEALTH_WATCH &&
                                  run.status.knob != ROBOTWEAX_SRT_TELEMETRY_KNOB_MAX_BANDWIDTH);
            }
        };
        watch(flush, 20);
        watch(clean, 100);
        check(!wrong, "after MAXBW is lifted: the drops keep their diagnosis, got " + describe(run.status));
    }
    {
        /* An outage seen from the sender: data waits without
         * acknowledgements, then the path returns with a burst of loss
         * reports, retransmissions and drops of the stale backlog. Not a
         * bitrate problem. */
        Run run(SENDER);
        run.feed(clean, 3);
        Condition stall = clean;
        stall.send_buffer_ratio = 1.0;
        stall.packets_per_second = 0;
        run.feed(stall, 3);
        Condition burst = clean;
        burst.send_buffer_ratio = 0.8;
        burst.sender_drops_per_second = 300;
        burst.retransmitted_per_second = 150;
        bool wrong = false;
        for (int i = 0; i < 160; ++i)
        {
            const auto verdict = run.feed(i < 10 ? burst : clean, 0.1);
            wrong = wrong || (verdict > ROBOTWEAX_SRT_TELEMETRY_HEALTH_WATCH &&
                              run.status.knob != ROBOTWEAX_SRT_TELEMETRY_KNOB_NONE);
        }
        check(!wrong, "drops after an acknowledgement stall: no knob, got " + describe(run.status));
    }
    {
        /* The capacity estimate follows the sending rate on a healthy path:
         * a ratio near 1 is OK. */
        Run run(SENDER);
        Condition c = clean;
        c.capacity_bps = 3.0e6; /* 3.16 / 3.0 = 1.05 */
        check(run.feed(c, 5) == ROBOTWEAX_SRT_TELEMETRY_HEALTH_OK, "estimate near the sending rate is OK");
        /* Estimate clearly below the sending rate, nothing queues: not
         * evidence on its own (jitter pulls the estimate down). */
        c.capacity_bps = 2.0e6; /* 1.58 */
        check(run.feed(c, 5) == ROBOTWEAX_SRT_TELEMETRY_HEALTH_OK &&
                  run.status.indicators[ROBOTWEAX_SRT_TELEMETRY_INDICATOR_PATH_HEADROOM].value > 1.5,
              "estimate below the rate without congestion: value shown, still OK");
        /* With the send buffer filling it is real: AT_RISK. */
        c.send_buffer_ratio = 0.45;
        check(run.feed(c, 5) == ROBOTWEAX_SRT_TELEMETRY_HEALTH_AT_RISK &&
                  run.status.reason == ROBOTWEAX_SRT_TELEMETRY_REASON_PATH_CAPACITY_EXCEEDED &&
                  std::abs(run.status.suggested_value - 1.4e6) < 1.0,
              "estimate below the rate with a filling send buffer: AT_RISK, bitrate 70 % of capacity");
    }
    {
        /* Heavy repairs against a 25 % OHEADBW budget. */
        Run run(SENDER);
        Condition c = clean;
        c.retransmitted_per_second = 63; /* 21 % of unique: 0.84 of budget */
        check(run.feed(c, 12) == ROBOTWEAX_SRT_TELEMETRY_HEALTH_AT_RISK &&
                  run.status.reason == ROBOTWEAX_SRT_TELEMETRY_REASON_OVERHEAD_BUDGET_HIGH &&
                  run.status.knob == ROBOTWEAX_SRT_TELEMETRY_KNOB_OVERHEAD_BANDWIDTH &&
                  run.status.suggested_value == 35.0,
              "overhead budget: AT_RISK, OHEADBW to 35 %");
    }
    {
        /* Receiver stopped reading: it shrinks the window, flight fills it. */
        Run run(SENDER);
        run.feed(clean, 2);
        Condition c = clean;
        c.flight_to_flow = 0.99;
        c.flow_window = 64;
        check(run.feed(c, 5) == ROBOTWEAX_SRT_TELEMETRY_HEALTH_FAILING &&
                  run.status.reason == ROBOTWEAX_SRT_TELEMETRY_REASON_PEER_READ_STALL &&
                  run.status.knob == ROBOTWEAX_SRT_TELEMETRY_KNOB_RECEIVING_APPLICATION,
              "peer read stall: FAILING, check the receiving application");
    }
    {
        /* Full flight with an unchanged window: acknowledgements stopped. */
        Run run(SENDER);
        run.feed(clean, 2);
        Condition c = clean;
        c.flight_to_flow = 0.99;
        check(run.feed(c, 5) == ROBOTWEAX_SRT_TELEMETRY_HEALTH_FAILING &&
                  run.status.reason == ROBOTWEAX_SRT_TELEMETRY_REASON_ACKNOWLEDGEMENTS_STALLED &&
                  run.status.knob == ROBOTWEAX_SRT_TELEMETRY_KNOB_NONE,
              "stalled acknowledgements are not blamed on the application");
    }
    {
        /* Absolute MAXBW exhausted from the start: the budget uses the time
         * the window covers, not the full 10 s. */
        Run run(SENDER);
        robotweax_srt_telemetry_health_context_v1 context;
        robotweax_srt_telemetry_health_context_init(&context);
        context.connection = ROBOTWEAX_SRT_TELEMETRY_CONNECTION_CONNECTED;
        context.flags = ROBOTWEAX_SRT_TELEMETRY_HEALTH_CONTEXT_BANDWIDTH_SETTINGS;
        context.max_bandwidth_bytes_per_second = 400'000; /* 3.2 Mbit/s */
        static_cast<void>(robotweax_srt_telemetry_health_engine_set_context(run.engine, &context));
        Condition c = clean; /* 3.16 Mbit/s unique */
        c.retransmitted_per_second = 30;
        run.feed(c, 1.5);
        const auto &budget = run.status.indicators[ROBOTWEAX_SRT_TELEMETRY_INDICATOR_OVERHEAD_BUDGET];
        check((budget.flags & ROBOTWEAX_SRT_TELEMETRY_INDICATOR_AVAILABLE) != 0 && budget.value > 1.0,
              "exhausted MAXBW budget is visible within 1.5 s");
        check(run.feed(c, 3) == ROBOTWEAX_SRT_TELEMETRY_HEALTH_FAILING &&
                  run.status.knob == ROBOTWEAX_SRT_TELEMETRY_KNOB_MAX_BANDWIDTH &&
                  std::abs(run.status.suggested_value - 543'000) < 1'500,
              "MAXBW advice in bytes/s (1.25 x sending rate)");
    }
    {
        /* RTT step on the sender side. */
        Run run(SENDER);
        Condition c = clean;
        c.rtt = 0.1; /* 120 ms latency = 1.2 x RTT */
        check(run.feed(c, 5) == ROBOTWEAX_SRT_TELEMETRY_HEALTH_FAILING &&
                  run.status.reason == ROBOTWEAX_SRT_TELEMETRY_REASON_LATENCY_LOW_FOR_RTT,
              "sender: latency below RTT x 1.5 is FAILING");
    }
    {
        /* Sending stops: UNKNOWN, not OK. */
        Run run(SENDER);
        run.feed(clean, 3);
        Condition c = clean;
        c.sending = false;
        run.feed(c, 25);
        check(run.status.verdict == ROBOTWEAX_SRT_TELEMETRY_HEALTH_UNKNOWN &&
                  run.status.reason == ROBOTWEAX_SRT_TELEMETRY_REASON_NO_DATA,
              "idle sender: UNKNOWN / NO_DATA, got " + describe(run.status));
    }
    {
        /* Without bandwidth settings the overhead indicator is unavailable,
         * which does not block an OK (it is not required). */
        Run run(SENDER);
        robotweax_srt_telemetry_health_context_v1 context;
        robotweax_srt_telemetry_health_context_init(&context);
        static_cast<void>(robotweax_srt_telemetry_health_engine_set_context(run.engine, &context));
        check(run.feed(clean, 3) == ROBOTWEAX_SRT_TELEMETRY_HEALTH_OK &&
                  run.status.indicators[ROBOTWEAX_SRT_TELEMETRY_INDICATOR_OVERHEAD_BUDGET].reason ==
                      ROBOTWEAX_SRT_TELEMETRY_REASON_MISSING_SETTING,
              "overhead budget needs the bandwidth settings");
    }
}

void test_review_cases()
{
    Condition clean;
    {
        /* Persistent congestion keeps its diagnosis: lossy samples never
         * raise the RTT baseline. */
        Run run(RECEIVER);
        run.feed(clean, 5);
        Condition c = clean;
        c.latency = 0.2;
        c.rtt = 0.28;
        c.packets_per_second = 280;
        c.loss_per_second = 220;
        c.drops_per_second = 220;
        c.belated_per_second = 2;
        c.headroom = 0.0;
        bool kept = true;
        for (int second_index = 0; second_index < 120; ++second_index)
        {
            run.feed(c, 1);
            if (second_index >= 10)
            {
                kept = kept && run.status.reason == ROBOTWEAX_SRT_TELEMETRY_REASON_PATH_CONGESTED;
            }
        }
        check(kept, "two minutes of congestion stay path_congested, got " + describe(run.status));
    }
    {
        /* A healthy RTT increase without loss is not congestion. */
        Run run(RECEIVER);
        run.feed(clean, 5);
        Condition c = clean;
        c.latency = 0.6;
        c.rtt = 0.09;
        check(run.feed(c, 20) == ROBOTWEAX_SRT_TELEMETRY_HEALTH_OK, "RTT rise without loss: OK");
    }
    {
        /* An outage longer than the latency: drops with no knob. */
        Run run(RECEIVER);
        run.feed(clean, 5);
        Condition gap = clean;
        gap.receiving = false;
        run.feed(gap, 2);
        Condition after = clean;
        after.drops_per_second = 300;
        after.loss_per_second = 300;
        after.belated_per_second = 20; /* some repairs arrive late too */
        run.feed(after, 0.5);
        check(run.feed(clean, 3) == ROBOTWEAX_SRT_TELEMETRY_HEALTH_FAILING &&
                  run.status.knob == ROBOTWEAX_SRT_TELEMETRY_KNOB_NONE,
              "drops after a gap longer than the latency: no knob, got " + describe(run.status));
        /* The gap leaves the count window before its drops do: the drops
         * are still the outage's, never latency advice. */
        bool wrong_knob = false;
        for (int i = 0; i < 120; ++i)
        {
            const auto verdict = run.feed(clean, 0.1);
            wrong_knob = wrong_knob ||
                         (verdict > ROBOTWEAX_SRT_TELEMETRY_HEALTH_WATCH && run.status.knob != ROBOTWEAX_SRT_TELEMETRY_KNOB_NONE);
        }
        check(!wrong_knob, "outage aftermath: no knob after the gap left the count window, got " + describe(run.status));
    }
    {
        /* Drops well after an outage are not the outage's: advice returns. */
        Run run(RECEIVER);
        run.feed(clean, 5);
        Condition gap = clean;
        gap.receiving = false;
        run.feed(gap, 2);
        run.feed(clean, 4);
        Condition lossy = clean;
        lossy.drops_per_second = 30;
        lossy.loss_per_second = 60;
        lossy.belated_per_second = 20;
        run.feed(lossy, 4);
        check(run.status.verdict > ROBOTWEAX_SRT_TELEMETRY_HEALTH_WATCH &&
                  run.status.knob == ROBOTWEAX_SRT_TELEMETRY_KNOB_LATENCY,
              "late repairs 4 s after an outage: latency advice, got " + describe(run.status));
    }
    {
        /* After a gap, OK waits for evidence: the drops that arrive with the
         * first packets must not follow an OK. */
        Run run(RECEIVER);
        run.feed(clean, 5);
        Condition gap = clean;
        gap.receiving = false;
        run.feed(gap, 3);
        bool ok_seen = false;
        const auto step = [&](const Condition &c)
        {
            const bool ok = run.feed(c, 0.1) == ROBOTWEAX_SRT_TELEMETRY_HEALTH_OK;
            ok_seen = ok_seen || ok;
        };
        for (int i = 0; i < 3; ++i)
        {
            step(clean);
        }
        Condition after = clean;
        after.drops_per_second = 300;
        after.loss_per_second = 300;
        step(after);
        for (int i = 0; i < 30; ++i)
        {
            step(clean);
        }
        check(!ok_seen && run.status.verdict == ROBOTWEAX_SRT_TELEMETRY_HEALTH_FAILING,
              "no OK between a gap and its drops, got " + describe(run.status));
    }
    {
        /* Libsrt grows the flow window by dropped packets: that must not
         * become the reference and leave a stall bias afterwards. */
        Run run(SENDER);
        run.feed(clean, 3);
        Condition outage = clean;
        outage.send_buffer_ratio = 1.0;
        outage.sender_drops_per_second = 300;
        outage.flow_window = 11'000;
        run.feed(outage, 2);
        run.feed(clean, 30);
        check(run.status.verdict == ROBOTWEAX_SRT_TELEMETRY_HEALTH_OK &&
                  run.status.indicators[ROBOTWEAX_SRT_TELEMETRY_INDICATOR_PEER_READ_STALL].value <= 0.05 + 1e-9,
              "no read-stall bias after an outage with drops, got " + describe(run.status));
    }
}

void test_unknown_and_context()
{
    {
        /* A provider without drop counts can never give a receiver OK. */
        Run run(RECEIVER);
        for (int i = 0; i < 50; ++i)
        {
            run.now += period;
            auto s = sample(RECEIVER, run.now, Condition{});
            s.available_metrics &= ~ROBOTWEAX_SRT_TELEMETRY_METRIC_BIT(
                ROBOTWEAX_SRT_TELEMETRY_METRIC_RECEIVER_DROP_PACKETS_PER_SECOND);
            static_cast<void>(robotweax_srt_telemetry_health_engine_push(run.engine, &s, &run.status));
        }
        check(run.status.verdict == ROBOTWEAX_SRT_TELEMETRY_HEALTH_UNKNOWN &&
                  run.status.reason == ROBOTWEAX_SRT_TELEMETRY_REASON_MISSING_METRIC &&
                  (run.status.missing_metrics &
                   ROBOTWEAX_SRT_TELEMETRY_METRIC_BIT(
                       ROBOTWEAX_SRT_TELEMETRY_METRIC_RECEIVER_DROP_PACKETS_PER_SECOND)) != 0,
              "missing drop counter: UNKNOWN naming the metric");
    }
    {
        Run run(RECEIVER);
        run.feed(Condition{}, 3);
        robotweax_srt_telemetry_health_context_v1 context;
        robotweax_srt_telemetry_health_context_init(&context);
        context.connection = ROBOTWEAX_SRT_TELEMETRY_CONNECTION_LOST;
        static_cast<void>(robotweax_srt_telemetry_health_engine_set_context(run.engine, &context));
        check(run.feed(Condition{}, 0.1) == ROBOTWEAX_SRT_TELEMETRY_HEALTH_FAILING &&
                  run.status.reason == ROBOTWEAX_SRT_TELEMETRY_REASON_CONNECTION_LOST &&
                  run.status.primary_indicator == -1,
              "connection lost: FAILING at once");
        context.connection = ROBOTWEAX_SRT_TELEMETRY_CONNECTION_CONNECTING;
        static_cast<void>(robotweax_srt_telemetry_health_engine_set_context(run.engine, &context));
        check(run.feed(Condition{}, 0.1) == ROBOTWEAX_SRT_TELEMETRY_HEALTH_UNKNOWN &&
                  run.status.reason == ROBOTWEAX_SRT_TELEMETRY_REASON_CONNECTING,
              "connecting: UNKNOWN");
    }
    {
        Run run(RECEIVER);
        run.feed(Condition{}, 3);
        run.now += period;
        robotweax_srt_telemetry_metric_sample_v1 failed{};
        failed.struct_size = sizeof(failed);
        failed.abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
        failed.sample_monotonic_nano = run.now;
        failed.quality_flags = ROBOTWEAX_SRT_TELEMETRY_METRIC_SAMPLE_SOURCE_ERROR;
        static_cast<void>(robotweax_srt_telemetry_health_engine_push(run.engine, &failed, &run.status));
        check(run.status.verdict == ROBOTWEAX_SRT_TELEMETRY_HEALTH_OK &&
                  run.status.reason == ROBOTWEAX_SRT_TELEMETRY_REASON_SOURCE_ERROR,
              "one failed read keeps the verdict, with the reason");
        check(run.feed(Condition{}, 0.1) == ROBOTWEAX_SRT_TELEMETRY_HEALTH_OK &&
                  run.status.reason == ROBOTWEAX_SRT_TELEMETRY_REASON_NONE,
              "back to OK after the error");
        for (int i = 0; i < 25; ++i)
        {
            run.now += period;
            failed.sample_monotonic_nano = run.now;
            static_cast<void>(robotweax_srt_telemetry_health_engine_push(run.engine, &failed, &run.status));
        }
        check(run.status.verdict == ROBOTWEAX_SRT_TELEMETRY_HEALTH_UNKNOWN &&
                  run.status.reason == ROBOTWEAX_SRT_TELEMETRY_REASON_SOURCE_ERROR,
              "failed reads for 2 s: UNKNOWN");
    }
    {
        Run run(ROBOTWEAX_SRT_TELEMETRY_ENDPOINT_ROLE_UNKNOWN);
        check(run.feed(Condition{}, 2) == ROBOTWEAX_SRT_TELEMETRY_HEALTH_UNKNOWN &&
                  run.status.reason == ROBOTWEAX_SRT_TELEMETRY_REASON_ROLE_UNKNOWN,
              "unknown role: UNKNOWN");
    }
    {
        /* Headroom is low while the first latency period fills: no verdict
         * from it during warm-up. */
        Run run(RECEIVER);
        Condition c;
        c.latency = 2.0;
        c.rtt = 0.03;
        c.headroom = 0.2;
        run.feed(c, 2.5);
        check(run.status.verdict == ROBOTWEAX_SRT_TELEMETRY_HEALTH_OK, "warm-up headroom ignored");
    }
    {
        /* Slow sampling (1.05 s) still grades headroom after warm-up. */
        Run run(RECEIVER);
        Condition c;
        c.headroom = 0.05;
        for (int i = 0; i < 20; ++i)
        {
            run.now += second + second / 20;
            auto s = sample(RECEIVER, run.now, c);
            s.interval_nano = second + second / 20;
            s.rate_numerators[ROBOTWEAX_SRT_TELEMETRY_METRIC_UNIQUE_RECEIVED_PACKETS_PER_SECOND] = 315;
            static_cast<void>(robotweax_srt_telemetry_health_engine_push(run.engine, &s, &run.status));
        }
        check(run.status.verdict == ROBOTWEAX_SRT_TELEMETRY_HEALTH_FAILING &&
                  run.status.reason == ROBOTWEAX_SRT_TELEMETRY_REASON_RECOVERY_HEADROOM_LOW,
              "headroom graded at 1.05 s sampling, got " + describe(run.status));
    }
    {
        /* Escalation survives short availability gaps. */
        Run run(RECEIVER);
        Condition clean;
        run.feed(clean, 3);
        Condition low = clean;
        low.headroom = 0.05;
        bool reached = false;
        for (int i = 0; i < 40 && !reached; ++i)
        {
            run.now += period;
            auto s = sample(RECEIVER, run.now, low);
            if (i % 3 == 2)
            {
                s.available_metrics &= ~ROBOTWEAX_SRT_TELEMETRY_METRIC_BIT(
                    ROBOTWEAX_SRT_TELEMETRY_METRIC_RECOVERY_HEADROOM_RATIO);
            }
            static_cast<void>(robotweax_srt_telemetry_health_engine_push(run.engine, &s, &run.status));
            reached = run.status.verdict == ROBOTWEAX_SRT_TELEMETRY_HEALTH_FAILING;
        }
        check(reached, "flickering availability does not block escalation");
    }
}

struct FakeReceiver
{
    std::uint64_t unique = 0;
    std::uint64_t drops = 0;
    std::uint64_t loss = 0;
    std::uint64_t drops_per_tick = 0;
    robotweax_srt_telemetry_delta_engine *engine = nullptr;
};

robotweax_srt_telemetry_error fake_receiver(void *data, const std::uint64_t unix_nano,
                                            const std::uint64_t monotonic_nano,
                                            robotweax_srt_telemetry_source_output_v1 *output)
{
    auto &f = *static_cast<FakeReceiver *>(data);
    f.unique += 30; /* 300 packets/s at 100 ms */
    f.drops += f.drops_per_tick;
    f.loss += f.drops_per_tick;
    auto &raw = output->raw;
    raw.provider = ROBOTWEAX_SRT_TELEMETRY_PROVIDER_ROBOTWEAX_SRT;
    raw.observed_at_unix_nano = unix_nano;
    raw.sample_monotonic_nano = monotonic_nano;
    raw.available_fields = ROBOTWEAX_SRT_TELEMETRY_FIELD_UNIQUE_RECEIVED_PACKET_TOTAL |
                           ROBOTWEAX_SRT_TELEMETRY_FIELD_UNIQUE_RECEIVED_WIRE_BYTE_TOTAL |
                           ROBOTWEAX_SRT_TELEMETRY_FIELD_RECEIVER_LOSS_PACKET_TOTAL |
                           ROBOTWEAX_SRT_TELEMETRY_FIELD_RECEIVER_DROP_PACKET_TOTAL |
                           ROBOTWEAX_SRT_TELEMETRY_FIELD_RTT_SECONDS |
                           ROBOTWEAX_SRT_TELEMETRY_FIELD_RECEIVE_BUFFER_SECONDS |
                           ROBOTWEAX_SRT_TELEMETRY_FIELD_RECEIVER_TSBPD_DELAY_SECONDS;
    raw.unique_received_packet_total = f.unique;
    raw.unique_received_wire_byte_total = f.unique * 1316;
    raw.receiver_loss_packet_total = f.loss;
    raw.receiver_drop_packet_total = f.drops;
    raw.rtt_seconds = 0.02;
    raw.receive_buffer_seconds = 0.12;
    raw.receiver_tsbpd_delay_seconds = 0.12;
    return robotweax_srt_telemetry_delta_engine_update(f.engine, &raw, &output->delta);
}

struct HealthEvents
{
    std::vector<robotweax_srt_telemetry_health_verdict> verdicts;
};

void on_health(void *user, const robotweax_srt_telemetry_source_info_v1 *,
               const robotweax_srt_telemetry_health_status_v1 *status)
{
    static_cast<HealthEvents *>(user)->verdicts.push_back(status->verdict);
}

void test_sampler_health()
{
    robotweax_srt_telemetry_sampler_config_v1 config{};
    robotweax_srt_telemetry_sampler_config_default(&config);
    robotweax_srt_telemetry_sampler *sampler = nullptr;
    static_cast<void>(robotweax_srt_telemetry_sampler_create(&config, &sampler));
    HealthEvents events;
    check(robotweax_srt_telemetry_sampler_set_health_callback(sampler, &on_health, &events) ==
                  ROBOTWEAX_SRT_TELEMETRY_OK &&
              robotweax_srt_telemetry_sampler_enable_journal(sampler, 64) == ROBOTWEAX_SRT_TELEMETRY_OK &&
              robotweax_srt_telemetry_sampler_enable_history(sampler, 60) == ROBOTWEAX_SRT_TELEMETRY_OK,
          "sampler setup");

    FakeReceiver fake;
    static_cast<void>(robotweax_srt_telemetry_delta_engine_create(&fake.engine));
    robotweax_srt_telemetry_source_config_v1 source{};
    source.struct_size = sizeof(source);
    source.abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
    source.sample = &fake_receiver;
    source.source_data = &fake;
    source.role = RECEIVER;
    std::uint64_t id = 0;
    static_cast<void>(robotweax_srt_telemetry_sampler_add_source(sampler, &source, &id));
    auto bad_role = source;
    bad_role.role = static_cast<robotweax_srt_telemetry_endpoint_role>(9);
    std::uint64_t ignored = 0;
    check(robotweax_srt_telemetry_sampler_add_source(sampler, &bad_role, &ignored) ==
              ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT,
          "invalid role rejected");

    robotweax_srt_telemetry_health_status_v1 status{};
    check(robotweax_srt_telemetry_sampler_health_read(sampler, id, &status) ==
              ROBOTWEAX_SRT_TELEMETRY_INVALID_STATE,
          "health off by default");
    check(robotweax_srt_telemetry_sampler_enable_health(sampler, 1) == ROBOTWEAX_SRT_TELEMETRY_OK,
          "enable health for existing sources");

    const std::uint64_t base_unix = UINT64_C(1'800'000'000) * second;
    std::uint64_t tick = 0;
    const auto run = [&](const double seconds)
    {
        for (int i = 0; i < static_cast<int>(seconds * 10); ++i, ++tick)
        {
            static_cast<void>(robotweax_srt_telemetry_sampler_tick(sampler, base_unix + tick * period,
                                                                   second + tick * period));
        }
    };
    run(5);
    check(robotweax_srt_telemetry_sampler_health_read(sampler, id, &status) == ROBOTWEAX_SRT_TELEMETRY_OK &&
              status.verdict == ROBOTWEAX_SRT_TELEMETRY_HEALTH_OK,
          "clean receiver is OK through the sampler, got " + describe(status));
    fake.drops_per_tick = 2; /* 2 of 32: far above 1 % */
    run(4);
    fake.drops_per_tick = 0;
    static_cast<void>(robotweax_srt_telemetry_sampler_health_read(sampler, id, &status));
    /* Both drop indicators fail; whichever crossed first explains it. */
    check(status.verdict == ROBOTWEAX_SRT_TELEMETRY_HEALTH_FAILING &&
              (status.reason == ROBOTWEAX_SRT_TELEMETRY_REASON_UNRECOVERED_LOSS ||
               status.reason == ROBOTWEAX_SRT_TELEMETRY_REASON_RECOVERY_INEFFICIENT) &&
              status.knob == ROBOTWEAX_SRT_TELEMETRY_KNOB_LATENCY,
          "sustained drops: FAILING through the sampler, got " + describe(status));
    bool saw_ok = false;
    for (const auto v : events.verdicts)
    {
        saw_ok = saw_ok || v == ROBOTWEAX_SRT_TELEMETRY_HEALTH_OK;
    }
    check(saw_ok && events.verdicts.back() == ROBOTWEAX_SRT_TELEMETRY_HEALTH_FAILING,
          "health callback sees the changes");

    std::vector<robotweax_srt_telemetry_event_v1> journal(64);
    std::size_t count = 0;
    static_cast<void>(robotweax_srt_telemetry_sampler_journal_read(sampler, 0, journal.data(), journal.size(),
                                                                   &count, nullptr));
    int verdict_events = 0;
    bool failing_logged = false;
    for (std::size_t i = 0; i < count; ++i)
    {
        if (journal[i].kind == ROBOTWEAX_SRT_TELEMETRY_EVENT_VERDICT_CHANGED)
        {
            ++verdict_events;
            failing_logged = failing_logged ||
                             (journal[i].current_value == ROBOTWEAX_SRT_TELEMETRY_HEALTH_FAILING &&
                              journal[i].severity == ROBOTWEAX_SRT_TELEMETRY_SEVERITY_ERROR &&
                              std::string_view(journal[i].detail).find("latency increase") != std::string_view::npos);
        }
    }
    check(verdict_events >= 2 && failing_logged, "verdict changes journaled with reason and advice");

    std::vector<robotweax_srt_telemetry_compact_record_v1> records(60);
    static_cast<void>(robotweax_srt_telemetry_sampler_history_read(sampler, id, 0, records.data(), records.size(),
                                                                   &count));
    bool ok_record = false;
    bool failing_record = false;
    for (std::size_t i = 0; i < count; ++i)
    {
        ok_record = ok_record || records[i].health_verdict == ROBOTWEAX_SRT_TELEMETRY_HEALTH_OK;
        failing_record = failing_record ||
                         (records[i].health_verdict == ROBOTWEAX_SRT_TELEMETRY_HEALTH_FAILING &&
                          records[i].health_reason != ROBOTWEAX_SRT_TELEMETRY_REASON_NONE);
    }
    check(ok_record && failing_record, "history records carry each second's worst verdict");

    /* Context: connection lost is FAILING at once. */
    robotweax_srt_telemetry_health_context_v1 context;
    robotweax_srt_telemetry_health_context_init(&context);
    context.connection = ROBOTWEAX_SRT_TELEMETRY_CONNECTION_LOST;
    check(robotweax_srt_telemetry_sampler_set_health_context(sampler, 999, &context) ==
              ROBOTWEAX_SRT_TELEMETRY_NOT_FOUND,
          "context for an unknown source");
    context.flags = 0x40;
    check(robotweax_srt_telemetry_sampler_set_health_context(sampler, id, &context) ==
              ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT,
          "invalid context rejected");
    context.flags = 0;
    static_cast<void>(robotweax_srt_telemetry_sampler_set_health_context(sampler, id, &context));
    run(0.1);
    static_cast<void>(robotweax_srt_telemetry_sampler_health_read(sampler, id, &status));
    check(status.reason == ROBOTWEAX_SRT_TELEMETRY_REASON_CONNECTION_LOST, "lost connection through the sampler");

    /* Thresholds apply to current and future sources of the role. */
    context.connection = ROBOTWEAX_SRT_TELEMETRY_CONNECTION_CONNECTED;
    static_cast<void>(robotweax_srt_telemetry_sampler_set_health_context(sampler, id, &context));
    check(robotweax_srt_telemetry_sampler_set_health_thresholds(
              sampler, RECEIVER, ROBOTWEAX_SRT_TELEMETRY_INDICATOR_RECOVERY_HEADROOM, 0.1, 0.5, 0.05) ==
              ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT,
          "unordered sampler thresholds rejected");
    check(robotweax_srt_telemetry_sampler_set_health_thresholds(
              sampler, RECEIVER, ROBOTWEAX_SRT_TELEMETRY_INDICATOR_LATENCY_SUFFICIENCY, 10.0, 7.0, 3.0) ==
              ROBOTWEAX_SRT_TELEMETRY_OK,
          "sampler thresholds");
    run(40); /* drops have aged out; latency / RTT = 6 is AT_RISK under the stricter thresholds */
    static_cast<void>(robotweax_srt_telemetry_sampler_health_read(sampler, id, &status));
    check(status.verdict == ROBOTWEAX_SRT_TELEMETRY_HEALTH_AT_RISK &&
              status.reason == ROBOTWEAX_SRT_TELEMETRY_REASON_LATENCY_LOW_FOR_RTT,
          "overridden thresholds take effect, got " + describe(status));

    check(robotweax_srt_telemetry_sampler_enable_health(sampler, 0) == ROBOTWEAX_SRT_TELEMETRY_OK &&
              robotweax_srt_telemetry_sampler_health_read(sampler, id, &status) ==
                  ROBOTWEAX_SRT_TELEMETRY_INVALID_STATE,
          "health can be turned off");
    robotweax_srt_telemetry_sampler_destroy(sampler);
    robotweax_srt_telemetry_delta_engine_destroy(fake.engine);
}

} // namespace

int main()
{
    test_api();
    test_clean_streams();
    test_hysteresis();
    test_receiver_classes();
    test_sender_classes();
    test_unknown_and_context();
    test_review_cases();
    test_sampler_health();
    if (failures != 0)
    {
        std::cerr << failures << " health checks failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "all health checks passed\n";
    return EXIT_SUCCESS;
}
