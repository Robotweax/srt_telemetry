#include "remote_input_draft.hpp"
#include <atomic>
#include <charconv>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <source_location>
#include <stdexcept>
#include <string_view>
#include <thread>
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
    c.producer_id = "producer-fixture";
    c.source_id = "source/\"back\\slash";
    c.role = d::EndpointRole::sender;
    c.origin = {"fixture-origin", "fixture/9.7", {0x00, 0xff, 0x22, 0x5c}};
    c.observer_node = "cloud-fixture";
    c.observer_instance = "cloud-process-1";
    c.observer_clock_domain = "cloud-monotonic-1";
    return c;
}
d::Observation sample(std::uint64_t sequence = 1, std::uint64_t producer = 1000)
{
    return {sequence, producer, 750, {}, {0x83, 0x00, 0xff, 0x00}};
}
void lifecycle_and_bounds()
{
    d::Recorder r(context(), 2, 100);
    check(r.latest(1).status == d::Status::no_sample);
    auto value = sample(10);
    check(r.push(value, {10, {}}) == d::PushResult::accepted);
    auto first = r.latest(11);
    check(first.record && first.record->first_in_segment);
    value.origin_payload[0] = 0;
    check(r.read_history(0).observation.origin_payload[0] == 0x83);
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
    c.origin.context.push_back(1);
    d::Recorder replacement(c);
    check(replacement.push(sample(1), {130, {}}) == d::PushResult::accepted);
    check(replacement.latest(131).record->first_in_segment);
    check(r.latest(132).status == d::Status::retired);
}
void validation()
{
    const char *fields[] = {
        "segment_id",          "producer_id",       "source_id",      "role",
        "origin.namespace_id", "origin.schema",     "origin.context", "observer_instance",
        "observer_node",       "producer_instance", "source_id",      "observer_clock_domain"};
    for (unsigned n = 0; n != 12; ++n)
    {
        auto c = context();
        if (n == 0)
            c.segment_id.clear();
        if (n == 1)
            c.producer_id.clear();
        if (n == 2)
            c.source_id.clear();
        if (n == 3)
            c.role = static_cast<d::EndpointRole>(999);
        if (n == 4)
            c.origin.namespace_id.clear();
        if (n == 5)
            c.origin.schema.clear();
        if (n == 6)
            c.origin.context.resize(4097);
        if (n == 7)
            c.observer_instance = "null";
        if (n == 8)
            c.observer_node = "a/b";
        if (n == 9)
            c.producer_instance = "";
        if (n == 10)
            c.source_id = std::string(513, 'a');
        if (n == 11)
            c.observer_clock_domain = "embedded\nnewline";
        bool caught = false;
        try
        {
            d::Recorder bad(c);
        }
        catch (const d::ValidationError &e)
        {
            caught = e.field() == fields[n] &&
                     std::string(e.what()).find(fields[n]) != std::string::npos;
        }
        check(caught);
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
            s.origin_payload.clear();
        if (n == 4)
            s.origin_payload.resize(16385);
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
    // Reads before the receipt see it with age 0; only a zero clock is refused.
    check(r.latest(9).status == d::Status::available);
    check(r.export_history_jsonl(9).find("\"receiptAgeNano\":\"0\"") != std::string::npos);
    rejects([&] { r.latest(0); });
    rejects([&] { r.export_history_jsonl(0); });
    // Writers stay ordered: a receipt before the last accepted one is refused.
    rejects([&] { r.push(sample(2), {9, {}}); });
    check(r.size() == 1 && r.overwritten_count() == 0);
    (void)r.latest(50);
    check(r.latest(49).status == d::Status::available);
    check(!r.export_history_jsonl(49).empty());
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
    c.producer_id = "mutated-after-construction";
    check(recorder.context().producer_id == "producer-fixture");
    auto value = sample();
    value.origin_payload.resize(16384, 255);
    check(recorder.push(value, {10, {}}) == d::PushResult::accepted);
    auto text = recorder.export_history_jsonl(11);
    check(text.find("\"node\":null,\"instance\":null") != std::string::npos);
    check(recorder.read_history(0).observation.origin_payload.size() == 16384);
    auto unknown = context();
    unknown.role = d::EndpointRole::unknown;
    unknown.origin = {"unrecognized-producer", "arbitrary-version/99.123", {}};
    d::Recorder neutral(unknown);
    check(neutral.push(sample(), {10, {}}) == d::PushResult::accepted);
    check(neutral.export_history_jsonl(11).find("\"role\":null") != std::string::npos);
    auto receiver = context();
    receiver.role = d::EndpointRole::receiver;
    d::Recorder other(receiver);
    check(other.push(sample(), {10, {}}) == d::PushResult::accepted);
}
void independent_readers_and_late_sequences()
{
    d::Recorder r(context(), 2, 10000);
    const auto one = sample(1), two = sample(2);
    check(r.push(one, {10, {}}) == d::PushResult::accepted);
    check(r.push(two, {11, {}}) == d::PushResult::accepted);
    check(r.push(one, {12, {}}) == d::PushResult::stale_sequence);
    auto different = one;
    different.origin_payload[0] ^= 1;
    check(r.push(different, {13, {}}) == d::PushResult::stale_sequence);
    check(r.size() == 2 && r.read_history(0).first_receipt.monotonic_nano == 10);
    check(r.latest(5000).record->first_receipt.monotonic_nano == 11);
    check(r.latest(4999).status == d::Status::available);
    check(!r.export_history_jsonl(4999).empty());
    // A reader that took its clock before a later push was accepted reads
    // that record with age 0: fresh, never an error.
    {
        d::Recorder late(context(), 2, 100);
        check(late.push(one, {1000, {}}) == d::PushResult::accepted);
        const auto current = late.latest(900);
        check(current.status == d::Status::available && current.record->first_receipt.monotonic_nano == 1000);
        const auto exported = late.export_history_jsonl(900);
        check(exported.find("\"receiptAgeNano\":\"0\"") != std::string::npos &&
              exported.find("\"freshness\":\"fresh\"") != std::string::npos);
        bool refused = false;
        try
        {
            static_cast<void>(late.latest(0));
        }
        catch (const d::ValidationError &e)
        {
            refused = e.field() == "exported_monotonic_nano";
        }
        check(refused);
    }
    // No writer or retirement occurs until both readers join.
    std::atomic<bool> failed{false};
    std::thread a(
        [&]
        {
            try
            {
                for (unsigned i = 0; i < 40; ++i)
                    if (r.latest(5000 + i).record->first_receipt.monotonic_nano != 11)
                        failed = true;
            }
            catch (...)
            {
                failed = true;
            }
        });
    std::thread b(
        [&]
        {
            try
            {
                for (unsigned i = 0; i < 40; ++i)
                    if (r.export_history_jsonl(4900 + i).empty())
                        failed = true;
            }
            catch (...)
            {
                failed = true;
            }
        });
    a.join();
    b.join();
    check(!failed);
    check(r.read_history(1).first_receipt.monotonic_nano == 11 && r.overwritten_count() == 0);
}
std::vector<std::uint8_t> read_bytes(const char *path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
        throw std::runtime_error("test origin file unavailable");
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
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
        if ((argc == 6 || argc == 11) && std::string(argv[1]) == "--opaque-origin")
        {
            auto c = context();
            c.origin = {argv[2], argv[3], read_bytes(argv[4])};
            d::Recorder r(c);
            auto value = sample();
            value.origin_payload = read_bytes(argv[5]);
            std::uint64_t receipt = 10, exported = 11;
            if (argc == 11)
            {
                const auto integer = [](const char *arg)
                {
                    std::string_view text(arg);
                    std::uint64_t number{};
                    const auto parsed =
                        std::from_chars(text.data(), text.data() + text.size(), number);
                    if (text.empty() || (text.size() > 1 && text[0] == '0') ||
                        parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
                        throw std::runtime_error("noncanonical test integer");
                    return number;
                };
                value.sequence = integer(argv[6]);
                value.producer_monotonic_nano = integer(argv[7]);
                value.interval_begin_monotonic_nano = integer(argv[8]);
                receipt = integer(argv[9]);
                exported = integer(argv[10]);
            }
            if (r.push(value, {receipt, {}}) != d::PushResult::accepted)
                throw std::runtime_error("test origin setup");
            std::cout << r.export_history_jsonl(exported);
            return 0;
        }
        static_assert(!std::is_copy_constructible_v<d::Recorder>);
        lifecycle_and_bounds();
        validation();
        absent_context_and_limits();
        independent_readers_and_late_sequences();
        std::cout << "PASS " << checks
                  << " remote draft assertions; no rate/UI/runtime qualification\n";
    }
    catch (const std::exception &e)
    {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
