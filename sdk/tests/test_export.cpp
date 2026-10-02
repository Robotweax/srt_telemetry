#include <robotweax/srt_telemetry_sampler.h>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <chrono>
#include <thread>
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
constexpr std::uint64_t base_unix = UINT64_C(1'800'000'000) * second;

struct Fake
{
    bool receiver = false;
    std::uint64_t packets = 0;
    std::uint64_t loss = 0;
    /* Connection age at the first sample, in s; 0: not reported. */
    double age = 0;
    robotweax_srt_telemetry_delta_engine *engine = nullptr;
};

robotweax_srt_telemetry_error fake_sample(void *data, const std::uint64_t unix_nano, const std::uint64_t monotonic_nano,
                                          robotweax_srt_telemetry_source_output_v1 *output)
{
    auto &f = *static_cast<Fake *>(data);
    f.packets += 30;
    f.loss += 1;
    auto &raw = output->raw;
    raw.provider = ROBOTWEAX_SRT_TELEMETRY_PROVIDER_ROBOTWEAX_SRT;
    raw.observed_at_unix_nano = unix_nano;
    raw.sample_monotonic_nano = monotonic_nano;
    raw.rtt_seconds = 0.02;
    if (f.age > 0)
    {
        output->has_ext = 1;
        output->ext.struct_size = sizeof(output->ext);
        output->ext.abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
        output->ext.available_fields = ROBOTWEAX_SRT_TELEMETRY_EXT_FIELD_CONNECTION_AGE_SECONDS;
        output->ext.connection_age_seconds = f.age + static_cast<double>(f.packets / 30 - 1) / 10;
    }
    if (f.receiver)
    {
        raw.available_fields = ROBOTWEAX_SRT_TELEMETRY_FIELD_UNIQUE_RECEIVED_PACKET_TOTAL |
                               ROBOTWEAX_SRT_TELEMETRY_FIELD_UNIQUE_RECEIVED_WIRE_BYTE_TOTAL |
                               ROBOTWEAX_SRT_TELEMETRY_FIELD_RECEIVER_LOSS_PACKET_TOTAL |
                               ROBOTWEAX_SRT_TELEMETRY_FIELD_RECEIVER_DROP_PACKET_TOTAL |
                               ROBOTWEAX_SRT_TELEMETRY_FIELD_RTT_SECONDS |
                               ROBOTWEAX_SRT_TELEMETRY_FIELD_RECEIVE_BUFFER_SECONDS |
                               ROBOTWEAX_SRT_TELEMETRY_FIELD_RECEIVER_TSBPD_DELAY_SECONDS;
        raw.unique_received_packet_total = f.packets;
        raw.unique_received_wire_byte_total = f.packets * 1316;
        raw.receiver_loss_packet_total = f.loss;
        raw.receive_buffer_seconds = 0.118;
        raw.receiver_tsbpd_delay_seconds = 0.12;
    }
    else
    {
        raw.available_fields = ROBOTWEAX_SRT_TELEMETRY_FIELD_UNIQUE_SENT_PACKET_TOTAL |
                               ROBOTWEAX_SRT_TELEMETRY_FIELD_UNIQUE_SENT_WIRE_BYTE_TOTAL |
                               ROBOTWEAX_SRT_TELEMETRY_FIELD_RETRANSMITTED_PACKET_TOTAL |
                               ROBOTWEAX_SRT_TELEMETRY_FIELD_SENDER_DROP_PACKET_TOTAL |
                               ROBOTWEAX_SRT_TELEMETRY_FIELD_RTT_SECONDS;
        raw.unique_sent_packet_total = f.packets;
        raw.unique_sent_wire_byte_total = f.packets * 1316;
        raw.retransmitted_packet_total = f.loss;
    }
    return robotweax_srt_telemetry_delta_engine_update(f.engine, &raw, &output->delta);
}

/* ------------------------------------------------- Prometheus validation */

struct Exposition
{
    std::map<std::string, std::string> types; /* family -> type */
    std::set<std::string> series;             /* name{labels} */
    std::vector<std::string> lines;
    bool valid = true;
    std::string error;
};

Exposition parse_prometheus(const std::string &text)
{
    Exposition e;
    static const std::regex help(R"(^# HELP ([a-zA-Z_:][a-zA-Z0-9_:]*) .+$)");
    static const std::regex type(R"(^# TYPE ([a-zA-Z_:][a-zA-Z0-9_:]*) (gauge|counter)$)");
    static const std::regex sample(
        R"(^([a-zA-Z_:][a-zA-Z0-9_:]*)\{([a-zA-Z_][a-zA-Z0-9_]*="(?:[^"\\\n]|\\["\\n])*"(?:,[a-zA-Z_][a-zA-Z0-9_]*="(?:[^"\\\n]|\\["\\n])*")*)\} (-?[0-9.]+(?:e[-+]?[0-9]+)?)$)");
    std::istringstream in(text);
    std::string line;
    std::set<std::string> helped;
    while (std::getline(in, line))
    {
        e.lines.push_back(line);
        std::smatch m;
        if (std::regex_match(line, m, help))
        {
            if (!helped.insert(m[1]).second)
            {
                e.valid = false;
                e.error = "duplicate HELP " + m[1].str();
            }
        }
        else if (std::regex_match(line, m, type))
        {
            if (e.types.count(m[1]) != 0)
            {
                e.valid = false;
                e.error = "duplicate TYPE " + m[1].str();
            }
            e.types[m[1]] = m[2];
        }
        else if (std::regex_match(line, m, sample))
        {
            if (e.types.count(m[1]) == 0)
            {
                e.valid = false;
                e.error = "sample before TYPE: " + line;
            }
            if (!e.series.insert(m[1].str() + "{" + m[2].str() + "}").second)
            {
                e.valid = false;
                e.error = "duplicate series: " + line;
            }
            if (e.types[m[1]] == "counter" && m[1].str().size() < 6)
            {
                e.valid = false;
            }
        }
        else
        {
            e.valid = false;
            e.error = "bad line: " + line;
        }
    }
    return e;
}

bool has_line(const Exposition &e, const std::string &prefix)
{
    for (const auto &l : e.lines)
    {
        if (l.rfind(prefix, 0) == 0)
        {
            return true;
        }
    }
    return false;
}

bool any_line_contains(const Exposition &e, const std::string &a, const std::string &b)
{
    for (const auto &l : e.lines)
    {
        if (l.find(a) != std::string::npos && l.find(b) != std::string::npos)
        {
            return true;
        }
    }
    return false;
}

/* ------------------------------------------------------- JSON validation */

struct Json
{
    const std::string &s;
    std::size_t i = 0;
    bool ok = true;

    void ws()
    {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\n' || s[i] == '\t' || s[i] == '\r'))
        {
            ++i;
        }
    }
    bool lit(const char *t)
    {
        const auto n = std::strlen(t);
        if (s.compare(i, n, t) == 0)
        {
            i += n;
            return true;
        }
        return false;
    }
    void string()
    {
        if (i >= s.size() || s[i] != '"')
        {
            ok = false;
            return;
        }
        ++i;
        while (i < s.size() && s[i] != '"')
        {
            if (static_cast<unsigned char>(s[i]) < 0x20)
            {
                ok = false;
                return;
            }
            if (s[i] == '\\')
            {
                ++i;
                if (i < s.size() && s[i] == 'u')
                {
                    i += 4;
                }
            }
            ++i;
        }
        if (i >= s.size())
        {
            ok = false;
            return;
        }
        ++i;
    }
    void value()
    {
        ws();
        if (!ok || i >= s.size())
        {
            ok = false;
            return;
        }
        const char c = s[i];
        if (c == '{')
        {
            ++i;
            ws();
            if (s[i] == '}')
            {
                ++i;
                return;
            }
            while (ok)
            {
                ws();
                string();
                ws();
                if (s[i] != ':')
                {
                    ok = false;
                    return;
                }
                ++i;
                value();
                ws();
                if (s[i] == ',')
                {
                    ++i;
                    continue;
                }
                if (s[i] == '}')
                {
                    ++i;
                    return;
                }
                ok = false;
            }
        }
        else if (c == '[')
        {
            ++i;
            ws();
            if (s[i] == ']')
            {
                ++i;
                return;
            }
            while (ok)
            {
                value();
                ws();
                if (s[i] == ',')
                {
                    ++i;
                    continue;
                }
                if (s[i] == ']')
                {
                    ++i;
                    return;
                }
                ok = false;
            }
        }
        else if (c == '"')
        {
            string();
        }
        else if (lit("true") || lit("false") || lit("null"))
        {
        }
        else
        {
            const auto start = i;
            while (i < s.size() && (std::isdigit(static_cast<unsigned char>(s[i])) || s[i] == '-' || s[i] == '+' ||
                                    s[i] == '.' || s[i] == 'e' || s[i] == 'E'))
            {
                ++i;
            }
            ok = ok && i > start;
        }
    }
};

bool valid_json(const std::string &text)
{
    Json j{text};
    j.value();
    j.ws();
    return j.ok && j.i == text.size();
}

std::string render(robotweax_srt_telemetry_sampler *sampler, const robotweax_srt_telemetry_render_format format,
                   const std::uint32_t flags = 0, const std::uint64_t window = 0)
{
    robotweax_srt_telemetry_render_options_v1 options;
    robotweax_srt_telemetry_render_options_default(format, &options);
    options.flags = flags;
    options.window_nano = window;
    std::string out;
    const auto error = robotweax_srt_telemetry_sampler_render_to(
        sampler, &options,
        [](void *user, const char *data, const std::size_t size)
        {
            static_cast<std::string *>(user)->append(data, size);
            return 0;
        },
        &out);
    check(error == ROBOTWEAX_SRT_TELEMETRY_OK, "render_to succeeds");
    return out;
}

void test_names()
{
    std::set<std::string> otel;
    std::set<std::string> prom;
    static const std::regex prom_name(R"(^srt_(sender|receiver|connection)_[a-z0-9_]+$)");
    static const std::regex otel_name(R"(^srt\.(sender|receiver|connection)\.[a-z0-9_.]+$)");
    for (int m = 0; m < ROBOTWEAX_SRT_TELEMETRY_METRIC_DEFINED_COUNT; ++m)
    {
        const auto metric = static_cast<robotweax_srt_telemetry_metric>(m);
        const std::string o = robotweax_srt_telemetry_metric_otel_name(metric);
        const std::string p = robotweax_srt_telemetry_metric_prometheus_name(metric);
        const std::string scope = robotweax_srt_telemetry_metric_scope(metric);
        const auto *info = robotweax_srt_telemetry_metric_info(metric);
        check(otel.insert(o).second && prom.insert(p).second, "names are unique: " + o);
        check(std::regex_match(p, prom_name) && std::regex_match(o, otel_name), "name syntax: " + p);
        const auto expected_scope = info->role == ROBOTWEAX_SRT_TELEMETRY_ENDPOINT_ROLE_SENDER     ? "sender"
                                    : info->role == ROBOTWEAX_SRT_TELEMETRY_ENDPOINT_ROLE_RECEIVER ? "receiver"
                                                                                                   : "connection";
        check(scope == expected_scope, "scope follows the metric's role: " + o);
        check(o.rfind("srt." + scope + ".", 0) == 0 && p.rfind("srt_" + scope + "_", 0) == 0,
              "names carry the scope: " + o);
        /* Prometheus base-unit suffixes */
        const std::string unit = info->unit;
        const auto ends = [&](const char *suffix) { return p.size() > std::strlen(suffix) && p.ends_with(suffix); };
        const bool suffix_ok = (unit == "bit/s" && ends("_bits_per_second")) ||
                               (unit == "{packet}/s" && ends("_packets_per_second")) ||
                               (unit == "s" && ends("_seconds")) || (unit == "{packet}" && ends("_packets")) ||
                               (unit == "By" && ends("_bytes")) || (unit == "1" && ends("_ratio"));
        check(suffix_ok, "Prometheus name carries the unit: " + p);
    }
    check(robotweax_srt_telemetry_metric_otel_name(static_cast<robotweax_srt_telemetry_metric>(99)) == nullptr,
          "unknown metric has no name");
#ifdef ROBOTWEAX_SRT_TELEMETRY_NAMES_DOC
    /* The published table lists every name. */
    std::ifstream file(ROBOTWEAX_SRT_TELEMETRY_NAMES_DOC);
    std::stringstream doc;
    doc << file.rdbuf();
    for (const auto &name : otel)
    {
        check(doc.str().find("`" + name + "`") != std::string::npos, "metric-names.md lists " + name);
    }
    for (const auto &name : prom)
    {
        check(doc.str().find("`" + name + "`") != std::string::npos, "metric-names.md lists " + name);
    }
#endif
}

void test_render()
{
    robotweax_srt_telemetry_sampler_config_v1 config{};
    robotweax_srt_telemetry_sampler_config_default(&config);
    robotweax_srt_telemetry_sampler *sampler = nullptr;
    static_cast<void>(robotweax_srt_telemetry_sampler_create(&config, &sampler));
    static_cast<void>(robotweax_srt_telemetry_sampler_enable_health(sampler, 1));

    Fake sender;
    Fake receiver;
    receiver.receiver = true;
    static_cast<void>(robotweax_srt_telemetry_delta_engine_create(&sender.engine));
    static_cast<void>(robotweax_srt_telemetry_delta_engine_create(&receiver.engine));
    const robotweax_srt_telemetry_label_v1 sender_labels[] = {
        {"site", "studio \"2\" \\ west"}, {"role", "uplink"}, {"host_role", "backup"}};
    const robotweax_srt_telemetry_label_v1 receiver_labels[] = {{"site", "studio-2"}};
    robotweax_srt_telemetry_source_config_v1 cfg{};
    cfg.struct_size = sizeof(cfg);
    cfg.abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
    cfg.sample = &fake_sample;
    cfg.source_data = &sender;
    cfg.role = ROBOTWEAX_SRT_TELEMETRY_ENDPOINT_ROLE_SENDER;
    cfg.labels = sender_labels;
    cfg.label_count = 3;
    std::uint64_t sender_id = 0;
    std::uint64_t receiver_id = 0;
    static_cast<void>(robotweax_srt_telemetry_sampler_add_source(sampler, &cfg, &sender_id));
    cfg.source_data = &receiver;
    cfg.role = ROBOTWEAX_SRT_TELEMETRY_ENDPOINT_ROLE_RECEIVER;
    cfg.labels = receiver_labels;
    cfg.label_count = 1;
    static_cast<void>(robotweax_srt_telemetry_sampler_add_source(sampler, &cfg, &receiver_id));

    /* Before any window: valid output, sources up, no window metrics. */
    {
        const auto e = parse_prometheus(render(sampler, ROBOTWEAX_SRT_TELEMETRY_RENDER_PROMETHEUS));
        check(e.valid, "empty exposition is valid: " + e.error);
        check(!has_line(e, "srt_receiver_recovery_headroom_ratio{"), "no window metrics before a window closes");
        check(valid_json(render(sampler, ROBOTWEAX_SRT_TELEMETRY_RENDER_JSON)), "empty snapshot is valid JSON");
    }

    for (std::uint64_t i = 0; i < 125; ++i)
    {
        static_cast<void>(
            robotweax_srt_telemetry_sampler_tick(sampler, base_unix + i * second / 10, second + i * second / 10));
    }

    std::uint64_t ids[4]{};
    std::size_t count = 0;
    check(robotweax_srt_telemetry_sampler_source_ids(sampler, ids, 4, &count) == ROBOTWEAX_SRT_TELEMETRY_OK &&
              count == 2 && ids[0] == sender_id && ids[1] == receiver_id,
          "source ids");
    check(robotweax_srt_telemetry_sampler_source_ids(sampler, ids, 1, &count) ==
                  ROBOTWEAX_SRT_TELEMETRY_BUFFER_TOO_SMALL &&
              count == 1,
          "source ids: too small");
    robotweax_srt_telemetry_window_summary_v1 window{};
    check(robotweax_srt_telemetry_sampler_window_read(sampler, receiver_id, 0, &window) == ROBOTWEAX_SRT_TELEMETRY_OK &&
              window.window_nano == 10 * second && window.start_unix_nano == base_unix,
          "default window is the latest closed 10 s window");
    check(robotweax_srt_telemetry_sampler_window_read(sampler, receiver_id, second, &window) ==
                  ROBOTWEAX_SRT_TELEMETRY_OK &&
              window.start_unix_nano == base_unix + 11 * second,
          "1 s window");
    check(robotweax_srt_telemetry_sampler_window_read(sampler, receiver_id, 3 * second, &window) ==
              ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT,
          "unconfigured window length");
    check(robotweax_srt_telemetry_sampler_window_read(sampler, 99, 0, &window) == ROBOTWEAX_SRT_TELEMETRY_NOT_FOUND,
          "unknown source");
    robotweax_srt_telemetry_metric_sample_v1 latest{};
    check(robotweax_srt_telemetry_sampler_latest_metrics(sampler, receiver_id, &latest) == ROBOTWEAX_SRT_TELEMETRY_OK &&
              latest.observed_at_unix_nano == base_unix + 124 * second / 10,
          "latest metrics");

    const auto text = render(sampler, ROBOTWEAX_SRT_TELEMETRY_RENDER_PROMETHEUS);
    auto e = parse_prometheus(text);
    check(e.valid, "exposition is valid: " + e.error);
    check(e.types["srt_receiver_recovery_headroom_ratio"] == "gauge" &&
              e.types["srt_receiver_lost_packets_total"] == "counter" &&
              e.types["srt_sender_unique_sent_packets_total"] == "counter",
          "families and types");
    check(any_line_contains(e, "srt_receiver_recovery_headroom_ratio{source_id=\"2\",role=\"receiver\",site=\"studio-2\"",
                            "stat=\"min\"} 0.98"),
          "gauge window statistics with host labels");
    check(any_line_contains(e, "srt_receiver_unique_received_packets_per_second{source_id=\"2\"", "stat=\"rate\"} 300"),
          "exact window rate");
    check(any_line_contains(e, "srt_receiver_unique_received_packets_total{source_id=\"2\"", "} 3750"),
          "lifetime counter");
    check(any_line_contains(e, "site=\"studio \\\"2\\\" \\\\ west\"", "srt_source_up"), "label values escaped");
    check(any_line_contains(e, "host_host_role=\"uplink\",host_role=\"backup\"", "role=\"sender\""),
          "reserved host label renamed past a colliding host label");
    check(!any_line_contains(e, "srt_receiver_", "role=\"sender\""), "a sender exports no receiver metrics");
    check(any_line_contains(e, "srt_connection_rtt_seconds{source_id=\"1\"", "stat=\"max\"} 0.02") &&
              any_line_contains(e, "srt_connection_rtt_seconds{source_id=\"2\"", "stat=\"max\"} 0.02"),
          "connection metrics for both roles");
    check(any_line_contains(e, "srt_health_verdict{source_id=\"2\"", "} ") &&
              any_line_contains(e, "srt_health_info{source_id=\"2\"", "verdict=\"") &&
              any_line_contains(e, "srt_health_indicator_state{source_id=\"2\"", "indicator=\"unrecovered_ratio\""),
          "health series");
    check(has_line(e, "srt_telemetry_schema_info{version=\"1\"} 1"), "schema version");
    check(any_line_contains(e, "srt_window_quality{source_id=\"1\"", "flag=\"partial\"} 0"), "window quality flags");

    const auto lean = parse_prometheus(render(sampler, ROBOTWEAX_SRT_TELEMETRY_RENDER_PROMETHEUS,
                                              ROBOTWEAX_SRT_TELEMETRY_RENDER_NO_COUNTERS |
                                                  ROBOTWEAX_SRT_TELEMETRY_RENDER_NO_INDICATORS));
    check(lean.valid && lean.types.count("srt_receiver_lost_packets_total") == 0 &&
              lean.types.count("srt_health_indicator_state") == 0 && lean.types.count("srt_health_verdict") == 1,
          "flags leave out counters and indicators");
    const auto one_second = parse_prometheus(render(sampler, ROBOTWEAX_SRT_TELEMETRY_RENDER_PROMETHEUS, 0, second));
    check(one_second.valid && has_line(one_second, "srt_window_seconds{source_id=\"1\",role=\"sender\",site=") &&
              any_line_contains(one_second, "srt_window_seconds", "} 1"),
          "explicit 1 s window");

    const auto json = render(sampler, ROBOTWEAX_SRT_TELEMETRY_RENDER_JSON);
    check(valid_json(json), "snapshot is valid JSON");
    check(json.find("\"generated_unix_ms\":1800000012400") != std::string::npos &&
              json.find("\"suggested_value\":null") != std::string::npos &&
              json.find("\"verdict_age_seconds\":") != std::string::npos,
          "snapshot times in ms, verdict age, no advice value without a knob");
    check(json.find("\"schema\":\"srt-telemetry.snapshot.v1\"") != std::string::npos &&
              json.find("\"srt.receiver.recovery_headroom\":{\"unit\":\"1\"") != std::string::npos &&
              json.find("\"srt.receiver.unique_received.packets\":3750") != std::string::npos &&
              json.find("\"health\":{\"verdict\":") != std::string::npos &&
              json.find("\"site\":\"studio \\\"2\\\" \\\\ west\"") != std::string::npos &&
              json.find("\"role\":\"uplink\"") != std::string::npos,
          "snapshot content");

    /* Buffer variant: size query, too small, exact. */
    robotweax_srt_telemetry_render_options_v1 options;
    robotweax_srt_telemetry_render_options_default(ROBOTWEAX_SRT_TELEMETRY_RENDER_PROMETHEUS, &options);
    std::size_t length = 0;
    check(robotweax_srt_telemetry_sampler_render(sampler, &options, nullptr, 0, &length) ==
                  ROBOTWEAX_SRT_TELEMETRY_BUFFER_TOO_SMALL &&
              length == text.size(),
          "size query");
    std::vector<char> buffer(length + 1);
    check(robotweax_srt_telemetry_sampler_render(sampler, &options, buffer.data(), buffer.size(), &length) ==
                  ROBOTWEAX_SRT_TELEMETRY_OK &&
              std::string(buffer.data()) == text,
          "render into a buffer");
    check(robotweax_srt_telemetry_sampler_render(sampler, &options, buffer.data(), length, &length) ==
              ROBOTWEAX_SRT_TELEMETRY_BUFFER_TOO_SMALL,
          "no room for the terminator");
    options.format = static_cast<robotweax_srt_telemetry_render_format>(7);
    check(robotweax_srt_telemetry_sampler_render(sampler, &options, buffer.data(), buffer.size(), &length) ==
              ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT,
          "unknown format");
    options.format = ROBOTWEAX_SRT_TELEMETRY_RENDER_JSON;
    options.window_nano = 7 * second;
    check(robotweax_srt_telemetry_sampler_render(sampler, &options, buffer.data(), buffer.size(), &length) ==
              ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT,
          "unconfigured window");

    /* The writer runs without the sampler lock: it may call the sampler. */
    struct Reentry
    {
        robotweax_srt_telemetry_sampler *sampler;
        robotweax_srt_telemetry_error inner = ROBOTWEAX_SRT_TELEMETRY_INVALID_STATE;
    } reentry{sampler};
    robotweax_srt_telemetry_render_options_default(ROBOTWEAX_SRT_TELEMETRY_RENDER_JSON, &options);
    static_cast<void>(robotweax_srt_telemetry_sampler_render_to(
        sampler, &options,
        [](void *user, const char *, std::size_t)
        {
            auto &r = *static_cast<Reentry *>(user);
            std::size_t n = 0;
            r.inner = robotweax_srt_telemetry_sampler_source_ids(r.sampler, nullptr, 0, &n);
            return 0;
        },
        &reentry));
    check(reentry.inner == ROBOTWEAX_SRT_TELEMETRY_BUFFER_TOO_SMALL, "writer may call the sampler");
    check(robotweax_srt_telemetry_sampler_render_to(
              sampler, &options, [](void *, const char *, std::size_t) { return 1; }, nullptr) ==
              ROBOTWEAX_SRT_TELEMETRY_SINK_FAILURE,
          "writer failure");

    /* Counters stay exact integers far above 2^53 / 1e9. */
    receiver.packets = UINT64_C(123'456'953'512);
    static_cast<void>(robotweax_srt_telemetry_sampler_tick(sampler, base_unix + 125 * second / 10, second + 125 * second / 10));
    {
        auto big = parse_prometheus(render(sampler, ROBOTWEAX_SRT_TELEMETRY_RENDER_PROMETHEUS));
        check(big.valid && any_line_contains(big, "srt_receiver_unique_received_packets_total{source_id=\"2\"",
                                             "} 123456953542"),
              "large counter printed exactly");
    }

    /* Health off: no health series, still valid. */
    static_cast<void>(robotweax_srt_telemetry_sampler_enable_health(sampler, 0));
    {
        auto plain = parse_prometheus(render(sampler, ROBOTWEAX_SRT_TELEMETRY_RENDER_PROMETHEUS));
        const auto plain_json = render(sampler, ROBOTWEAX_SRT_TELEMETRY_RENDER_JSON);
        check(plain.valid && plain.types.count("srt_health_verdict") == 0 && valid_json(plain_json) &&
                  plain_json.find("\"health\"") == std::string::npos,
              "health disabled: no health output");
    }

    /* Ticks stop: after two more window lengths the window is stale. */
    {
        const auto before = parse_prometheus(render(sampler, ROBOTWEAX_SRT_TELEMETRY_RENDER_PROMETHEUS, 0, second));
        check(has_line(before, "srt_window_seconds{source_id=\"2\""), "1 s window present while ticking");
        std::this_thread::sleep_for(std::chrono::milliseconds(3'200));
        const auto after = parse_prometheus(render(sampler, ROBOTWEAX_SRT_TELEMETRY_RENDER_PROMETHEUS, 0, second));
        check(after.valid && !has_line(after, "srt_window_seconds{") &&
                  !has_line(after, "srt_receiver_recovery_headroom_ratio{") &&
                  has_line(after, "srt_receiver_unique_received_packets_total{"),
              "stale window left out, counters kept");
    }

    robotweax_srt_telemetry_sampler_destroy(sampler);
    robotweax_srt_telemetry_delta_engine_destroy(sender.engine);
    robotweax_srt_telemetry_delta_engine_destroy(receiver.engine);

    /* No sources at all. */
    robotweax_srt_telemetry_sampler *empty = nullptr;
    static_cast<void>(robotweax_srt_telemetry_sampler_create(&config, &empty));
    {
        auto e0 = parse_prometheus(render(empty, ROBOTWEAX_SRT_TELEMETRY_RENDER_PROMETHEUS));
        const auto j0 = render(empty, ROBOTWEAX_SRT_TELEMETRY_RENDER_JSON);
        check(e0.valid && valid_json(j0) && j0.find("\"sources\":[]") != std::string::npos,
              "no sources: valid empty output");
    }
    robotweax_srt_telemetry_sampler_destroy(empty);
}

std::string render_otlp(robotweax_srt_telemetry_sampler *sampler, const robotweax_srt_telemetry_label_v1 *resource,
                        const std::size_t count, robotweax_srt_telemetry_error *error = nullptr)
{
    robotweax_srt_telemetry_render_options_v1 options;
    robotweax_srt_telemetry_render_options_default(ROBOTWEAX_SRT_TELEMETRY_RENDER_OTLP_JSON, &options);
    std::string out;
    const auto e = robotweax_srt_telemetry_sampler_render_otlp_to(
        sampler, &options, resource, count,
        [](void *user, const char *data, const std::size_t size)
        {
            static_cast<std::string *>(user)->append(data, size);
            return 0;
        },
        &out);
    if (error != nullptr)
    {
        *error = e;
    }
    return out;
}

/* Builds a sampler with a sender and a receiver after 12.5 s of samples. */
robotweax_srt_telemetry_sampler *otlp_sampler(Fake &sender, Fake &receiver)
{
    robotweax_srt_telemetry_sampler_config_v1 config{};
    robotweax_srt_telemetry_sampler_config_default(&config);
    robotweax_srt_telemetry_sampler *sampler = nullptr;
    static_cast<void>(robotweax_srt_telemetry_sampler_create(&config, &sampler));
    static_cast<void>(robotweax_srt_telemetry_sampler_enable_health(sampler, 1));
    receiver.receiver = true;
    static_cast<void>(robotweax_srt_telemetry_delta_engine_create(&sender.engine));
    static_cast<void>(robotweax_srt_telemetry_delta_engine_create(&receiver.engine));
    static const robotweax_srt_telemetry_label_v1 sender_labels[] = {
        {"site", "studio \"2\" \\ west"}, {"role", "uplink"}, {"host_role", "backup"}};
    static const robotweax_srt_telemetry_label_v1 receiver_labels[] = {{"site", "studio-2"}};
    robotweax_srt_telemetry_source_config_v1 cfg{};
    cfg.struct_size = sizeof(cfg);
    cfg.abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
    cfg.sample = &fake_sample;
    cfg.source_data = &sender;
    cfg.role = ROBOTWEAX_SRT_TELEMETRY_ENDPOINT_ROLE_SENDER;
    cfg.labels = sender_labels;
    cfg.label_count = 3;
    std::uint64_t id = 0;
    static_cast<void>(robotweax_srt_telemetry_sampler_add_source(sampler, &cfg, &id));
    cfg.source_data = &receiver;
    cfg.role = ROBOTWEAX_SRT_TELEMETRY_ENDPOINT_ROLE_RECEIVER;
    cfg.labels = receiver_labels;
    cfg.label_count = 1;
    static_cast<void>(robotweax_srt_telemetry_sampler_add_source(sampler, &cfg, &id));
    for (std::uint64_t i = 0; i < 125; ++i)
    {
        static_cast<void>(
            robotweax_srt_telemetry_sampler_tick(sampler, base_unix + i * second / 10, second + i * second / 10));
    }
    return sampler;
}

void test_otlp()
{
    Fake sender;
    Fake receiver;
    sender.age = 3600;
    auto *sampler = otlp_sampler(sender, receiver);
    const auto plain = render(sampler, ROBOTWEAX_SRT_TELEMETRY_RENDER_OTLP_JSON);
    check(valid_json(plain), "OTLP: valid JSON");
    check(plain.rfind("{\"resourceMetrics\":[{\"resource\":{\"attributes\":[", 0) == 0,
          "OTLP: an ExportMetricsServiceRequest");
    check(plain.find("{\"key\":\"service.name\",\"value\":{\"stringValue\":\"srt-telemetry\"}}") != std::string::npos,
          "OTLP: default service.name");
    check(plain.find("\"scope\":{\"name\":\"robotweax.srt_telemetry\"") != std::string::npos &&
              plain.find("{\"key\":\"srt.metric_schema\",\"value\":{\"intValue\":\"1\"}}") != std::string::npos,
          "OTLP: scope and schema version");
    check(plain.find("\"name\":\"srt.receiver.unique_received.packets\",") != std::string::npos &&
              plain.find("\"sum\":{\"aggregationTemporality\":2,\"isMonotonic\":true,\"dataPoints\":[") !=
                  std::string::npos,
          "OTLP: counters are cumulative monotonic sums");
    /* The receiver's counter: 30 packets per tick, 125 ticks; start at the
     * first sample. */
    check(plain.find("\"startTimeUnixNano\":\"" + std::to_string(base_unix) + "\",\"timeUnixNano\":\"" +
                     std::to_string(base_unix + 124 * second / 10) + "\",\"asInt\":\"3750\"") != std::string::npos,
          "OTLP: counter value, start and time as strings");
    /* The sender reported a connection age of 1 h at its first sample: its
     * counters start at the connection, not when the source was added. */
    check(plain.find("\"startTimeUnixNano\":\"" + std::to_string(base_unix - 3600 * second) + "\",\"timeUnixNano\":\"" +
                     std::to_string(base_unix + 124 * second / 10) + "\",\"asInt\":\"3750\"") != std::string::npos,
          "OTLP: counters start at the connection when its age is known");
    check(plain.find("\"name\":\"srt.receiver.recovery_headroom\",") != std::string::npos &&
              plain.find("{\"key\":\"stat\",\"value\":{\"stringValue\":\"min\"}}") != std::string::npos &&
              plain.find("\"unit\":\"bit/s\"") != std::string::npos,
          "OTLP: window gauges with the stat attribute and UCUM units");
    /* "role" collides with the exporter's; "host_role" is the host's own,
     * so "role" becomes "host_host_role" (as in Prometheus). */
    check(plain.find("{\"key\":\"host_host_role\",\"value\":{\"stringValue\":\"uplink\"}}") != std::string::npos &&
              plain.find("{\"key\":\"host_role\",\"value\":{\"stringValue\":\"backup\"}}") !=
                  std::string::npos &&
              plain.find("{\"key\":\"site\",\"value\":{\"stringValue\":\"studio \\\"2\\\" \\\\ west\"}}") !=
                  std::string::npos,
          "OTLP: host labels renamed like Prometheus labels, escaped");
    check(plain.find("{\"key\":\"source_id\",\"value\":{\"intValue\":\"1\"}}") != std::string::npos &&
              plain.find("\"name\":\"srt.health.verdict\"") != std::string::npos &&
              plain.find("\"name\":\"srt.health.info\"") != std::string::npos &&
              plain.find("\"name\":\"srt.health.indicator.state\"") != std::string::npos,
          "OTLP: source attributes and health");
    check(plain.find("\"name\":\"srt.sender.unique_sent.packets\"") != std::string::npos &&
              plain.find("\"name\":\"srt.sender.recovery_headroom\"") == std::string::npos,
          "OTLP: role scoping");

    const robotweax_srt_telemetry_label_v1 resource[] = {{"service.name", "encoder-7"},
                                                         {"deployment.environment", "studio"}};
    robotweax_srt_telemetry_error error = ROBOTWEAX_SRT_TELEMETRY_OK;
    const auto custom = render_otlp(sampler, resource, 2, &error);
    check(error == ROBOTWEAX_SRT_TELEMETRY_OK && valid_json(custom) &&
              custom.find("\"stringValue\":\"encoder-7\"") != std::string::npos &&
              custom.find("\"stringValue\":\"srt-telemetry\"") == std::string::npos &&
              custom.find("\"stringValue\":\"studio\"") != std::string::npos,
          "OTLP: host resource attributes replace the default service.name");
    const robotweax_srt_telemetry_label_v1 empty_key[] = {{"", "x"}};
    static_cast<void>(render_otlp(sampler, empty_key, 1, &error));
    check(error == ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT, "OTLP: empty resource key rejected");
    static_cast<void>(render_otlp(sampler, nullptr, 1, &error));
    check(error == ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT, "OTLP: missing resource attributes rejected");
    static_cast<void>(render_otlp(sampler, resource, 33, &error));
    check(error == ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT, "OTLP: too many resource attributes rejected");
    const robotweax_srt_telemetry_label_v1 duplicate[] = {{"k", "a"}, {"k", "b"}};
    static_cast<void>(render_otlp(sampler, duplicate, 2, &error));
    check(error == ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT, "OTLP: duplicate resource key rejected");
    for (const char *bad : {"\xff\xfe", "a\nb", "\xc0\xaf", "\xed\xa0\x80", "\xe2\x82", "\xc2\x85"})
    {
        const robotweax_srt_telemetry_label_v1 invalid[] = {{"site", bad}};
        static_cast<void>(render_otlp(sampler, invalid, 1, &error));
        check(error == ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT, "OTLP: malformed UTF-8 or control character rejected");
    }
    const robotweax_srt_telemetry_label_v1 sdk[] = {{"site", "K\xc3\xb6ln \xe2\x82\xac \xf0\x9f\x93\xa1"},
                                                    {"telemetry.sdk.language", "c"}};
    const auto overridden = render_otlp(sampler, sdk, 2, &error);
    auto occurrences = [&](const std::string &needle) {
        std::size_t n = 0;
        for (auto at = overridden.find(needle); at != std::string::npos; at = overridden.find(needle, at + 1))
        {
            ++n;
        }
        return n;
    };
    check(error == ROBOTWEAX_SRT_TELEMETRY_OK && valid_json(overridden) &&
              occurrences("\"key\":\"telemetry.sdk.language\"") == 1 &&
              occurrences("\"key\":\"telemetry.sdk.name\"") == 1 &&
              overridden.find("K\xc3\xb6ln") != std::string::npos,
          "OTLP: UTF-8 accepted; each default resource attribute unless set");
    {
        robotweax_srt_telemetry_render_options_v1 options;
        robotweax_srt_telemetry_render_options_default(ROBOTWEAX_SRT_TELEMETRY_RENDER_JSON, &options);
        std::string sink;
        check(robotweax_srt_telemetry_sampler_render_otlp_to(
                  sampler, &options, resource, 2,
                  [](void *, const char *, std::size_t) { return 0; }, &sink) ==
                  ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT,
              "OTLP: render_otlp_to needs the OTLP format");
    }
    const auto trimmed = render(sampler, ROBOTWEAX_SRT_TELEMETRY_RENDER_OTLP_JSON,
                                ROBOTWEAX_SRT_TELEMETRY_RENDER_NO_COUNTERS | ROBOTWEAX_SRT_TELEMETRY_RENDER_NO_INDICATORS);
    check(valid_json(trimmed) && trimmed.find("\"sum\":") == std::string::npos &&
              trimmed.find("srt.health.indicator") == std::string::npos,
          "OTLP: flags trim counters and indicators");
    robotweax_srt_telemetry_sampler_destroy(sampler);
    robotweax_srt_telemetry_delta_engine_destroy(sender.engine);
    robotweax_srt_telemetry_delta_engine_destroy(receiver.engine);

    robotweax_srt_telemetry_sampler_config_v1 config{};
    robotweax_srt_telemetry_sampler_config_default(&config);
    robotweax_srt_telemetry_sampler *empty = nullptr;
    static_cast<void>(robotweax_srt_telemetry_sampler_create(&config, &empty));
    const auto none = render(empty, ROBOTWEAX_SRT_TELEMETRY_RENDER_OTLP_JSON);
    check(valid_json(none) && none.find("\"metrics\":[]") != std::string::npos, "OTLP: no sources, no metrics");
    robotweax_srt_telemetry_sampler_destroy(empty);
}

/* --dump-otlp: print a rendered request (for tools/otlp_validate.py). */
int dump_otlp()
{
    Fake sender;
    Fake receiver;
    auto *sampler = otlp_sampler(sender, receiver);
    const robotweax_srt_telemetry_label_v1 resource[] = {{"service.name", "encoder-7"}};
    std::cout << render_otlp(sampler, resource, 1);
    robotweax_srt_telemetry_sampler_destroy(sampler);
    robotweax_srt_telemetry_delta_engine_destroy(sender.engine);
    robotweax_srt_telemetry_delta_engine_destroy(receiver.engine);
    return EXIT_SUCCESS;
}

} // namespace

int main(const int argc, char **argv)
{
    if (argc > 1 && std::string_view(argv[1]) == "--dump-otlp")
    {
        return dump_otlp();
    }
    test_names();
    test_render();
    test_otlp();
    if (failures != 0)
    {
        std::cerr << failures << " export checks failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "all export checks passed\n";
    return EXIT_SUCCESS;
}
