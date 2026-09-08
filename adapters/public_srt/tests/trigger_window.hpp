#pragma once

#include <robotweax/srt_telemetry.hpp>

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string_view>

// Host-owned capture policy. One owner invokes methods; SDK recorder locking is
// unchanged. Re-arming requires a fresh instance / connection epoch.
class TriggerWindow final
{
  public:
    TriggerWindow(std::size_t pre_records, std::size_t capacity, std::uint64_t post_ms)
        : pre_(validate(pre_records, capacity, post_ms)), capture_(capacity),
          post_nano_(post_ms * 1'000'000)
    {
    }

    void trigger(std::uint64_t monotonic_nano)
    {
        if (trigger_count_ != 0)
        {
            if (trigger_count_ != std::numeric_limits<std::uint64_t>::max()) ++trigger_count_;
            return; // Never extend an existing incident's deadline, even after freeze.
        }
        if (frozen_) throw std::invalid_argument("cannot trigger a closed untriggered window");
        if (have_sample_ && monotonic_nano < last_sample_nano_)
            throw std::invalid_argument("trigger precedes latest monotonic sample");
        if (monotonic_nano > std::numeric_limits<std::uint64_t>::max() - post_nano_)
            throw std::invalid_argument("trigger deadline overflow");
        trigger_nano_ = monotonic_nano;
        deadline_nano_ = monotonic_nano + post_nano_;
        trigger_count_ = 1;
        pre_count_ = pre_.size();
        for (std::size_t index = 0; index < pre_count_; ++index)
        {
            const auto record = pre_.read(index);
            capture_.push(record.raw, record.delta);
        }
    }

    void push(const robotweax_srt_telemetry_raw_sample_v1 &raw,
              const robotweax_srt_telemetry_delta_v1 &delta)
    {
        if (frozen_) return;
        if ((have_sample_ && raw.sample_monotonic_nano <= last_sample_nano_) ||
            (trigger_count_ != 0 && raw.sample_monotonic_nano < trigger_nano_))
        {
            finish("monotonic-boundary");
            return;
        }
        have_sample_ = true;
        last_sample_nano_ = raw.sample_monotonic_nano;
        if (trigger_count_ == 0)
        {
            pre_.push(raw, delta);
            return;
        }
        if (raw.sample_monotonic_nano > deadline_nano_)
        {
            finish("deadline");
            return;
        }
        capture_.push(raw, delta);
        if (capture_.size() == capture_.capacity()) finish("capacity");
        else if (raw.sample_monotonic_nano == deadline_nano_) finish("deadline");
    }

    void finish(std::string_view reason)
    {
        if (!frozen_)
        {
            frozen_ = true;
            finish_reason_ = reason;
        }
    }

    const robotweax::srt::telemetry::BoundedRecorder &recording() const noexcept
    { return trigger_count_ == 0 ? pre_ : capture_; }
    bool frozen() const noexcept { return frozen_; }
    std::string_view reason() const noexcept { return finish_reason_; }
    std::uint64_t trigger_count() const noexcept { return trigger_count_; }
    std::uint64_t trigger_nano() const noexcept { return trigger_nano_; }
    std::uint64_t deadline_nano() const noexcept { return deadline_nano_; }
    std::size_t pre_count() const noexcept { return pre_count_; }
    std::uint64_t pre_overwrites() const noexcept { return pre_.overwritten_count(); }
    std::size_t allocated_records() const noexcept { return pre_.capacity() + capture_.capacity(); }

  private:
    static std::size_t validate(std::size_t pre, std::size_t capacity, std::uint64_t post_ms)
    {
        if (pre == 0 || pre >= capacity || capacity > 4096 || post_ms == 0 || post_ms > 60'000)
            throw std::invalid_argument("window requires 0 < pre < capacity <= 4096 and 1..60000 post ms");
        return pre;
    }
    robotweax::srt::telemetry::BoundedRecorder pre_;
    robotweax::srt::telemetry::BoundedRecorder capture_;
    std::uint64_t post_nano_;
    std::uint64_t trigger_nano_ = 0;
    std::uint64_t deadline_nano_ = 0;
    std::uint64_t trigger_count_ = 0;
    std::uint64_t last_sample_nano_ = 0;
    std::size_t pre_count_ = 0;
    bool have_sample_ = false;
    bool frozen_ = false;
    std::string_view finish_reason_ = "open";
};
