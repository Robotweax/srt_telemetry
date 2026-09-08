#include "trigger_window.hpp"

#include <cstdlib>
#include <iostream>
#include <stdexcept>

void require(bool condition, const char *message)
{
    if (!condition) throw std::runtime_error(message);
}

void push(TriggerWindow &window, std::uint64_t nano)
{
    robotweax_srt_telemetry_raw_sample_v1 raw{};
    raw.struct_size = sizeof(raw);
    raw.abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
    raw.sample_monotonic_nano = nano;
    robotweax_srt_telemetry_delta_v1 delta{};
    delta.struct_size = sizeof(delta);
    delta.abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
    window.push(raw, delta);
}

int main()
{
    try
    {
        TriggerWindow window(3, 6, 2);
        for (std::uint64_t index = 1; index <= 10; ++index) push(window, index * 1'000'000);
        window.trigger(10'000'000);
        window.trigger(11'000'000);
        require(window.deadline_nano() == 12'000'000 && window.trigger_count() == 2,
                "repeated trigger extended deadline");
        require(window.pre_count() == 3 && window.pre_overwrites() == 7, "pre bound");
        require(window.recording().read(0).raw.sample_monotonic_nano == 8'000'000, "pre window");
        push(window, 11'000'000);
        push(window, 12'000'000);
        push(window, 13'000'000);
        require(window.frozen() && window.reason() == "deadline" && window.recording().size() == 5,
                "post deadline");
        window.trigger(100'000'000);
        require(window.trigger_count() == 3 && window.recording().size() == 5, "frozen coalescing");
        require(window.allocated_records() == 9, "allocation bound");

        TriggerWindow capacity(1, 2, 60'000);
        push(capacity, 1);
        capacity.trigger(1);
        push(capacity, 2);
        push(capacity, 3);
        require(capacity.reason() == "capacity" && capacity.recording().size() == 2 &&
                    capacity.recording().overwritten_count() == 0, "post overflow overwrote pre");

        TriggerWindow boundary(1, 3, 1);
        push(boundary, 10);
        boundary.trigger(10);
        push(boundary, 9);
        require(boundary.reason() == "monotonic-boundary", "clock regression boundary");
        TriggerWindow trigger_clock(1, 3, 1);
        push(trigger_clock, 1);
        trigger_clock.trigger(10);
        push(trigger_clock, 9);
        require(trigger_clock.reason() == "monotonic-boundary", "sample precedes trigger clock");
        TriggerWindow closed(1, 3, 1);
        push(closed, 1);
        closed.finish("connection-ended");
        bool closed_rejected = false;
        try { closed.trigger(2); }
        catch (const std::invalid_argument &) { closed_rejected = true; }
        require(closed_rejected, "closed untriggered window accepted trigger");
        TriggerWindow next_epoch(1, 3, 1);
        push(next_epoch, 1);
        require(next_epoch.trigger_count() == 0 && next_epoch.recording().size() == 1,
                "epoch carries previous trigger or records");
        bool rejected = false;
        try { TriggerWindow invalid(4096, 4097, 1); }
        catch (const std::invalid_argument &) { rejected = true; }
        require(rejected, "resource guard");
        TriggerWindow truncated(1, 3, 100);
        push(truncated, 1);
        truncated.trigger(1);
        truncated.finish("connection-ended");
        require(truncated.reason() == "connection-ended", "truncation reason");
        std::cout << "trigger window: pre/post, coalescing, capacity, clock and epoch boundaries passed\n";
        return EXIT_SUCCESS;
    }
    catch (const std::exception &error)
    {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
