#include "remote_input_draft.hpp"
#include <algorithm>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace robotweax::srt::telemetry::remote_draft
{
namespace
{
void require(bool ok)
{
    if (!ok)
        throw std::invalid_argument("invalid remote-input draft value");
}
void ascii(const std::string &text, std::size_t maximum)
{
    require(
        !text.empty() && text.size() <= maximum &&
        std::all_of(text.begin(), text.end(), [](unsigned char c) { return c >= 32 && c <= 126; }));
}
void optional_ascii(const std::optional<std::string> &text)
{
    if (text)
        ascii(*text, 128);
}
void observer(const std::optional<std::string> &text)
{
    if (!text)
        return;
    require(!text->empty() && text->size() <= 64 && *text != "null" &&
            std::all_of(text->begin(), text->end(),
                        [](unsigned char c)
                        {
                            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                                   (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-';
                        }));
}
void wall(const std::optional<WallTime> &value)
{
    if (!value)
        return;
    ascii(value->provenance, 128);
    switch (value->synchronization)
    {
    case Synchronization::unknown:
    case Synchronization::unsynchronized:
        require(!value->maximum_error_nano);
        break;
    case Synchronization::synchronized:
        require(value->maximum_error_nano.has_value());
        break;
    default:
        require(false);
    }
}
void validate(const Context &c)
{
    ascii(c.segment_id, 128);
    ascii(c.device, 128);
    ascii(c.session, 128);
    ascii(c.resource, 512);
    ascii(c.observer_clock_domain, 128);
    require(c.source_generation && c.association_generation && c.component_generation &&
            c.profile_revision && c.metadata_epoch && c.binding_generation);
    require(std::any_of(c.media_session_id.begin(), c.media_session_id.end(),
                        [](auto b) { return b != 0; }));
    require(c.native_kind == "single" && c.media_kind == "live" && c.scope == "connection" &&
            c.role == "sender" && !c.member_slot && !c.member_generation && c.path_label.empty());
    optional_ascii(c.producer_instance);
    optional_ascii(c.producer_clock_domain);
    require(!c.provider || *c.provider == "robotweax-srt" || *c.provider == "haivision-srt");
    optional_ascii(c.provider_revision);
    optional_ascii(c.build_profile);
    optional_ascii(c.qualification_id);
    observer(c.observer_node);
    observer(c.observer_instance);
    require(c.sampling == Sampling::unknown || c.sampling == Sampling::averaged ||
            c.sampling == Sampling::instantaneous);
    require(c.clear_policy == ClearPolicy::unknown ||
            c.clear_policy == ClearPolicy::exclusive_no_clear);
}
std::string quote(std::string_view value)
{
    std::string out = "\"";
    for (const char c : value)
    {
        if (c == '\\' || c == '"')
            out += '\\';
        out += c;
    }
    return out + '"';
}
std::string integer(std::uint64_t value)
{
    return quote(std::to_string(value));
}
std::string optional_integer(std::optional<std::uint64_t> value)
{
    return value ? integer(*value) : "null";
}
std::string optional_text(const std::optional<std::string> &value)
{
    return value ? quote(*value) : "null";
}
template <class Range> std::string hex(const Range &bytes)
{
    constexpr char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (auto byte : bytes)
    {
        out += digits[byte >> 4];
        out += digits[byte & 15];
    }
    return out;
}
std::string render_wall(const std::optional<WallTime> &value)
{
    if (!value)
        return "null";
    const char *state = value->synchronization == Synchronization::synchronized ? "synchronized"
                        : value->synchronization == Synchronization::unsynchronized
                            ? "unsynchronized"
                            : "unknown";
    return "{\"unixNano\":" + integer(value->unix_nano) + ",\"synchronization\":" + quote(state) +
           ",\"maximumErrorNano\":" + optional_integer(value->maximum_error_nano) +
           ",\"provenance\":" + quote(value->provenance) + '}';
}
const char *boolean(bool v)
{
    return v ? "true" : "false";
}
} // namespace

Recorder::Recorder(Context context, std::size_t capacity, std::uint64_t age)
    : context_(std::move(context)), maximum_age_(age)
{
    validate(context_);
    require(capacity && capacity <= 256 && age && age <= 60'000'000'000);
    slots_.resize(capacity);
}
PushResult Recorder::push(const Observation &value, const Receipt &receipt)
{
    if (retired_)
        return PushResult::retired;
    require(value.sequence && value.producer_monotonic_nano && receipt.monotonic_nano &&
            receipt.monotonic_nano >= last_receipt_call_ && !value.native_rwbl.empty() &&
            value.native_rwbl.size() <= 16384);
    require(!value.interval_begin_monotonic_nano ||
            *value.interval_begin_monotonic_nano <= value.producer_monotonic_nano);
    wall(value.producer_wall_time);
    wall(receipt.wall_time);
    require(context_.clear_policy != ClearPolicy::exclusive_no_clear ||
            !value.cleared_after_sample);
    const auto *previous = size_ ? &*slots_[(head_ + size_ - 1) % slots_.size()] : nullptr;
    if (previous)
    {
        const auto &old = previous->observation;
        if (value.sequence == old.sequence)
            return value == old ? PushResult::duplicate : PushResult::conflict;
        if (value.sequence < old.sequence)
            return PushResult::stale_sequence;
        if (value.producer_monotonic_nano < old.producer_monotonic_nano)
            return PushResult::producer_clock_regression;
    }
    if (size_ == slots_.size() && overwritten_ == std::numeric_limits<std::uint64_t>::max())
        throw std::overflow_error("remote recorder local overwrite count exhausted");
    Record candidate{
        value, receipt, !previous, previous && value.sequence - previous->observation.sequence > 1,
        previous && value.producer_monotonic_nano == previous->observation.producer_monotonic_nano};
    // Allocate/copy the owned record before replacing any current data.
    if (size_ == slots_.size())
    {
        slots_[head_] = std::move(candidate);
        head_ = (head_ + 1) % slots_.size();
        ++overwritten_;
    }
    else
    {
        slots_[(head_ + size_) % slots_.size()] = std::move(candidate);
        ++size_;
    }
    last_receipt_call_ = receipt.monotonic_nano;
    return PushResult::accepted;
}
void Recorder::check_export_clock(std::uint64_t now) const
{
    require(now && now >= last_receipt_call_ && now >= last_export_clock_);
    last_export_clock_ = now;
}
Record Recorder::read_history(std::size_t index) const
{
    if (index >= size_)
        throw std::out_of_range("remote history index");
    return *slots_[(head_ + index) % slots_.size()];
}
Current Recorder::latest(std::uint64_t now) const
{
    check_export_clock(now);
    if (retired_)
        return {Status::retired, {}, now};
    if (!size_)
        return {Status::no_sample, {}, now};
    const auto &value = *slots_[(head_ + size_ - 1) % slots_.size()];
    if (now - value.first_receipt.monotonic_nano > maximum_age_)
        return {Status::stale, {}, now};
    return {Status::available, value, now};
}
std::string Recorder::export_history_jsonl(std::uint64_t now) const
{
    check_export_clock(now);
    const auto &c = context_;
    const char *sampling = c.sampling == Sampling::instantaneous ? "instantaneous"
                           : c.sampling == Sampling::averaged    ? "averaged"
                                                                 : "unknown";
    std::string out =
        "{\"type\":\"remoteHistory\",\"schemaVersion\":\"remote-input.draft1\""
        ",\"origin\":\"remote_media_endpoint\",\"maximumAgeNano\":" +
        integer(maximum_age_) + ",\"segmentId\":" + quote(c.segment_id) +
        ",\"row\":{\"device\":" + quote(c.device) + ",\"session\":" + quote(c.session) +
        ",\"resource\":" + quote(c.resource) +
        ",\"mediaSessionIdHex\":" + quote(hex(c.media_session_id)) +
        ",\"sourceGeneration\":" + integer(c.source_generation) +
        ",\"nativeKind\":\"single\",\"mediaKind\":\"live\",\"scope\":\"connection\",\"memberSlot\":"
        "0,\"memberGeneration\":"
        "\"0\",\"pathLabelHex\":\"\"}" +
        ",\"pair\":{\"associationGeneration\":" + integer(c.association_generation) +
        ",\"componentGeneration\":" + integer(c.component_generation) +
        ",\"profileRevision\":" + integer(c.profile_revision) +
        ",\"metadataEpoch\":" + integer(c.metadata_epoch) +
        ",\"bindingGeneration\":" + integer(c.binding_generation) + '}' +
        ",\"producer\":{\"role\":\"sender\",\"instance\":" + optional_text(c.producer_instance) +
        ",\"clockDomain\":" + optional_text(c.producer_clock_domain) +
        ",\"provider\":" + optional_text(c.provider) +
        ",\"providerRevision\":" + optional_text(c.provider_revision) +
        ",\"buildProfile\":" + optional_text(c.build_profile) +
        ",\"qualificationId\":" + optional_text(c.qualification_id) +
        ",\"sampling\":" + quote(sampling) + ",\"clearPolicy\":" +
        quote(c.clear_policy == ClearPolicy::exclusive_no_clear ? "exclusiveNoClear" : "unknown") +
        '}' + ",\"observer\":{\"node\":" + optional_text(c.observer_node) +
        ",\"instance\":" + optional_text(c.observer_instance) +
        ",\"clockDomain\":" + quote(c.observer_clock_domain) + '}' +
        ",\"localOverwrittenRecords\":" + integer(overwritten_) +
        ",\"retired\":" + boolean(retired_) + "}\n";
    for (std::size_t n = 0; n < size_; ++n)
    {
        const auto &r = *slots_[(head_ + n) % slots_.size()];
        const auto &o = r.observation;
        out +=
            "{\"type\":\"remoteObservation\",\"schemaVersion\":\"remote-input.draft1\""
            ",\"segmentId\":" +
            quote(c.segment_id) + ",\"historical\":true,\"sequence\":" + integer(o.sequence) +
            ",\"producerMonotonicNano\":" + integer(o.producer_monotonic_nano) +
            ",\"intervalBeginMonotonicNano\":" + optional_integer(o.interval_begin_monotonic_nano) +
            ",\"producerWallTime\":" + render_wall(o.producer_wall_time) +
            ",\"firstReceipt\":{\"monotonicNano\":" + integer(r.first_receipt.monotonic_nano) +
            ",\"wallTime\":" + render_wall(r.first_receipt.wall_time) + '}' +
            ",\"exportedMonotonicNano\":" + integer(now) +
            ",\"receiptAgeNano\":" + integer(now - r.first_receipt.monotonic_nano) +
            ",\"freshness\":" +
            quote(retired_                                              ? "retired"
                  : now - r.first_receipt.monotonic_nano > maximum_age_ ? "stale"
                                                                        : "fresh") +
            ",\"firstInSegment\":" + boolean(r.first_in_segment) +
            ",\"sequenceDiscontinuity\":" + boolean(r.sequence_discontinuity) +
            ",\"equalProducerTime\":" + boolean(r.equal_producer_time) +
            ",\"clearedAfterSample\":" + boolean(o.cleared_after_sample) +
            ",\"native\":{\"namespace\":\"rwx-broadcast-01\",\"type\":5,\"schema\":\"1.0\","
            "\"encoding\":\"rwblHex\",\"payload\":" +
            quote(hex(o.native_rwbl)) + "}}\n";
    }
    return out;
}
} // namespace robotweax::srt::telemetry::remote_draft
