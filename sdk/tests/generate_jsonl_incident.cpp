#include <robotweax/srt_telemetry.hpp>

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>

namespace
{

constexpr std::uint64_t interval_nano = 1'000'000'000ULL;
constexpr std::uint64_t base_unix_nano = 1'800'000'000'000'000'000ULL;

robotweax_srt_telemetry_raw_sample_v1 sample(const robotweax_srt_telemetry_provider provider,
                                             const std::uint64_t index)
{
    robotweax_srt_telemetry_raw_sample_v1 value{};
    value.struct_size = sizeof(value);
    value.abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
    value.provider = provider;
    value.capabilities = ROBOTWEAX_SRT_TELEMETRY_CAP_COMMON_STATS_V1;
    value.observed_at_unix_nano = base_unix_nano + (index + 1) * interval_nano;
    value.sample_monotonic_nano = (index + 1) * interval_nano;
    return value;
}

bool write_file(const std::string &path, const std::string &contents)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    return output.good();
}

} // namespace

int main(const int argc, char **argv)
{
    if (argc != 3)
    {
        std::cerr << "usage: generate-jsonl-incident SENDER.jsonl RECEIVER.jsonl\n";
        return EXIT_FAILURE;
    }

    robotweax::srt::telemetry::DeltaEngine sender_engine;
    robotweax::srt::telemetry::DeltaEngine receiver_engine;
    robotweax::srt::telemetry::BoundedRecorder sender_recorder(9);
    robotweax::srt::telemetry::BoundedRecorder receiver_recorder(9);
    std::uint64_t unique_packets = 1'000;
    std::uint64_t physical_packets = 1'000;
    std::uint64_t loss_packets = 0;
    std::uint64_t retransmitted_packets = 0;

    for (std::uint64_t index = 0; index < 9; ++index)
    {
        if (index != 0)
        {
            unique_packets += 100;
            physical_packets += 100;
            if (index >= 3 && index <= 6)
            {
                physical_packets += 5;
                loss_packets += 5;
                retransmitted_packets += 5;
            }
        }

        auto sender = sample(ROBOTWEAX_SRT_TELEMETRY_PROVIDER_ROBOTWEAX_SRT, index);
        sender.available_fields = ROBOTWEAX_SRT_TELEMETRY_FIELD_SENT_PACKET_TOTAL |
                                  ROBOTWEAX_SRT_TELEMETRY_FIELD_UNIQUE_SENT_PACKET_TOTAL |
                                  ROBOTWEAX_SRT_TELEMETRY_FIELD_SENDER_LOSS_PACKET_TOTAL |
                                  ROBOTWEAX_SRT_TELEMETRY_FIELD_RETRANSMITTED_PACKET_TOTAL;
        sender.sent_packet_total = physical_packets;
        sender.unique_sent_packet_total = unique_packets;
        sender.sender_loss_packet_total = loss_packets;
        sender.retransmitted_packet_total = retransmitted_packets;
        sender_recorder.push(sender, sender_engine.update(sender));

        auto receiver = sample(ROBOTWEAX_SRT_TELEMETRY_PROVIDER_HAIVISION_SRT, index);
        receiver.available_fields = ROBOTWEAX_SRT_TELEMETRY_FIELD_RECEIVED_PACKET_TOTAL |
                                    ROBOTWEAX_SRT_TELEMETRY_FIELD_UNIQUE_RECEIVED_PACKET_TOTAL |
                                    ROBOTWEAX_SRT_TELEMETRY_FIELD_RECEIVER_LOSS_PACKET_TOTAL;
        receiver.received_packet_total = unique_packets;
        receiver.unique_received_packet_total = unique_packets;
        receiver.receiver_loss_packet_total = loss_packets;
        receiver_recorder.push(receiver, receiver_engine.update(receiver));
    }

    robotweax::srt::telemetry::JsonlExportMetadata sender_metadata;
    sender_metadata.stream_id = "fixture-sender";
    sender_metadata.flow_id = "fixture-constant-loss";
    sender_metadata.provider_version = "0.2.0-test";
    sender_metadata.provider_revision = "0123456789abcdef";
    sender_metadata.build_profile = "release-test";
    sender_metadata.role = ROBOTWEAX_SRT_TELEMETRY_ENDPOINT_ROLE_SENDER;
    sender_metadata.configured_provider = ROBOTWEAX_SRT_TELEMETRY_PROVIDER_ROBOTWEAX_SRT;

    robotweax::srt::telemetry::JsonlExportMetadata receiver_metadata;
    receiver_metadata.stream_id = "fixture-receiver";
    receiver_metadata.flow_id = "fixture-constant-loss";
    receiver_metadata.provider_version = "1.5.5-test";
    receiver_metadata.provider_revision = "fedcba9876543210";
    receiver_metadata.build_profile = "release-test";
    receiver_metadata.role = ROBOTWEAX_SRT_TELEMETRY_ENDPOINT_ROLE_RECEIVER;
    receiver_metadata.configured_provider = ROBOTWEAX_SRT_TELEMETRY_PROVIDER_HAIVISION_SRT;

    if (!write_file(argv[1], sender_recorder.export_jsonl(sender_metadata)) ||
        !write_file(argv[2], receiver_recorder.export_jsonl(receiver_metadata)))
    {
        std::cerr << "failed to write JSONL incident fixture\n";
        return EXIT_FAILURE;
    }
    std::cout << "JSONL incident fixture generated\n";
    return EXIT_SUCCESS;
}
