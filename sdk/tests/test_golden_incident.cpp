#include <robotweax/srt_telemetry.hpp>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace
{

using Raw = robotweax_srt_telemetry_raw_sample_v1;
using Delta = robotweax_srt_telemetry_delta_v1;

struct CounterMapping
{
    std::string_view raw_name;
    std::string_view expected_name;
    std::uint64_t field;
    std::uint64_t Raw::*raw_member;
    std::uint64_t Delta::*delta_member;
};

struct RateMapping
{
    std::string_view expected_name;
    std::uint64_t field;
    double Delta::*member;
};

constexpr CounterMapping counter_mappings[] = {
    {"sent_packet_total", "expected_sent_packet_delta",
     ROBOTWEAX_SRT_TELEMETRY_FIELD_SENT_PACKET_TOTAL, &Raw::sent_packet_total,
     &Delta::sent_packet_delta},
    {"received_packet_total", "expected_received_packet_delta",
     ROBOTWEAX_SRT_TELEMETRY_FIELD_RECEIVED_PACKET_TOTAL, &Raw::received_packet_total,
     &Delta::received_packet_delta},
    {"unique_sent_packet_total", "expected_unique_sent_packet_delta",
     ROBOTWEAX_SRT_TELEMETRY_FIELD_UNIQUE_SENT_PACKET_TOTAL, &Raw::unique_sent_packet_total,
     &Delta::unique_sent_packet_delta},
    {"unique_received_packet_total", "expected_unique_received_packet_delta",
     ROBOTWEAX_SRT_TELEMETRY_FIELD_UNIQUE_RECEIVED_PACKET_TOTAL, &Raw::unique_received_packet_total,
     &Delta::unique_received_packet_delta},
    {"sender_loss_packet_total", "expected_sender_loss_packet_delta",
     ROBOTWEAX_SRT_TELEMETRY_FIELD_SENDER_LOSS_PACKET_TOTAL, &Raw::sender_loss_packet_total,
     &Delta::sender_loss_packet_delta},
    {"receiver_loss_packet_total", "expected_receiver_loss_packet_delta",
     ROBOTWEAX_SRT_TELEMETRY_FIELD_RECEIVER_LOSS_PACKET_TOTAL, &Raw::receiver_loss_packet_total,
     &Delta::receiver_loss_packet_delta},
    {"retransmitted_packet_total", "expected_retransmitted_packet_delta",
     ROBOTWEAX_SRT_TELEMETRY_FIELD_RETRANSMITTED_PACKET_TOTAL, &Raw::retransmitted_packet_total,
     &Delta::retransmitted_packet_delta},
    {"sender_drop_packet_total", "expected_sender_drop_packet_delta",
     ROBOTWEAX_SRT_TELEMETRY_FIELD_SENDER_DROP_PACKET_TOTAL, &Raw::sender_drop_packet_total,
     &Delta::sender_drop_packet_delta},
    {"receiver_drop_packet_total", "expected_receiver_drop_packet_delta",
     ROBOTWEAX_SRT_TELEMETRY_FIELD_RECEIVER_DROP_PACKET_TOTAL, &Raw::receiver_drop_packet_total,
     &Delta::receiver_drop_packet_delta},
    {"receiver_undecryptable_packet_total", "expected_receiver_undecryptable_packet_delta",
     ROBOTWEAX_SRT_TELEMETRY_FIELD_RECEIVER_UNDECRYPTABLE_PACKET_TOTAL,
     &Raw::receiver_undecryptable_packet_total, &Delta::receiver_undecryptable_packet_delta},
    {"sent_wire_byte_total", "expected_sent_wire_byte_delta",
     ROBOTWEAX_SRT_TELEMETRY_FIELD_SENT_WIRE_BYTE_TOTAL, &Raw::sent_wire_byte_total,
     &Delta::sent_wire_byte_delta},
    {"received_wire_byte_total", "expected_received_wire_byte_delta",
     ROBOTWEAX_SRT_TELEMETRY_FIELD_RECEIVED_WIRE_BYTE_TOTAL, &Raw::received_wire_byte_total,
     &Delta::received_wire_byte_delta},
    {"unique_sent_wire_byte_total", "expected_unique_sent_wire_byte_delta",
     ROBOTWEAX_SRT_TELEMETRY_FIELD_UNIQUE_SENT_WIRE_BYTE_TOTAL, &Raw::unique_sent_wire_byte_total,
     &Delta::unique_sent_wire_byte_delta},
    {"unique_received_wire_byte_total", "expected_unique_received_wire_byte_delta",
     ROBOTWEAX_SRT_TELEMETRY_FIELD_UNIQUE_RECEIVED_WIRE_BYTE_TOTAL,
     &Raw::unique_received_wire_byte_total, &Delta::unique_received_wire_byte_delta},
    {"retransmitted_wire_byte_total", "expected_retransmitted_wire_byte_delta",
     ROBOTWEAX_SRT_TELEMETRY_FIELD_RETRANSMITTED_WIRE_BYTE_TOTAL,
     &Raw::retransmitted_wire_byte_total, &Delta::retransmitted_wire_byte_delta},
};

constexpr RateMapping rate_mappings[] = {
    {"expected_sent_wire_bits_per_second", ROBOTWEAX_SRT_TELEMETRY_RATE_SENT_WIRE_BITS_PER_SECOND,
     &Delta::sent_wire_bits_per_second},
    {"expected_received_wire_bits_per_second",
     ROBOTWEAX_SRT_TELEMETRY_RATE_RECEIVED_WIRE_BITS_PER_SECOND,
     &Delta::received_wire_bits_per_second},
    {"expected_unique_sent_wire_bits_per_second",
     ROBOTWEAX_SRT_TELEMETRY_RATE_UNIQUE_SENT_WIRE_BITS_PER_SECOND,
     &Delta::unique_sent_wire_bits_per_second},
    {"expected_unique_received_wire_bits_per_second",
     ROBOTWEAX_SRT_TELEMETRY_RATE_UNIQUE_RECEIVED_WIRE_BITS_PER_SECOND,
     &Delta::unique_received_wire_bits_per_second},
};

std::vector<std::string> split_csv(const std::string &line)
{
    auto normalized = line;
    if (!normalized.empty() && normalized.back() == '\r')
    {
        normalized.pop_back();
    }
    std::vector<std::string> values;
    std::size_t begin = 0;
    while (true)
    {
        const auto comma = normalized.find(',', begin);
        values.push_back(
            normalized.substr(begin, comma == std::string::npos ? comma : comma - begin));
        if (comma == std::string::npos)
        {
            return values;
        }
        begin = comma + 1;
    }
}

class CsvRow final
{
  public:
    CsvRow(const std::unordered_map<std::string, std::size_t> &columns,
           std::vector<std::string> values)
        : columns_(columns), values_(std::move(values))
    {
        if (values_.size() != columns_.size())
        {
            throw std::runtime_error("CSV row has an unexpected column count");
        }
    }

    [[nodiscard]] const std::string &get(const std::string_view name) const
    {
        const auto found = columns_.find(std::string(name));
        if (found == columns_.end())
        {
            throw std::runtime_error("CSV column is missing: " + std::string(name));
        }
        return values_[found->second];
    }

  private:
    const std::unordered_map<std::string, std::size_t> &columns_;
    std::vector<std::string> values_;
};

std::uint64_t unsigned_value(const std::string &value, const std::string_view name)
{
    if (value.empty())
    {
        throw std::runtime_error("required value is empty: " + std::string(name));
    }
    if (value.front() == '-')
    {
        throw std::runtime_error("negative unsigned value: " + std::string(name));
    }
    std::size_t consumed = 0;
    const auto parsed = std::stoull(value, &consumed);
    if (consumed != value.size())
    {
        throw std::runtime_error("invalid unsigned value: " + std::string(name));
    }
    return parsed;
}

std::optional<std::uint64_t> optional_unsigned(const std::string &value,
                                               const std::string_view name)
{
    return value.empty() ? std::nullopt : std::optional<std::uint64_t>(unsigned_value(value, name));
}

double double_value(const std::string &value, const std::string_view name)
{
    if (value.empty())
    {
        throw std::runtime_error("required value is empty: " + std::string(name));
    }
    std::size_t consumed = 0;
    const auto parsed = std::stod(value, &consumed);
    if (consumed != value.size() || !std::isfinite(parsed))
    {
        throw std::runtime_error("invalid floating-point value: " + std::string(name));
    }
    return parsed;
}

robotweax_srt_telemetry_provider provider_value(const std::string &provider)
{
    if (provider == "robotweax-srt")
    {
        return ROBOTWEAX_SRT_TELEMETRY_PROVIDER_ROBOTWEAX_SRT;
    }
    if (provider == "haivision-srt")
    {
        return ROBOTWEAX_SRT_TELEMETRY_PROVIDER_HAIVISION_SRT;
    }
    throw std::runtime_error("unsupported provider: " + provider);
}

void require_equal(const std::uint64_t actual, const std::uint64_t expected,
                   const std::string_view name, const std::string &context)
{
    if (actual != expected)
    {
        throw std::runtime_error(context + " expected " + std::string(name) + "=" +
                                 std::to_string(expected) + ", got " + std::to_string(actual));
    }
}

void validate_row(const CsvRow &row, robotweax::srt::telemetry::DeltaEngine &engine)
{
    Raw raw{};
    raw.struct_size = sizeof(raw);
    raw.abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
    raw.provider = provider_value(row.get("provider"));
    raw.capabilities = ROBOTWEAX_SRT_TELEMETRY_CAP_COMMON_STATS_V1;
    raw.observed_at_unix_nano =
        unsigned_value(row.get("observed_at_unix_nano"), "observed_at_unix_nano");
    raw.sample_monotonic_nano =
        unsigned_value(row.get("sample_monotonic_nano"), "sample_monotonic_nano");
    for (const auto &mapping : counter_mappings)
    {
        const auto value = optional_unsigned(row.get(mapping.raw_name), mapping.raw_name);
        if (value.has_value())
        {
            raw.available_fields |= mapping.field;
            raw.*(mapping.raw_member) = *value;
        }
    }
    const auto &rtt = row.get("rtt_seconds");
    if (!rtt.empty())
    {
        raw.available_fields |= ROBOTWEAX_SRT_TELEMETRY_FIELD_RTT_SECONDS;
        raw.rtt_seconds = double_value(rtt, "rtt_seconds");
    }

    const auto delta = engine.update(raw);
    const auto context = row.get("role") + "/" + row.get("sample_index");
    require_equal(delta.quality_flags,
                  unsigned_value(row.get("expected_quality_flags"), "expected_quality_flags"),
                  "quality_flags", context);
    require_equal(delta.interval_nano,
                  unsigned_value(row.get("expected_interval_nano"), "expected_interval_nano"),
                  "interval_nano", context);
    require_equal(delta.counter_reset_fields, 0, "counter_reset_fields", context);

    std::uint64_t expected_available_fields = 0;
    for (const auto &mapping : counter_mappings)
    {
        const auto expected =
            optional_unsigned(row.get(mapping.expected_name), mapping.expected_name);
        if (expected.has_value())
        {
            expected_available_fields |= mapping.field;
            require_equal(delta.*(mapping.delta_member), *expected, mapping.expected_name, context);
        }
    }
    require_equal(delta.available_delta_fields, expected_available_fields, "available_delta_fields",
                  context);

    std::uint64_t expected_available_rates = 0;
    for (const auto &mapping : rate_mappings)
    {
        const auto &expected_text = row.get(mapping.expected_name);
        if (!expected_text.empty())
        {
            expected_available_rates |= mapping.field;
            const auto expected = double_value(expected_text, mapping.expected_name);
            const auto actual = delta.*(mapping.member);
            if (std::abs(actual - expected) > 0.001)
            {
                throw std::runtime_error(
                    context + " expected " + std::string(mapping.expected_name) + "=" +
                    std::to_string(expected) + ", got " + std::to_string(actual));
            }
        }
    }
    require_equal(delta.available_rates, expected_available_rates, "available_rates", context);
}

int run(const std::string &dataset_path)
{
    std::ifstream source(dataset_path);
    if (!source)
    {
        throw std::runtime_error("cannot open dataset: " + dataset_path);
    }
    std::string line;
    if (!std::getline(source, line))
    {
        throw std::runtime_error("dataset is empty");
    }
    const auto header = split_csv(line);
    std::unordered_map<std::string, std::size_t> columns;
    for (std::size_t index = 0; index < header.size(); ++index)
    {
        if (!columns.emplace(header[index], index).second)
        {
            throw std::runtime_error("duplicate CSV column: " + header[index]);
        }
    }

    std::map<std::string, std::unique_ptr<robotweax::srt::telemetry::DeltaEngine>> engines;
    std::size_t row_count = 0;
    while (std::getline(source, line))
    {
        CsvRow row(columns, split_csv(line));
        const auto &role = row.get("role");
        auto [position, inserted] = engines.try_emplace(role);
        if (inserted)
        {
            position->second = std::make_unique<robotweax::srt::telemetry::DeltaEngine>();
        }
        validate_row(row, *position->second);
        ++row_count;
    }
    if (row_count == 0 || engines.empty())
    {
        throw std::runtime_error("dataset has no samples");
    }
    std::cout << "golden incident delta validation passed: " << row_count << " rows across "
              << engines.size() << " roles\n";
    return EXIT_SUCCESS;
}

} // namespace

int main(const int argc, char **argv)
{
    if (argc != 2)
    {
        std::cerr << "usage: golden-incident-test SAMPLES.csv\n";
        return EXIT_FAILURE;
    }
    try
    {
        return run(argv[1]);
    }
    catch (const std::exception &error)
    {
        std::cerr << "FAIL: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
