#pragma once
// Review draft only: not installed, not a released SDK/recording version.
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace robotweax::srt::telemetry::remote_draft
{
enum class Synchronization
{
    unknown,
    unsynchronized,
    synchronized
};
enum class Sampling
{
    unknown,
    averaged,
    instantaneous
};
enum class ClearPolicy
{
    unknown,
    exclusive_no_clear
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
    std::string segment_id;
    std::string device, session, resource;
    std::array<std::uint8_t, 32> media_session_id{};
    std::uint64_t source_generation{}, association_generation{}, component_generation{},
        profile_revision{}, metadata_epoch{}, binding_generation{};
    // First draft: Single / Live sender, connection row only. Unsupported
    // roles/scopes must not be silently normalized to these values.
    std::string media_kind, scope, role;
    std::uint8_t member_slot{};
    std::uint64_t member_generation{};
    std::vector<std::uint8_t> path_label;
    std::optional<std::string> producer_instance, producer_clock_domain;
    std::optional<std::string> observer_node, observer_instance;
    std::string observer_clock_domain;
    std::optional<std::string> provider, provider_revision, build_profile, qualification_id;
    Sampling sampling{Sampling::unknown};
    ClearPolicy clear_policy{ClearPolicy::unknown};
};
struct Observation
{
    std::uint64_t sequence{}, producer_monotonic_nano{};
    std::optional<std::uint64_t> interval_begin_monotonic_nano;
    std::optional<WallTime> producer_wall_time;
    bool cleared_after_sample{};
    // Original Type 5/schema 1.0 RWBL bytes, copied exactly, never injected into
    // the local socket adapter. Host validates/authenticates/decodes the body
    // and binds its context before calling this recorder. No parsing here.
    std::vector<std::uint8_t> native_rwbl;
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
// One immutable row/Metadata/observer segment per recorder, host serialized.
// This is storage, not admission authority. The host must freshly recheck its
// source/lease/pair/grants at ingestion and actual publication. No threads,
// callbacks, socket getters, SRT/Metadata dependency, metrics/rates or UI input.
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
    std::uint64_t maximum_age_, overwritten_{}, last_receipt_call_{};
    mutable std::uint64_t last_export_clock_{};
    bool retired_{};
};
} // namespace robotweax::srt::telemetry::remote_draft
