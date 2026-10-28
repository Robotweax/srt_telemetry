#include "numeric_input_draft.hpp"
#include "../../sdk/src/counter_engine.hpp"
#include <bit>
#include <cmath>
#include <type_traits>

namespace robotweax::srt::telemetry::remote_draft
{
namespace math = robotweax_srt_telemetry_internal::counters;
static_assert(std::is_nothrow_copy_assignable_v<math::State>);
static_assert(std::is_nothrow_copy_assignable_v<std::optional<NumericValues>>);
static_assert(std::is_nothrow_copy_constructible_v<NumericResult>);
namespace
{
constexpr std::uint64_t raw_known = (UINT64_C(1) << 27) - 1;
constexpr std::uint64_t ext_known = (UINT64_C(1) << 26) - 1;
constexpr std::uint64_t raw_queues = ROBOTWEAX_SRT_TELEMETRY_FIELD_AVAILABLE_SEND_BUFFER_BYTES |
                                     ROBOTWEAX_SRT_TELEMETRY_FIELD_SEND_BUFFER_BYTES |
                                     ROBOTWEAX_SRT_TELEMETRY_FIELD_SEND_BUFFER_SECONDS |
                                     ROBOTWEAX_SRT_TELEMETRY_FIELD_RECEIVE_BUFFER_BYTES |
                                     ROBOTWEAX_SRT_TELEMETRY_FIELD_RECEIVE_BUFFER_SECONDS;
constexpr std::uint64_t ext_queues = ROBOTWEAX_SRT_TELEMETRY_EXT_FIELD_SEND_BUFFER_PACKETS |
                                     ROBOTWEAX_SRT_TELEMETRY_EXT_FIELD_RECEIVE_BUFFER_PACKETS;
constexpr std::uint64_t since_clear =
    ROBOTWEAX_SRT_TELEMETRY_EXT_FIELD_RECEIVED_RETRANSMITTED_PACKET_SINCE_CLEAR |
    ROBOTWEAX_SRT_TELEMETRY_EXT_FIELD_BELATED_PACKET_SINCE_CLEAR;
void require(bool ok, const char *field)
{
    if (!ok)
        throw ValidationError(field);
}
void policy(const Context &c, const NumericQualification &q)
{
    require(!(q.raw_fields & ~raw_known), "qualification.raw_fields");
    require(!(q.extended_fields & ~ext_known), "qualification.extended_fields");
    require(q.provider == ROBOTWEAX_SRT_TELEMETRY_PROVIDER_UNKNOWN ||
                q.provider == ROBOTWEAX_SRT_TELEMETRY_PROVIDER_ROBOTWEAX_SRT ||
                q.provider == ROBOTWEAX_SRT_TELEMETRY_PROVIDER_HAIVISION_SRT,
            "qualification.provider");
    require(q.id.size() <= 128, "qualification.id");
    for (const char ch : q.id)
        require(ch >= 32 && ch <= 126, "qualification.id");
    if (q.raw_fields || q.extended_fields)
    {
        require(!q.id.empty(), "qualification.id");
        require(q.provider != ROBOTWEAX_SRT_TELEMETRY_PROVIDER_UNKNOWN, "qualification.provider");
        require(c.role != EndpointRole::unknown, "qualification.role");
    }
    require(q.queue_sampling == GaugeSampling::unknown ||
                q.queue_sampling == GaugeSampling::averaged ||
                q.queue_sampling == GaugeSampling::instantaneous,
            "qualification.queue_sampling");
    if ((q.raw_fields & raw_queues) || (q.extended_fields & ext_queues))
        require(q.queue_sampling == GaugeSampling::instantaneous, "qualification.queue_sampling");
    require(q.since_clear == SinceClearPolicy::unknown ||
                q.since_clear == SinceClearPolicy::exclusive_no_clear ||
                q.since_clear == SinceClearPolicy::explicit_boundary,
            "qualification.since_clear");
    if (q.extended_fields &
        ROBOTWEAX_SRT_TELEMETRY_EXT_FIELD_ESTIMATED_LINK_CAPACITY_BITS_PER_SECOND)
        require(q.provider == ROBOTWEAX_SRT_TELEMETRY_PROVIDER_HAIVISION_SRT,
                "qualification.extended_fields.provider");
    if (q.extended_fields & ROBOTWEAX_SRT_TELEMETRY_EXT_FIELD_DATA_ARRIVAL_RATE_BITS_PER_SECOND)
        require(q.provider == ROBOTWEAX_SRT_TELEMETRY_PROVIDER_ROBOTWEAX_SRT,
                "qualification.extended_fields.provider");
}
template <class T> bool same(T a, T b)
{
    if constexpr (std::is_same_v<T, double>)
        return std::bit_cast<std::uint64_t>(a) == std::bit_cast<std::uint64_t>(b);
    else
        return a == b;
}
bool same_values(const NumericValues &a, const NumericValues &b)
{
    if (a.raw.available_fields != b.raw.available_fields ||
        a.extended.available_fields != b.extended.available_fields ||
        a.clear_boundary != b.clear_boundary)
        return false;
#define RWX_NUMERIC_FIELD(type, member, bit)                                                       \
    if (!same(a.raw.member, b.raw.member))                                                         \
        return false;
#include "../../sdk/src/raw_numeric_fields.inc"
#undef RWX_NUMERIC_FIELD
#define RWX_NUMERIC_FIELD(type, member, bit)                                                       \
    if (!same(a.extended.member, b.extended.member))                                               \
        return false;
#include "../../sdk/src/ext_numeric_fields.inc"
#undef RWX_NUMERIC_FIELD
    return true;
}
NumericValues qualify(const NumericValues &in, const NumericQualification &q)
{
    require(!(in.raw.available_fields & ~raw_known), "values.raw.available_fields");
    require(!(in.extended.available_fields & ~ext_known), "values.extended.available_fields");
    NumericValues out;
    out.raw.available_fields = in.raw.available_fields & q.raw_fields;
    out.extended.available_fields = in.extended.available_fields & q.extended_fields;
    out.clear_boundary = in.clear_boundary;
    const auto finite = [](auto value)
    {
        if constexpr (std::is_same_v<decltype(value), double>)
            return std::isfinite(value) && value >= 0;
        else
            return true;
    };
#define RWX_NUMERIC_FIELD(type, member, bit)                                                       \
    if (in.raw.available_fields & bit)                                                             \
        require(finite(in.raw.member), "values.raw." #member);                                     \
    if (out.raw.available_fields & bit)                                                            \
        out.raw.member = in.raw.member;
#include "../../sdk/src/raw_numeric_fields.inc"
#undef RWX_NUMERIC_FIELD
#define RWX_NUMERIC_FIELD(type, member, bit)                                                       \
    if (in.extended.available_fields & bit)                                                        \
        require(finite(in.extended.member), "values.extended." #member);                           \
    if (out.extended.available_fields & bit)                                                       \
        out.extended.member = in.extended.member;
#include "../../sdk/src/ext_numeric_fields.inc"
#undef RWX_NUMERIC_FIELD
    return out;
}
} // namespace
struct NumericProcessor::Impl
{
    Recorder recorder;
    NumericQualification qualification;
    math::State state;
    std::optional<NumericValues> previous;
    bool retired{};
    Impl(Context c, NumericQualification q) : recorder(std::move(c), 1), qualification(std::move(q))
    {
        policy(recorder.context(), qualification);
    }
};
NumericProcessor::NumericProcessor(Context c, NumericQualification q)
    : impl_(std::make_unique<Impl>(std::move(c), std::move(q)))
{
}
NumericProcessor::~NumericProcessor() = default;
NumericResult NumericProcessor::push(const Observation &observation, const Receipt &receipt,
                                     const NumericValues &values)
{
    auto &s = *impl_;
    if (s.retired)
        return {PushResult::retired, {}};
    // These are alternative evidence modes. Accepting a clear-boundary token
    // alongside an exclusive-no-clear promise could hide a proven clear and
    // derive an invalid since-clear delta. Reject before any publication.
    require(s.qualification.since_clear != SinceClearPolicy::exclusive_no_clear ||
                !values.clear_boundary,
            "values.clear_boundary");
    auto qualified = qualify(values, s.qualification);
    auto next = s.state;
    const bool boundary_changed = s.previous && s.previous->clear_boundary &&
                                  values.clear_boundary &&
                                  s.previous->clear_boundary != values.clear_boundary;
    const bool clear_known =
        s.previous && (s.qualification.since_clear == SinceClearPolicy::exclusive_no_clear ||
                       (s.qualification.since_clear == SinceClearPolicy::explicit_boundary &&
                        s.previous->clear_boundary && values.clear_boundary &&
                        s.previous->clear_boundary == values.clear_boundary));
    ProcessedNumeric result;
    result.sequence = observation.sequence;
    result.producer_monotonic_nano = observation.producer_monotonic_nano;
    result.qualified_values = qualified;
    const auto ext = math::ext_point(qualified.extended);
    math::update(next,
                 math::point(qualified.raw, static_cast<int>(s.qualification.provider),
                             observation.producer_monotonic_nano),
                 &ext, result.delta, &result.extended_delta,
                 clear_known ? ROBOTWEAX_SRT_TELEMETRY_EXT_COUNTER_FIELDS
                             : ROBOTWEAX_SRT_TELEMETRY_EXT_COUNTER_FIELDS & ~since_clear);
    result.clear_boundary_changed =
        s.qualification.since_clear == SinceClearPolicy::explicit_boundary && boundary_changed;
    result.since_clear_delta_eligible = clear_known && result.delta.interval_nano != 0;
    // All potentially throwing validation/allocation remains before numeric
    // publication. Recorder publishes its owned original bytes atomically.
    const auto status = s.recorder.push(observation, receipt);
    if (status == PushResult::duplicate && s.previous && !same_values(*s.previous, values))
        return {PushResult::conflict, {}};
    if (status != PushResult::accepted)
        return {status, {}};
    // Fixed-size scalar state/outputs: no allocation or callback after acceptance.
    s.state = next;
    s.previous = values;
    return {status, result};
}
void NumericProcessor::retire() noexcept
{
    impl_->retired = true;
    impl_->recorder.retire();
}
Current NumericProcessor::latest(std::uint64_t now) const
{
    return impl_->recorder.latest(now);
}
std::string NumericProcessor::export_history_jsonl(std::uint64_t now) const
{
    return impl_->recorder.export_history_jsonl(now);
}
const Context &NumericProcessor::context() const noexcept
{
    return impl_->recorder.context();
}
const NumericQualification &NumericProcessor::qualification() const noexcept
{
    return impl_->qualification;
}
} // namespace robotweax::srt::telemetry::remote_draft
