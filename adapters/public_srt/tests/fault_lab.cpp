/* Fault lab: real SRT streams through an impairing UDP relay, with the
 * sampler and health engine on both ends. Each scenario connects a fresh
 * caller (sender) and listener (receiver) through the relay, streams for a
 * clean warm-up, applies one fault, and scores the verdict each role reports
 * against the verdicts that role should report.
 *
 * Gate (roadmap Phase 2): per role, the right verdict in at least 9 of 10
 * fault classes, every control OK throughout, and never a wrong OK: once a
 * fault has been active for the detection grace, a role that should see it
 * must not report OK (three consecutive reads) until 2 s after it ends;
 * and every At risk or Failing read must name a right knob, from 4 s after
 * the first one until the end of the settle period.
 *
 * Usage: fault_lab [--scenario NAME] [--list] [--timeline] */

#include <robotweax/srt_telemetry_public_srt.h>
#include <robotweax/srt_telemetry_sampler.h>

#if defined(ROBOTWEAX_SRT_TELEMETRY_RUNTIME_USE_FLAT_SRT_HEADER)
#include <srt.h>
#define ROBOTWEAX_SRT_TELEMETRY_USE_FLAT_SRT_HEADER
#else
#include <srt/srt.h>
#endif
#include <robotweax/srt_telemetry_public_srt_host.h>

#include "impairing_relay.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <queue>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace
{

using Clock = std::chrono::steady_clock;
using Verdict = robotweax_srt_telemetry_health_verdict;

constexpr auto provider =
#if defined(ROBOTWEAX_SRT_TELEMETRY_RUNTIME_PROVIDER_ROBOTWEAX)
    ROBOTWEAX_SRT_TELEMETRY_PROVIDER_ROBOTWEAX_SRT;
constexpr std::string_view provider_name = "robotweax-srt";
constexpr std::uint64_t ext_fields = ROBOTWEAX_SRT_TELEMETRY_PUBLIC_SRT_ROBOTWEAX_EXT_CANDIDATE_FIELDS;
#else
    ROBOTWEAX_SRT_TELEMETRY_PROVIDER_HAIVISION_SRT;
constexpr std::string_view provider_name = "haivision-srt";
constexpr std::uint64_t ext_fields = ROBOTWEAX_SRT_TELEMETRY_PUBLIC_SRT_HAIVISION_V1_5_5_EXT_CANDIDATE_FIELDS;
#endif

constexpr Verdict UNKNOWN = ROBOTWEAX_SRT_TELEMETRY_HEALTH_UNKNOWN;
constexpr Verdict OK = ROBOTWEAX_SRT_TELEMETRY_HEALTH_OK;
constexpr Verdict WATCH = ROBOTWEAX_SRT_TELEMETRY_HEALTH_WATCH;
constexpr Verdict AT_RISK = ROBOTWEAX_SRT_TELEMETRY_HEALTH_AT_RISK;
constexpr Verdict FAILING = ROBOTWEAX_SRT_TELEMETRY_HEALTH_FAILING;

/* Default stream: 500 packets/s of 1316 bytes, about 5.3 Mbit/s of
 * payload. */
constexpr int payload_bytes = 1316;

double seconds_since(const Clock::time_point start)
{
    return std::chrono::duration<double>(Clock::now() - start).count();
}

using robotweax_srt_telemetry_lab::Impairment;
using robotweax_srt_telemetry_lab::Relay;

/* -------------------------------------------------------------- scenarios */

struct Scenario
{
    std::string name;
    std::string description;
    int latency_ms = 200;
    int packets_per_second = 500;
    int peer_idle_timeout_ms = 0; /* 0: provider default (5 s) */
    Impairment base;
    /* Applied at fault onset; restores the base at fault end unless the
     * fault is permanent. */
    Impairment fault;
    double fault_seconds = 12;
    double settle_seconds = 4;
    bool stall_receiver = false;
    bool small_receive_buffer = false;
    std::int64_t sender_max_bandwidth = 0; /* bytes/s at onset; 0 = unchanged */
    bool permanent = false;
    /* Verdicts each role should report for this fault (worst over the fault
     * and settle period). A role whose set contains OK cannot give a wrong
     * OK. Empty: not observable from that role, not scored. */
    std::set<Verdict> expect_sender;
    std::set<Verdict> expect_receiver;
    /* Knobs a right diagnosis names on every At risk or Failing read
     * (gated). Empty: advice not judged for that role. */
    std::set<robotweax_srt_telemetry_health_knob> knob_sender;
    std::set<robotweax_srt_telemetry_health_knob> knob_receiver;
    bool control = false;
};

constexpr auto K_NONE = ROBOTWEAX_SRT_TELEMETRY_KNOB_NONE;
constexpr auto K_LATENCY = ROBOTWEAX_SRT_TELEMETRY_KNOB_LATENCY;
constexpr auto K_BITRATE = ROBOTWEAX_SRT_TELEMETRY_KNOB_BITRATE;
constexpr auto K_MAXBW = ROBOTWEAX_SRT_TELEMETRY_KNOB_MAX_BANDWIDTH;
constexpr auto K_APP = ROBOTWEAX_SRT_TELEMETRY_KNOB_RECEIVING_APPLICATION;

std::vector<Scenario> scenarios()
{
    std::vector<Scenario> list;
    const std::set<Verdict> fine = {OK, WATCH};
    const std::set<Verdict> bad = {AT_RISK, FAILING};
    {
        Scenario s;
        s.name = "clean";
        s.description = "control: 20 ms RTT, 200 ms latency, no impairment, 60 s";
        s.fault = s.base;
        s.fault_seconds = 60;
        s.expect_sender = {OK};
        s.expect_receiver = {OK};
        s.control = true;
        list.push_back(s);
    }
    {
        Scenario s;
        s.name = "rtt_rise_healthy";
        s.description = "control: RTT rises from 20 to 60 ms with 400 ms latency, no loss";
        s.latency_ms = 400;
        s.fault.delay_ms = 30;
        s.expect_sender = {OK};
        s.expect_receiver = {OK};
        s.control = true;
        list.push_back(s);
    }
    {
        Scenario s;
        s.name = "high_bitrate";
        s.description = "control: 2500 packets/s (26 Mbit/s), no impairment";
        s.packets_per_second = 2500;
        s.fault = s.base;
        s.expect_sender = {OK};
        s.expect_receiver = {OK};
        s.control = true;
        list.push_back(s);
    }
    {
        Scenario s;
        s.name = "random_loss_2pct";
        s.description = "2 % random forward loss, ample latency: repaired";
        s.fault.forward_loss = 0.02;
        s.expect_sender = fine;
        s.expect_receiver = fine;
        list.push_back(s);
    }
    {
        Scenario s;
        s.name = "burst_loss";
        s.description = "30-packet loss burst every 2 s, ample latency: repaired";
        s.fault.burst_every_ms = 2000;
        s.fault.burst_packets = 30;
        s.expect_sender = fine;
        s.expect_receiver = fine;
        list.push_back(s);
    }
    {
        Scenario s;
        s.name = "jitter_reorder";
        s.description = "0-20 ms forward jitter (reordering), ample latency";
        s.fault.jitter_ms = 20;
        s.expect_sender = fine;
        s.expect_receiver = fine;
        list.push_back(s);
    }
    {
        Scenario s;
        s.name = "heavy_loss_tight_latency";
        s.description = "15 % forward loss, 60 ms latency on a 20 ms RTT path";
        s.latency_ms = 60;
        s.fault.forward_loss = 0.15;
        s.expect_sender = {WATCH, AT_RISK, FAILING};
        s.expect_receiver = bad;
        s.knob_sender = {K_LATENCY};
        s.knob_receiver = {K_LATENCY};
        list.push_back(s);
    }
    {
        Scenario s;
        s.name = "rtt_step";
        s.description = "RTT steps from 20 ms to 140 ms with 200 ms latency";
        s.fault.delay_ms = 70;
        s.expect_sender = bad;
        s.expect_receiver = bad;
        s.knob_sender = {K_LATENCY};
        s.knob_receiver = {K_LATENCY};
        list.push_back(s);
    }
    {
        Scenario s;
        s.name = "bandwidth_cap";
        s.description = "path capped at 3 Mbit/s for a 5.3 Mbit/s stream (100 kB queue), 40 s";
        s.fault_seconds = 40;
        s.fault.forward_rate_bps = 3e6;
        s.fault.queue_limit_bytes = 100'000;
        s.expect_sender = bad;
        s.expect_receiver = bad;
        s.knob_sender = {K_BITRATE};
        s.knob_receiver = {K_BITRATE};
        list.push_back(s);
    }
    {
        Scenario s;
        s.name = "outage_6s";
        s.description = "path blackholed for 6 s, then restored (peer idle timeout 10 s)";
        s.fault.blackhole = true;
        s.peer_idle_timeout_ms = 10'000;
        s.fault_seconds = 6;
        /* Long enough to watch the aftermath: the sender's backlog arrives
         * late after the outage, and its drops must not turn into latency
         * advice once the gap has left the count window. */
        s.settle_seconds = 20;
        /* The receiver cannot tell an outage from a paused source while it
         * lasts: Unknown, then Failing for the drops after the gap. */
        s.expect_sender = bad;
        s.expect_receiver = bad;
        /* No setting recovers a gap longer than the latency. */
        s.knob_sender = {K_NONE};
        s.knob_receiver = {K_NONE};
        list.push_back(s);
    }
    {
        Scenario s;
        s.name = "receiver_stall";
        s.description = "receiving application stops reading (3 MB receive buffer)";
        s.fault = s.base;
        s.stall_receiver = true;
        s.small_receive_buffer = true;
        s.expect_sender = bad;
        s.expect_receiver = bad;
        s.knob_sender = {K_APP};
        s.knob_receiver = {K_APP};
        list.push_back(s);
    }
    {
        Scenario s;
        s.name = "maxbw_too_low";
        s.description = "sender SRTO_MAXBW set to 2.8 Mbit/s for a 5.3 Mbit/s stream";
        s.fault = s.base;
        s.sender_max_bandwidth = 350'000;
        s.expect_sender = bad;
        s.expect_receiver = bad;
        s.knob_sender = {K_MAXBW}; /* the receiver cannot see the sender's setting */
        list.push_back(s);
    }
    {
        Scenario s;
        s.name = "connection_lost";
        s.description = "path blackholed for good: the peers time out";
        s.fault.blackhole = true;
        s.permanent = true;
        s.settle_seconds = 0;
        s.expect_sender = {FAILING};
        s.expect_receiver = {FAILING};
        list.push_back(s);
    }
    return list;
}

/* ------------------------------------------------------------- telemetry */

struct Monitor
{
    robotweax_srt_telemetry_public_srt_connection_monitor *value = nullptr;
    robotweax_srt_telemetry_health_context_v1 context{};
    ~Monitor()
    {
        robotweax_srt_telemetry_public_srt_connection_monitor_destroy(value);
    }
};

bool create_monitor(const SRTSOCKET socket, Monitor &monitor)
{
    robotweax_srt_telemetry_public_srt_connection_config_v1 config{};
    config.struct_size = sizeof(config);
    config.abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
    config.socket = socket;
    config.provider = provider;
    robotweax_srt_telemetry_host_connection_functions(&config);
    robotweax_srt_telemetry_health_context_init(&monitor.context);
    return robotweax_srt_telemetry_public_srt_connection_monitor_create(&config, &monitor.value) ==
           ROBOTWEAX_SRT_TELEMETRY_OK;
}

void poll_monitor(Monitor &monitor)
{
    robotweax_srt_telemetry_public_srt_connection_result_v1 result{};
    result.struct_size = sizeof(result);
    result.abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
    const auto now = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count());
    if (robotweax_srt_telemetry_public_srt_connection_monitor_poll(monitor.value, now, now, &result) ==
        ROBOTWEAX_SRT_TELEMETRY_OK)
    {
        static_cast<void>(robotweax_srt_telemetry_public_srt_health_context(&result, &monitor.context));
    }
}

struct Session
{
    robotweax_srt_telemetry_public_srt_session *value = nullptr;
    ~Session()
    {
        robotweax_srt_telemetry_public_srt_session_destroy(value);
    }
};

bool create_session(const SRTSOCKET socket, Session &session)
{
    robotweax_srt_telemetry_public_srt_config_v2 config{};
    config.struct_size = sizeof(config);
    config.abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
    config.socket = socket;
    config.provider = provider;
    config.capabilities = ROBOTWEAX_SRT_TELEMETRY_CAP_COMMON_STATS_V1 | ROBOTWEAX_SRT_TELEMETRY_CAP_FEC_STATS_V1;
    config.qualified_fields = ROBOTWEAX_SRT_TELEMETRY_PUBLIC_SRT_V1_5_5_CANDIDATE_FIELDS;
    config.qualified_ext_fields = ext_fields;
    config.bistats = &srt_bistats;
    return robotweax_srt_telemetry_public_srt_session_create_v2(&config, &session.value) ==
           ROBOTWEAX_SRT_TELEMETRY_OK;
}

struct Point
{
    double t;
    Verdict verdict;
    robotweax_srt_telemetry_health_reason reason;
    robotweax_srt_telemetry_health_knob knob;
    double value;
};

/* Per-second metric trace (--trace): the 1 s windows of both sources. */
struct Trace
{
    std::mutex mutex;
    std::uint64_t sender_id = 0;
    std::uint64_t receiver_id = 0;
    Clock::time_point start;
    bool enabled = false;
};

void on_window(void *user, const robotweax_srt_telemetry_source_info_v1 *source,
               const robotweax_srt_telemetry_window_summary_v1 *w)
{
    auto &trace = *static_cast<Trace *>(user);
    if (!trace.enabled || w->window_nano != UINT64_C(1'000'000'000))
    {
        return;
    }
    const auto m = [&](const robotweax_srt_telemetry_metric metric, const bool rate, const char *stat = "mean")
    {
        if ((w->available_metrics & ROBOTWEAX_SRT_TELEMETRY_METRIC_BIT(metric)) == 0)
        {
            return std::string("-");
        }
        const auto &x = w->metrics[metric];
        const double v = rate ? x.rate : std::string_view(stat) == "min" ? x.min : std::string_view(stat) == "max" ? x.max : x.mean;
        std::ostringstream o;
        o << std::setprecision(3) << v;
        return o.str();
    };
    std::lock_guard lock(trace.mutex);
    std::cout << "    " << std::fixed << std::setprecision(1) << seconds_since(trace.start) << std::defaultfloat;
    if (source->source_id == trace.sender_id)
    {
        std::cout << " S uniq=" << m(ROBOTWEAX_SRT_TELEMETRY_METRIC_UNIQUE_SENT_BITS_PER_SECOND, true)
                  << " retx=" << m(ROBOTWEAX_SRT_TELEMETRY_METRIC_RETRANSMITTED_BITS_PER_SECOND, true)
                  << " cap=" << m(ROBOTWEAX_SRT_TELEMETRY_METRIC_LINK_CAPACITY_BITS_PER_SECOND, false)
                  << " rtt=" << m(ROBOTWEAX_SRT_TELEMETRY_METRIC_RTT_SECONDS, false, "max")
                  << " sbuf=" << m(ROBOTWEAX_SRT_TELEMETRY_METRIC_SEND_BUFFER_TO_DROP_THRESHOLD_RATIO, false, "max")
                  << " fl/fw=" << m(ROBOTWEAX_SRT_TELEMETRY_METRIC_FLIGHT_TO_FLOW_WINDOW_RATIO, false, "max")
                  << " fw=" << m(ROBOTWEAX_SRT_TELEMETRY_METRIC_FLOW_WINDOW_PACKETS, false, "min")
                  << " sdrop=" << m(ROBOTWEAX_SRT_TELEMETRY_METRIC_SENDER_DROP_PACKETS_PER_SECOND, true)
                  << " sloss=" << m(ROBOTWEAX_SRT_TELEMETRY_METRIC_SENDER_LOSS_PACKETS_PER_SECOND, true) << '\n';
    }
    else
    {
        std::cout << " R uniq=" << m(ROBOTWEAX_SRT_TELEMETRY_METRIC_UNIQUE_RECEIVED_PACKETS_PER_SECOND, true)
                  << " loss=" << m(ROBOTWEAX_SRT_TELEMETRY_METRIC_RECEIVER_LOSS_PACKETS_PER_SECOND, true)
                  << " drop=" << m(ROBOTWEAX_SRT_TELEMETRY_METRIC_RECEIVER_DROP_PACKETS_PER_SECOND, true)
                  << " belated=" << m(ROBOTWEAX_SRT_TELEMETRY_METRIC_BELATED_PACKETS_PER_SECOND, true)
                  << " head=" << m(ROBOTWEAX_SRT_TELEMETRY_METRIC_RECOVERY_HEADROOM_RATIO, false, "min") << '/'
                  << m(ROBOTWEAX_SRT_TELEMETRY_METRIC_RECOVERY_HEADROOM_RATIO, false, "max")
                  << " rtt=" << m(ROBOTWEAX_SRT_TELEMETRY_METRIC_RTT_SECONDS, false, "max")
                  << " lat=" << m(ROBOTWEAX_SRT_TELEMETRY_METRIC_RECEIVER_TSBPD_DELAY_SECONDS, false) << '\n';
    }
}

/* ----------------------------------------------------------------- scoring */

const char *name(const Verdict v)
{
    return robotweax_srt_telemetry_health_verdict_name(v);
}

int severity(const Verdict v)
{
    return v == UNKNOWN ? 0 : static_cast<int>(v);
}

struct RoleScore
{
    bool scored = false;
    bool pass = false;
    bool wrong_ok = false;
    Verdict worst = UNKNOWN;
    std::string worst_reason;
    std::string advice;
    double first_detection = -1; /* seconds after onset */
    bool saw_unknown = false;
    bool advice_judged = false;
    bool advice_right = false;
    int advice_reads = 0;      /* At risk / Failing reads judged for advice */
    int advice_wrong = 0;      /* ... that named another knob */
    double advice_wrong_at = -1; /* first wrong read, seconds after onset */
    std::string wrong_knob;
    std::string knob;
};

std::string expect_text(const std::set<Verdict> &expected)
{
    std::string text;
    for (const auto v : expected)
    {
        text += text.empty() ? "" : "|";
        text += name(v);
    }
    return text;
}

RoleScore score(const std::vector<Point> &points, const std::set<Verdict> &expected,
                const std::set<robotweax_srt_telemetry_health_knob> &knobs, const double onset,
                const double fault_end, const double end, const bool control)
{
    RoleScore result;
    if (expected.empty())
    {
        return result;
    }
    result.scored = true;
    constexpr double grace = 4.0; /* gauge window 1 s + enter hold 2 s + margin */
    const bool ok_allowed = expected.count(OK) != 0;
    int ok_run = 0; /* consecutive OK reads: one read is not a verdict */
    for (const auto &p : points)
    {
        if (p.t < onset || p.t > end)
        {
            continue;
        }
        result.saw_unknown = result.saw_unknown || p.verdict == UNKNOWN;
        if (severity(p.verdict) > severity(result.worst))
        {
            result.worst = p.verdict;
            result.worst_reason = robotweax_srt_telemetry_health_reason_name(p.reason);
            std::ostringstream advice;
            if (p.knob != ROBOTWEAX_SRT_TELEMETRY_KNOB_NONE)
            {
                advice << robotweax_srt_telemetry_health_knob_name(p.knob);
                if (p.value != 0)
                {
                    advice << ' ' << std::setprecision(4) << p.value;
                }
            }
            result.advice = advice.str();
        }
        if (result.first_detection < 0 && p.verdict != OK && p.verdict != UNKNOWN && !ok_allowed)
        {
            result.first_detection = p.t - onset;
        }
        ok_run = p.verdict == OK ? ok_run + 1 : 0;
        /* The aftermath counts too: the first 2 s after a fault ends (drops
         * that arrive with the first packets after an outage). */
        if (!ok_allowed && ok_run >= 3 && p.t >= onset + grace && p.t <= fault_end + 2.0)
        {
            result.wrong_ok = true;
        }
    }
    /* Advice: the knob named most often at the worst verdict. */
    std::array<int, 8> knob_count{};
    for (const auto &p : points)
    {
        /* A lost connection has no knob; judge the diagnosis before it. */
        if (p.t >= onset && p.t <= end && p.verdict == result.worst && p.verdict != OK &&
            p.reason != ROBOTWEAX_SRT_TELEMETRY_REASON_CONNECTION_LOST &&
            p.reason != ROBOTWEAX_SRT_TELEMETRY_REASON_SOURCE_ERROR &&
            static_cast<std::size_t>(p.knob) < knob_count.size())
        {
            knob_count[static_cast<std::size_t>(p.knob)] += 1;
        }
    }
    const auto top = static_cast<robotweax_srt_telemetry_health_knob>(
        std::max_element(knob_count.begin(), knob_count.end()) - knob_count.begin());
    result.knob = robotweax_srt_telemetry_health_knob_name(top);
    for (const auto &p : points)
    {
        if (p.t >= onset && p.t <= end && p.verdict == result.worst && p.knob == top &&
            top != ROBOTWEAX_SRT_TELEMETRY_KNOB_NONE)
        {
            std::ostringstream advice;
            advice << result.knob;
            if (p.value != 0)
            {
                advice << ' ' << std::setprecision(4) << p.value;
            }
            result.advice = advice.str();
            result.worst_reason = robotweax_srt_telemetry_health_reason_name(p.reason);
        }
    }
    if (!knobs.empty())
    {
        /* Every At risk or Failing read must name a right knob, from 4 s
         * after the first one (advice may sharpen while evidence builds,
         * e.g. latency first, then bitrate once congestion shows) until the
         * end of the settle period, aftermath included. A lost connection
         * has no knob. */
        constexpr double advice_settle = 4.0; /* as the detection grace */
        double first_bad = -1;
        for (const auto &p : points)
        {
            if (p.t < onset || p.t > end || severity(p.verdict) < severity(AT_RISK) ||
                p.reason == ROBOTWEAX_SRT_TELEMETRY_REASON_CONNECTION_LOST ||
                p.reason == ROBOTWEAX_SRT_TELEMETRY_REASON_SOURCE_ERROR)
            {
                continue;
            }
            if (first_bad < 0)
            {
                first_bad = p.t;
            }
            if (p.t < first_bad + advice_settle)
            {
                continue;
            }
            result.advice_reads += 1;
            if (knobs.count(p.knob) == 0)
            {
                result.advice_wrong += 1;
                if (result.advice_wrong_at < 0)
                {
                    result.advice_wrong_at = p.t - onset;
                    result.wrong_knob = robotweax_srt_telemetry_health_knob_name(p.knob);
                }
            }
        }
        result.advice_judged = true;
        result.advice_right = result.advice_wrong == 0 && (result.advice_reads > 0 || knobs.count(top) != 0);
    }
    if (control)
    {
        /* The control must be OK throughout, with no UNKNOWN once warm. */
        result.pass = result.worst == OK && !result.saw_unknown;
    }
    else
    {
        result.pass = expected.count(result.worst) != 0 && !result.wrong_ok;
    }
    return result;
}

/* -------------------------------------------------------------- execution */

struct Outcome
{
    std::string name;
    bool setup_ok = false;
    bool control = false;
    RoleScore sender;
    RoleScore receiver;
    std::set<Verdict> expect_sender;
    std::set<Verdict> expect_receiver;
    std::uint64_t received = 0;
};

bool set_int(const SRTSOCKET socket, const SRT_SOCKOPT option, const int value)
{
    return srt_setsockflag(socket, option, &value, sizeof(value)) != SRT_ERROR;
}

Outcome run_scenario(const Scenario &scenario, const bool timeline, const bool trace_metrics)
{
    Trace trace;
    trace.enabled = trace_metrics;
    Outcome outcome;
    outcome.name = scenario.name;
    outcome.control = scenario.control;
    outcome.expect_sender = scenario.expect_sender;
    outcome.expect_receiver = scenario.expect_receiver;

    const SRTSOCKET listener = srt_create_socket();
    const SRTSOCKET caller = srt_create_socket();
    SRTSOCKET accepted = SRT_INVALID_SOCK;
    const auto cleanup_sockets = [&]
    {
        for (const auto s : {caller, accepted, listener})
        {
            if (s != SRT_INVALID_SOCK)
            {
                srt_close(s);
            }
        }
    };
    sockaddr_in listen_address{};
    listen_address.sin_family = AF_INET;
    listen_address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t address_size = sizeof(listen_address);
    const int timeout_ms = 200;
    bool ok = (scenario.peer_idle_timeout_ms == 0 ||
               (set_int(listener, SRTO_PEERIDLETIMEO, scenario.peer_idle_timeout_ms) &&
                set_int(caller, SRTO_PEERIDLETIMEO, scenario.peer_idle_timeout_ms))) &&
              set_int(listener, SRTO_LATENCY, scenario.latency_ms) &&
              set_int(caller, SRTO_LATENCY, scenario.latency_ms) && set_int(caller, SRTO_CONNTIMEO, 3000) &&
              set_int(caller, SRTO_SNDTIMEO, timeout_ms);
    if (ok && scenario.small_receive_buffer)
    {
        ok = set_int(listener, SRTO_RCVBUF, 3'000'000) && set_int(listener, SRTO_FC, 2'000);
    }
    ok = ok && srt_bind(listener, reinterpret_cast<sockaddr *>(&listen_address), sizeof(listen_address)) != SRT_ERROR &&
         srt_getsockname(listener, reinterpret_cast<sockaddr *>(&listen_address),
                         reinterpret_cast<int *>(&address_size)) != SRT_ERROR &&
         srt_listen(listener, 1) != SRT_ERROR;
    Relay relay;
    ok = ok && relay.start(listen_address);
    if (!ok)
    {
        std::cerr << scenario.name << ": setup failed: " << srt_getlasterror_str() << '\n';
        cleanup_sockets();
        return outcome;
    }
    relay.set(scenario.base);
    const auto &relay_address = relay.address();
    if (srt_connect(caller, reinterpret_cast<const sockaddr *>(&relay_address), sizeof(relay_address)) ==
        SRT_ERROR)
    {
        std::cerr << scenario.name << ": connect failed: " << srt_getlasterror_str() << '\n';
        cleanup_sockets();
        return outcome;
    }
    accepted = srt_accept(listener, nullptr, nullptr);
    if (accepted == SRT_INVALID_SOCK || !set_int(accepted, SRTO_RCVTIMEO, timeout_ms))
    {
        std::cerr << scenario.name << ": accept failed\n";
        cleanup_sockets();
        return outcome;
    }

    Session sender_session;
    Session receiver_session;
    Monitor sender_monitor;
    Monitor receiver_monitor;
    robotweax_srt_telemetry_sampler_config_v1 config{};
    robotweax_srt_telemetry_sampler_config_default(&config);
    config.callbacks.user_data = &trace;
    config.callbacks.on_window = &on_window;
    robotweax_srt_telemetry_sampler *sampler = nullptr;
    std::uint64_t sender_id = 0;
    std::uint64_t receiver_id = 0;
    robotweax_srt_telemetry_source_config_v1 sender_source{};
    robotweax_srt_telemetry_source_config_v1 receiver_source{};
    ok = create_session(caller, sender_session) && create_session(accepted, receiver_session) &&
         create_monitor(caller, sender_monitor) && create_monitor(accepted, receiver_monitor) &&
         robotweax_srt_telemetry_sampler_create(&config, &sampler) == ROBOTWEAX_SRT_TELEMETRY_OK &&
         robotweax_srt_telemetry_sampler_enable_health(sampler, 1) == ROBOTWEAX_SRT_TELEMETRY_OK;
    if (ok)
    {
        robotweax_srt_telemetry_public_srt_source_config(sender_session.value,
                                                         ROBOTWEAX_SRT_TELEMETRY_ENDPOINT_ROLE_SENDER, nullptr, 0,
                                                         &sender_source);
        robotweax_srt_telemetry_public_srt_source_config(receiver_session.value,
                                                         ROBOTWEAX_SRT_TELEMETRY_ENDPOINT_ROLE_RECEIVER, nullptr, 0,
                                                         &receiver_source);
        ok = robotweax_srt_telemetry_sampler_add_source(sampler, &sender_source, &sender_id) ==
                 ROBOTWEAX_SRT_TELEMETRY_OK &&
             robotweax_srt_telemetry_sampler_add_source(sampler, &receiver_source, &receiver_id) ==
                 ROBOTWEAX_SRT_TELEMETRY_OK;
        {
            std::lock_guard lock(trace.mutex);
            trace.sender_id = sender_id;
            trace.receiver_id = receiver_id;
            trace.start = Clock::now();
        }
        ok = ok && robotweax_srt_telemetry_sampler_start(sampler) == ROBOTWEAX_SRT_TELEMETRY_OK;
    }
    if (!ok)
    {
        std::cerr << scenario.name << ": telemetry setup failed\n";
        robotweax_srt_telemetry_sampler_destroy(sampler);
        cleanup_sockets();
        return outcome;
    }
    outcome.setup_ok = true;

    std::atomic<bool> streaming{true};
    std::atomic<bool> stalled{false};
    std::atomic<std::uint64_t> received{0};
    std::thread sender_thread(
        [&]
        {
            std::array<char, payload_bytes> payload{};
            payload.fill('x');
            const auto start = Clock::now();
            const auto period = std::chrono::nanoseconds(1'000'000'000 / scenario.packets_per_second);
            for (std::int64_t index = 0; streaming; ++index)
            {
                std::this_thread::sleep_until(start + period * index);
                if (srt_sendmsg(caller, payload.data(), payload_bytes, -1, 1) == SRT_ERROR)
                {
                    const auto state = srt_getsockstate(caller);
                    if (state != SRTS_CONNECTED)
                    {
                        while (streaming)
                        {
                            std::this_thread::sleep_for(std::chrono::milliseconds(20));
                        }
                        return;
                    }
                }
            }
        });
    std::thread reader_thread(
        [&]
        {
            std::array<char, 1500> buffer{};
            while (streaming)
            {
                if (stalled)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    continue;
                }
                if (srt_recvmsg(accepted, buffer.data(), static_cast<int>(buffer.size())) > 0)
                {
                    received.fetch_add(1);
                }
                else if (srt_getsockstate(accepted) != SRTS_CONNECTED)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                }
            }
        });

    constexpr double warmup = 6.0;
    const double onset = warmup;
    const double fault_end = onset + scenario.fault_seconds;
    const double end = fault_end + scenario.settle_seconds;
    std::vector<Point> sender_points;
    std::vector<Point> receiver_points;
    const auto start = Clock::now();
    {
        std::lock_guard lock(trace.mutex);
        trace.start = start;
    }
    bool fault_on = false;
    bool fault_off = false;
    auto next = start;
    while (seconds_since(start) < end)
    {
        const auto t = seconds_since(start);
        if (!fault_on && t >= onset)
        {
            fault_on = true;
            relay.set(scenario.fault);
            stalled = scenario.stall_receiver;
            if (scenario.sender_max_bandwidth > 0)
            {
                const std::int64_t value = scenario.sender_max_bandwidth;
                static_cast<void>(srt_setsockflag(caller, SRTO_MAXBW, &value, sizeof(value)));
            }
        }
        if (fault_on && !fault_off && !scenario.permanent && t >= fault_end)
        {
            fault_off = true;
            relay.set(scenario.base);
            stalled = false;
            if (scenario.sender_max_bandwidth > 0)
            {
                const std::int64_t value = -1;
                static_cast<void>(srt_setsockflag(caller, SRTO_MAXBW, &value, sizeof(value)));
            }
        }
        poll_monitor(sender_monitor);
        poll_monitor(receiver_monitor);
        static_cast<void>(
            robotweax_srt_telemetry_sampler_set_health_context(sampler, sender_id, &sender_monitor.context));
        static_cast<void>(
            robotweax_srt_telemetry_sampler_set_health_context(sampler, receiver_id, &receiver_monitor.context));
        robotweax_srt_telemetry_health_status_v1 status{};
        if (robotweax_srt_telemetry_sampler_health_read(sampler, sender_id, &status) == ROBOTWEAX_SRT_TELEMETRY_OK)
        {
            sender_points.push_back({t, status.verdict, status.reason, status.knob, status.suggested_value});
        }
        if (robotweax_srt_telemetry_sampler_health_read(sampler, receiver_id, &status) ==
            ROBOTWEAX_SRT_TELEMETRY_OK)
        {
            receiver_points.push_back({t, status.verdict, status.reason, status.knob, status.suggested_value});
        }
        next += std::chrono::milliseconds(100);
        std::this_thread::sleep_until(next);
    }

    streaming = false;
    sender_thread.join();
    reader_thread.join();
    static_cast<void>(robotweax_srt_telemetry_sampler_remove_source(sampler, sender_id));
    static_cast<void>(robotweax_srt_telemetry_sampler_remove_source(sampler, receiver_id));
    robotweax_srt_telemetry_sampler_destroy(sampler);
    outcome.received = received.load();

    outcome.sender =
        score(sender_points, scenario.expect_sender, scenario.knob_sender, onset, fault_end, end, scenario.control);
    outcome.receiver = score(receiver_points, scenario.expect_receiver, scenario.knob_receiver, onset, fault_end,
                             end, scenario.control);

    if (timeline)
    {
        const auto print = [&](const char *role, const std::vector<Point> &points)
        {
            std::cout << "  " << role << ':';
            Verdict last = static_cast<Verdict>(-1);
            robotweax_srt_telemetry_health_reason last_reason = ROBOTWEAX_SRT_TELEMETRY_REASON_NONE;
            robotweax_srt_telemetry_health_knob last_knob = ROBOTWEAX_SRT_TELEMETRY_KNOB_NONE;
            for (const auto &p : points)
            {
                if (p.verdict != last || p.reason != last_reason || p.knob != last_knob)
                {
                    std::cout << ' ' << std::fixed << std::setprecision(1) << p.t << "s=" << name(p.verdict) << '/'
                              << robotweax_srt_telemetry_health_reason_name(p.reason);
                    if (p.knob != ROBOTWEAX_SRT_TELEMETRY_KNOB_NONE)
                    {
                        std::cout << '/' << robotweax_srt_telemetry_health_knob_name(p.knob);
                    }
                    last = p.verdict;
                    last_reason = p.reason;
                    last_knob = p.knob;
                }
            }
            std::cout << '\n';
        };
        print("sender", sender_points);
        print("receiver", receiver_points);
    }
    cleanup_sockets();
    return outcome;
}

void print_role(const char *role, const RoleScore &s, const std::set<Verdict> &expected)
{
    std::cout << ",\"" << role << "\":{";
    if (!s.scored)
    {
        std::cout << "\"scored\":false}";
        return;
    }
    std::cout << "\"expected\":\"" << expect_text(expected) << "\",\"worst\":\"" << name(s.worst)
              << "\",\"reason\":\"" << s.worst_reason << "\",\"advice\":\"" << s.advice
              << "\",\"detectedAfter\":" << std::fixed << std::setprecision(1) << s.first_detection
              << ",\"knob\":\"" << s.knob << "\",\"adviceRight\":"
              << (s.advice_judged ? (s.advice_right ? "true" : "false") : "null");
    if (s.advice_judged)
    {
        std::cout << ",\"adviceReads\":" << s.advice_reads << ",\"adviceWrong\":" << s.advice_wrong;
        if (s.advice_wrong > 0)
        {
            std::cout << ",\"firstWrong\":{\"after\":" << std::setprecision(1) << s.advice_wrong_at
                      << ",\"knob\":\"" << s.wrong_knob << "\"}";
        }
    }
    std::cout
              << ",\"wrongOk\":" << (s.wrong_ok ? "true" : "false") << ",\"pass\":" << (s.pass ? "true" : "false")
              << '}';
}

} // namespace

int main(const int argc, char **argv)
{
    std::string only;
    bool timeline = false;
    bool trace_metrics = false;
    for (int i = 1; i < argc; ++i)
    {
        const std::string_view arg = argv[i];
        if (arg == "--scenario" && i + 1 < argc)
        {
            only = argv[++i];
        }
        else if (arg == "--timeline")
        {
            timeline = true;
        }
        else if (arg == "--trace")
        {
            timeline = true;
            trace_metrics = true;
        }
        else if (arg == "--list")
        {
            for (const auto &s : scenarios())
            {
                std::cout << s.name << ": " << s.description << '\n';
            }
            return EXIT_SUCCESS;
        }
        else
        {
            std::cerr << "usage: fault_lab [--scenario NAME] [--list] [--timeline] [--trace]\n";
            return EXIT_FAILURE;
        }
    }
    if (srt_startup() < 0)
    {
        std::cerr << "FAIL: srt_startup\n";
        return EXIT_FAILURE;
    }
    srt_setloglevel(LOG_ERR);

    std::vector<Outcome> outcomes;
    for (const auto &scenario : scenarios())
    {
        if (!only.empty() && scenario.name != only)
        {
            continue;
        }
        auto outcome = run_scenario(scenario, timeline, trace_metrics);
        std::cout << "SCENARIO {\"provider\":\"" << provider_name << "\",\"name\":\"" << outcome.name
                  << "\",\"setup\":" << (outcome.setup_ok ? "true" : "false") << ",\"received\":" << outcome.received;
        print_role("sender", outcome.sender, outcome.expect_sender);
        print_role("receiver", outcome.receiver, outcome.expect_receiver);
        std::cout << "}\n" << std::flush;
        outcomes.push_back(std::move(outcome));
    }
    srt_cleanup();

    struct Tally
    {
        int faults = 0;
        int right = 0;
        int wrong_ok = 0;
        int advice_judged = 0;
        int advice_right = 0;
        bool control_ok = true;
        bool control_seen = false;
    };
    Tally sender;
    Tally receiver;
    bool setup_ok = true;
    for (const auto &o : outcomes)
    {
        setup_ok = setup_ok && o.setup_ok;
        for (auto [tally, role] : {std::pair{&sender, &o.sender}, std::pair{&receiver, &o.receiver}})
        {
            if (!role->scored)
            {
                continue;
            }
            if (o.control)
            {
                tally->control_seen = true;
                tally->control_ok = tally->control_ok && role->pass;
                continue;
            }
            tally->faults += 1;
            tally->right += role->pass ? 1 : 0;
            tally->wrong_ok += role->wrong_ok ? 1 : 0;
            tally->advice_judged += role->advice_judged ? 1 : 0;
            tally->advice_right += role->advice_right ? 1 : 0;
        }
    }
    const auto gate = [](const Tally &t)
    {
        /* At least 9 of 10: 90 % of the fault classes, rounded up; and
         * every judged fault class names a right knob on every read. */
        const int needed = (t.faults * 9 + 9) / 10;
        return t.right >= needed && t.wrong_ok == 0 && t.control_ok && t.advice_right == t.advice_judged;
    };
    const bool full = only.empty();
    const bool passed = setup_ok && (!full || (gate(sender) && gate(receiver)));
    std::cout << "GATE {\"provider\":\"" << provider_name << "\",\"sender\":{\"right\":" << sender.right
              << ",\"faults\":" << sender.faults << ",\"wrongOk\":" << sender.wrong_ok
              << ",\"controlOk\":" << (sender.control_ok ? "true" : "false") << ",\"advice\":\""
              << sender.advice_right << '/' << sender.advice_judged << "\"},\"receiver\":{\"right\":"
              << receiver.right << ",\"faults\":" << receiver.faults << ",\"wrongOk\":" << receiver.wrong_ok
              << ",\"controlOk\":" << (receiver.control_ok ? "true" : "false") << ",\"advice\":\""
              << receiver.advice_right << '/' << receiver.advice_judged << "\"},\"pass\":" << (passed ? "true" : "false") << "}\n";
    return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
