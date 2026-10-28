#pragma once
// Uninstalled numeric experiment. Origin owners provide normalized, qualified
// values; this component never parses origin data or fabricates wall time.
#include "remote_input_draft.hpp"
#include <memory>
#include <robotweax/srt_telemetry.h>

namespace robotweax::srt::telemetry::remote_draft
{
struct RawValues
{
    std::uint64_t available_fields{};
    uint64_t sent_packet_total{};
    uint64_t received_packet_total{};
    uint64_t unique_sent_packet_total{};
    uint64_t unique_received_packet_total{};
    uint64_t sender_loss_packet_total{};
    uint64_t receiver_loss_packet_total{};
    uint64_t retransmitted_packet_total{};
    uint64_t sender_drop_packet_total{};
    uint64_t receiver_drop_packet_total{};
    uint64_t receiver_undecryptable_packet_total{};
    uint64_t sent_wire_byte_total{};
    uint64_t received_wire_byte_total{};
    uint64_t unique_sent_wire_byte_total{};
    uint64_t unique_received_wire_byte_total{};
    uint64_t retransmitted_wire_byte_total{};
    double rtt_seconds{};
    uint64_t flight_size_packets{};
    uint64_t flow_window_packets{};
    uint64_t congestion_window_packets{};
    uint64_t available_send_buffer_bytes{};
    uint64_t available_receive_buffer_bytes{};
    uint64_t send_buffer_bytes{};
    double send_buffer_seconds{};
    uint64_t receive_buffer_bytes{};
    double receive_buffer_seconds{};
    double sender_tsbpd_delay_seconds{};
    double receiver_tsbpd_delay_seconds{};
};
struct ExtendedValues
{
    std::uint64_t available_fields{};
    double connection_age_seconds{};
    uint64_t sent_ack_packet_total{};
    uint64_t received_ack_packet_total{};
    uint64_t sent_nak_packet_total{};
    uint64_t received_nak_packet_total{};
    uint64_t sender_active_nano_total{};
    uint64_t receiver_loss_wire_byte_total{};
    uint64_t sender_drop_wire_byte_total{};
    uint64_t receiver_drop_wire_byte_total{};
    uint64_t receiver_undecryptable_payload_byte_total{};
    uint64_t sent_filter_extra_packet_total{};
    uint64_t received_filter_extra_packet_total{};
    uint64_t filter_recovered_packet_total{};
    uint64_t filter_unrecovered_packet_total{};
    uint64_t received_retransmitted_packet_since_clear{};
    uint64_t belated_packet_since_clear{};
    uint64_t reorder_distance_packets{};
    double packet_send_period_seconds{};
    double estimated_link_capacity_bits_per_second{};
    double data_arrival_rate_bits_per_second{};
    double send_bandwidth_limit_bits_per_second{};
    uint64_t mss_bytes{};
    uint64_t send_buffer_packets{};
    uint64_t receive_buffer_packets{};
    uint64_t reorder_tolerance_packets{};
    double average_belated_seconds{};
};

enum class GaugeSampling
{
    unknown,
    averaged,
    instantaneous
};
enum class SinceClearPolicy
{
    unknown,
    exclusive_no_clear,
    explicit_boundary
};
struct NumericQualification
{
    // Identifier of host-owned evidence, not a source credential/attestation.
    std::string id;
    robotweax_srt_telemetry_provider provider{ROBOTWEAX_SRT_TELEMETRY_PROVIDER_UNKNOWN};
    std::uint64_t raw_fields{}, extended_fields{};
    GaugeSampling queue_sampling{GaugeSampling::unknown};
    SinceClearPolicy since_clear{SinceClearPolicy::unknown};
};
struct NumericValues
{
    RawValues raw;
    ExtendedValues extended;
    // Origin owner provides actual clear-boundary evidence; null is unknown.
    // Must be absent with exclusive_no_clear; choose explicit_boundary to use it.
    std::optional<std::uint64_t> clear_boundary;
};
struct ProcessedNumeric
{
    std::uint64_t sequence{}, producer_monotonic_nano{};
    NumericValues qualified_values;
    robotweax_srt_telemetry_delta_v1 delta{};
    robotweax_srt_telemetry_ext_delta_v1 extended_delta{};
    bool clear_boundary_changed{}, since_clear_delta_eligible{};
};
struct NumericResult
{
    PushResult status;
    std::optional<ProcessedNumeric> sample;
};
// One immutable source/origin/observer/qualification segment, external executor.
// No reset, policy change, source authority, callbacks, threads or native effects.
// Only accepted observations emit numeric results. Existing history export stays
// raw draft2; no consumer/recorder numeric schema or public C ABI is introduced.
class NumericProcessor final
{
  public:
    explicit NumericProcessor(Context, NumericQualification = {});
    ~NumericProcessor();
    NumericProcessor(const NumericProcessor &) = delete;
    NumericProcessor &operator=(const NumericProcessor &) = delete;
    NumericResult push(const Observation &, const Receipt &, const NumericValues &);
    void retire() noexcept;
    Current latest(std::uint64_t now) const;
    std::string export_history_jsonl(std::uint64_t now) const;
    const Context &context() const noexcept;
    const NumericQualification &qualification() const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace robotweax::srt::telemetry::remote_draft
