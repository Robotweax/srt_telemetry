#include "remote_input_draft.hpp"
#include <iostream>
#include <limits>
#include <source_location>
#include <stdexcept>
#include <type_traits>

namespace d = robotweax::srt::telemetry::remote_draft;
namespace
{
unsigned checks;
constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
void check(bool ok, std::source_location where = std::source_location::current())
{
    ++checks;
    if (!ok)
        throw std::runtime_error("remote-input test line " + std::to_string(where.line()));
}
template <class Fn> void rejects(Fn fn)
{
    bool caught = false;
    try
    {
        fn();
    }
    catch (const std::invalid_argument &)
    {
        caught = true;
    }
    check(caught);
}
d::Context context()
{
    d::Context c;
    c.segment_id = "cloud-segment-1";
    c.device = "Kq2x81a0kfdT7mZ3pLw9Ab";
    c.session = "Q3vXn0b8cR2hY6tJ1sWe4A";
    c.resource = "camera/\"back\\slash";
    c.role = "sender";
    c.media_kind = "live";
    c.scope = "connection";
    c.media_session_id.fill(255);
    c.source_generation = maximum;
    c.association_generation = maximum - 1;
    c.component_generation = maximum - 2;
    c.profile_revision = maximum - 3;
    c.metadata_epoch = maximum - 4;
    c.binding_generation = maximum - 5;
    c.observer_node = "cloud-fixture";
    c.observer_instance = "cloud-process-1";
    c.observer_clock_domain = "cloud-monotonic-1";
    c.sampling = d::Sampling::averaged;
    return c;
}
d::Observation sample(std::uint64_t sequence = 1, std::uint64_t producer = 1000)
{
    return {sequence, producer, 750, {}, false, {0x83, 0x00, 0xff, 0x00}};
}
void lifecycle_and_bounds()
{
    d::Recorder r(context(), 2, 100);
    check(r.latest(1).status == d::Status::no_sample);
    auto value = sample(10);
    check(r.push(value, {10, {}}) == d::PushResult::accepted);
    auto first = r.latest(11);
    check(first.record && first.record->first_in_segment);
    value.native_rwbl[0] = 0;
    check(r.read_history(0).observation.native_rwbl[0] == 0x83);
    check(r.push(sample(10), {20, {}}) == d::PushResult::duplicate);
    check(r.latest(110).status == d::Status::available);
    check(r.latest(111).status == d::Status::stale);
    check(r.read_history(0).first_receipt.monotonic_nano == 10);
    check(r.push(value, {121, {}}) == d::PushResult::conflict);
    check(r.push(sample(9), {122, {}}) == d::PushResult::stale_sequence);
    check(r.push(sample(11, 999), {123, {}}) == d::PushResult::producer_clock_regression);
    check(r.push(sample(12), {124, {}}) == d::PushResult::accepted);
    auto equal = r.latest(125);
    check(equal.record && equal.record->sequence_discontinuity &&
          equal.record->equal_producer_time);
    check(r.push(sample(13, 1100), {126, {}}) == d::PushResult::accepted);
    check(r.size() == 2 && r.capacity() == 2 && r.overwritten_count() == 1);
    check(r.read_history(0).observation.sequence == 12);
    check(!r.read_history(0).first_in_segment);
    bool outside = false;
    try
    {
        r.read_history(2);
    }
    catch (const std::out_of_range &)
    {
        outside = true;
    }
    check(outside);
    r.retire();
    check(r.latest(127).status == d::Status::retired);
    check(r.push(sample(14, 1200), {128, {}}) == d::PushResult::retired);
    check(r.export_history_jsonl(129).find("\"freshness\":\"retired\"") != std::string::npos);
    check(first.record->observation.sequence == 10 &&
          first.record->first_receipt.monotonic_nano == 10);
    auto c = context();
    ++c.segment_id.back();
    --c.metadata_epoch;
    d::Recorder replacement(c);
    check(replacement.push(sample(1), {130, {}}) == d::PushResult::accepted);
    check(replacement.latest(131).record->first_in_segment);
    check(r.latest(132).status == d::Status::retired);
}
void validation()
{
    for (unsigned n = 0; n != 15; ++n)
    {
        auto c = context();
        if (n == 0)
            c.role = "receiver";
        if (n == 1)
            c.role = "unknown";
        if (n == 2)
            c.media_kind = "play";
        if (n == 3)
            c.scope = "member";
        if (n == 4)
            c.member_slot = 1;
        if (n == 5)
            c.member_generation = 1;
        if (n == 6)
            c.path_label = {1};
        if (n == 7)
            c.source_generation = 0;
        if (n == 8)
            c.media_session_id.fill(0);
        if (n == 9)
            c.observer_instance = "null";
        if (n == 10)
            c.observer_node = "a/b";
        if (n == 11)
            c.producer_instance = "";
        if (n == 12)
            c.resource = std::string(513, 'a');
        if (n == 13)
            c.provider = "guessed";
        if (n == 14)
            c.device = "embedded\nnewline";
        rejects([&] { d::Recorder bad(c); });
    }
    rejects([&] { d::Recorder bad(context(), 0); });
    rejects([&] { d::Recorder bad(context(), 257); });
    rejects([&] { d::Recorder bad(context(), 1, 0); });
    rejects([&] { d::Recorder bad(context(), 1, 60'000'000'001); });
    d::Recorder r(context());
    for (unsigned n = 0; n != 9; ++n)
    {
        auto s = sample();
        d::Receipt receipt{10, {}};
        if (n == 0)
            s.sequence = 0;
        if (n == 1)
            s.producer_monotonic_nano = 0;
        if (n == 2)
            s.interval_begin_monotonic_nano = 1001;
        if (n == 3)
            s.native_rwbl.clear();
        if (n == 4)
            s.native_rwbl.resize(16385);
        if (n == 5)
            receipt.monotonic_nano = 0;
        if (n == 6)
            s.producer_wall_time = d::WallTime{0, d::Synchronization::synchronized, {}, "ptp"};
        if (n == 7)
            s.producer_wall_time = d::WallTime{0, d::Synchronization::unknown, 1, "unqualified"};
        if (n == 8)
            receipt.wall_time = d::WallTime{0, d::Synchronization::unsynchronized, {}, ""};
        rejects([&] { r.push(s, receipt); });
        check(r.size() == 0);
    }
    check(r.push(sample(), {10, {}}) == d::PushResult::accepted);
    rejects([&] { r.latest(9); });
    rejects([&] { r.export_history_jsonl(9); });
    rejects([&] { r.push(sample(2), {9, {}}); });
    check(r.size() == 1 && r.overwritten_count() == 0);
    (void)r.latest(50);
    rejects([&] { r.latest(49); });
    rejects([&] { r.export_history_jsonl(49); });
    check(r.latest(50).record->first_receipt.monotonic_nano == 10);
    d::Recorder max(context(), 1, 60'000'000'000);
    auto s = sample(maximum, maximum);
    s.interval_begin_monotonic_nano.reset();
    check(max.push(s, {maximum, {}}) == d::PushResult::accepted);
    check(max.latest(maximum).status == d::Status::available);
    check(max.export_history_jsonl(maximum).find("18446744073709551615") != std::string::npos);
}
void absent_context_and_limits()
{
    auto c = context();
    c.observer_node.reset();
    c.observer_instance.reset();
    d::Recorder recorder(c, 256);
    c.device = "mutated-after-construction";
    check(recorder.context().device == "Kq2x81a0kfdT7mZ3pLw9Ab");
    auto value = sample();
    value.native_rwbl.resize(16384, 255);
    check(recorder.push(value, {10, {}}) == d::PushResult::accepted);
    auto text = recorder.export_history_jsonl(11);
    check(text.find("\"node\":null,\"instance\":null") != std::string::npos);
    check(recorder.read_history(0).observation.native_rwbl.size() == 16384);
    auto empty_role = context();
    empty_role.role.clear();
    rejects([&] { d::Recorder invalid(empty_role); });
    auto bad_mode = context();
    bad_mode.sampling = static_cast<d::Sampling>(999);
    rejects([&] { d::Recorder invalid(bad_mode); });
    bad_mode = context();
    bad_mode.clear_policy = static_cast<d::ClearPolicy>(999);
    rejects([&] { d::Recorder invalid(bad_mode); });
    auto exclusive = context();
    exclusive.clear_policy = d::ClearPolicy::exclusive_no_clear;
    d::Recorder no_clear(exclusive);
    auto cleared = sample();
    cleared.cleared_after_sample = true;
    rejects([&] { no_clear.push(cleared, {10, {}}); });
    check(no_clear.size() == 0);
}
void output_golden()
{
    d::Recorder r(context(), 3, 100);
    auto a = sample(maximum - 1, maximum - 2);
    if (r.push(a, {50, {}}) != d::PushResult::accepted)
        throw std::runtime_error("golden setup");
    auto b = sample(maximum, maximum - 1);
    b.producer_wall_time =
        d::WallTime{0, d::Synchronization::unsynchronized, {}, "producer-clock-fixture"};
    d::Receipt receipt{
        51, d::WallTime{maximum, d::Synchronization::synchronized, 5, "cloud-ptp-fixture"}};
    if (r.push(b, receipt) != d::PushResult::accepted)
        throw std::runtime_error("golden setup");
    // Different new Cloud wall time must not refresh a duplicate's first receipt.
    receipt.monotonic_nano = 60;
    receipt.wall_time.reset();
    if (r.push(b, receipt) != d::PushResult::duplicate)
        throw std::runtime_error("golden duplicate");
    std::cout << r.export_history_jsonl(70);
}
} // namespace
int main(int argc, char **argv)
{
    try
    {
        if (argc == 2 && std::string(argv[1]) == "--jsonl")
        {
            output_golden();
            return 0;
        }
        static_assert(!std::is_copy_constructible_v<d::Recorder>);
        lifecycle_and_bounds();
        validation();
        absent_context_and_limits();
        std::cout << "PASS " << checks
                  << " remote draft assertions; no rate/UI/runtime qualification\n";
    }
    catch (const std::exception &e)
    {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
