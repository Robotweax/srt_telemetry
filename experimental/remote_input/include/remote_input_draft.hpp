#pragma once
// Review draft only: not installed, not a released SDK/recording version.
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace robotweax::srt::telemetry::remote_draft
{
enum class Synchronization
{
    unknown,
    unsynchronized,
    synchronized
};
enum class EndpointRole
{
    unknown,
    sender,
    receiver
};
// Field paths are draft diagnostics, not a released C error-code contract.
class ValidationError final : public std::invalid_argument
{
  public:
    explicit ValidationError(std::string field)
        : std::invalid_argument("invalid remote-input draft field: " + field),
          field_(std::move(field))
    {
    }
    const std::string &field() const noexcept
    {
        return field_;
    }

  private:
    std::string field_;
};
struct Origin
{
    std::string namespace_id, schema;
    // Producer-owned opaque context, 0..4096 bytes. Telemetry does not parse it.
    std::vector<std::uint8_t> context;
};
struct WallTime
{
    std::uint64_t unix_nano{};
    Synchronization synchronization{Synchronization::unknown};
    std::optional<std::uint64_t> maximum_error_nano;
    std::string provenance;
    bool operator==(const WallTime &) const = default;
};
struct Context
{
    std::string segment_id, producer_id, source_id;
    EndpointRole role{EndpointRole::unknown};
    std::optional<std::string> producer_instance, producer_clock_domain;
    std::optional<std::string> observer_node, observer_instance;
    std::string observer_clock_domain;
    Origin origin;
};
struct Observation
{
    std::uint64_t sequence{}, producer_monotonic_nano{};
    std::optional<std::uint64_t> interval_begin_monotonic_nano;
    std::optional<WallTime> producer_wall_time;
    // Opaque original bytes belonging to Context::origin, 1..16384 bytes.
    // Host validates/authorizes them before storage and publication.
    std::vector<std::uint8_t> origin_payload;
    bool operator==(const Observation &) const = default;
};
struct Receipt
{
    std::uint64_t monotonic_nano{};
    std::optional<WallTime> wall_time;
    bool operator==(const Receipt &) const = default;
};
enum class PushResult
{
    accepted,
    duplicate,
    conflict,
    stale_sequence,
    producer_clock_regression,
    retired
};
enum class Status
{
    available,
    no_sample,
    stale,
    retired
};
struct Record
{
    Observation observation;
    Receipt first_receipt;
    bool first_in_segment{}, sequence_discontinuity{}, equal_producer_time{};
};
struct Current
{
    Status status;
    std::optional<Record> record;
    std::uint64_t exported_monotonic_nano{};
};
// One immutable producer/source/origin/observer segment per recorder.
// Concurrent const reads are safe only while no mutation or destruction occurs.
// The host must externally serialize all mutations against readers/writers.
// This is storage, not admission authority. The host must freshly recheck its
// origin-specific authority at ingestion and publication. No internal threads,
// callbacks, socket getters, metrics/rates or UI input.
class Recorder final
{
  public:
    explicit Recorder(Context, std::size_t capacity = 8,
                      std::uint64_t maximum_age_nano = 1'000'000'000);
    Recorder(const Recorder &) = delete;
    Recorder &operator=(const Recorder &) = delete;
    PushResult push(const Observation &, const Receipt &);
    Current latest(std::uint64_t exported_monotonic_nano) const;
    Record read_history(std::size_t oldest_index) const;
    // Explicit historical export, including after retirement; never renews
    // authorization or first receipt. A separate, closed draft schema.
    std::string export_history_jsonl(std::uint64_t exported_monotonic_nano) const;
    void retire() noexcept
    {
        retired_ = true;
    }
    std::size_t size() const noexcept
    {
        return size_;
    }
    std::size_t capacity() const noexcept
    {
        return slots_.size();
    }
    std::uint64_t overwritten_count() const noexcept
    {
        return overwritten_;
    }
    const Context &context() const noexcept
    {
        return context_;
    }

  private:
    void check_export_clock(std::uint64_t) const;
    Context context_;
    std::vector<std::optional<Record>> slots_;
    std::size_t head_{}, size_{};
    std::uint64_t maximum_age_, overwritten_{}, last_accepted_receipt_nano_{};
    bool retired_{};
};
} // namespace robotweax::srt::telemetry::remote_draft
