/* Impairing UDP relay for SRT test and demo programs: forwards between an
 * SRT caller and listener on loopback, applying loss, bursts, delay, jitter,
 * a rate cap with tail drop, or a blackhole. Used by the fault lab and the
 * dashboard demo. Test support code, not part of the library. */
#ifndef ROBOTWEAX_SRT_TELEMETRY_IMPAIRING_RELAY_HPP
#define ROBOTWEAX_SRT_TELEMETRY_IMPAIRING_RELAY_HPP

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <queue>
#include <random>
#include <thread>
#include <vector>

namespace robotweax_srt_telemetry_lab
{

using Clock = std::chrono::steady_clock;


struct Impairment
{
    double forward_loss = 0;           /* caller -> listener, probability */
    int burst_every_ms = 0;            /* drop a burst every N ms ... */
    int burst_packets = 0;             /* ... of this many forward packets */
    int delay_ms = 10;                 /* each direction */
    int jitter_ms = 0;                 /* extra forward delay, uniform 0..N */
    double forward_rate_bps = 0;       /* 0: unlimited */
    std::size_t queue_limit_bytes = 0; /* tail drop above this backlog */
    bool blackhole = false;            /* both directions */
};

/* Forwards UDP between the SRT caller (front) and listener (back), applying
 * the current impairment. Single thread; packets wait in a release queue. */
class Relay final
{
  public:
    bool start(const sockaddr_in &listener)
    {
        listener_ = listener;
        front_ = ::socket(AF_INET, SOCK_DGRAM, 0);
        back_ = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (front_ < 0 || back_ < 0)
        {
            return false;
        }
        const int buffer = 4 << 20;
        for (const int fd : {front_, back_})
        {
            static_cast<void>(::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buffer, sizeof(buffer)));
            static_cast<void>(::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buffer, sizeof(buffer)));
        }
        sockaddr_in any{};
        any.sin_family = AF_INET;
        any.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::bind(front_, reinterpret_cast<sockaddr *>(&any), sizeof(any)) != 0 ||
            ::bind(back_, reinterpret_cast<sockaddr *>(&any), sizeof(any)) != 0)
        {
            return false;
        }
        socklen_t size = sizeof(address_);
        socklen_t back_size = sizeof(back_address_);
        if (::getsockname(front_, reinterpret_cast<sockaddr *>(&address_), &size) != 0 ||
            ::getsockname(back_, reinterpret_cast<sockaddr *>(&back_address_), &back_size) != 0)
        {
            return false;
        }
        running_ = true;
        thread_ = std::thread([this] { run(); });
        return true;
    }

    ~Relay()
    {
        running_ = false;
        if (thread_.joinable())
        {
            thread_.join();
        }
        if (front_ >= 0)
        {
            ::close(front_);
        }
        if (back_ >= 0)
        {
            ::close(back_);
        }
    }

    [[nodiscard]] const sockaddr_in &address() const noexcept
    {
        return address_;
    }

    /* The address the listener sees this relay's traffic come from. */
    [[nodiscard]] const sockaddr_in &back_address() const noexcept
    {
        return back_address_;
    }

    void set(const Impairment &impairment)
    {
        std::lock_guard lock(mutex_);
        impairment_ = impairment;
    }

    [[nodiscard]] Impairment get()
    {
        std::lock_guard lock(mutex_);
        return impairment_;
    }

  private:
    struct Pending
    {
        Clock::time_point release;
        std::uint64_t order;
        bool forward;
        std::vector<char> data;
        bool operator>(const Pending &other) const
        {
            return release != other.release ? release > other.release : order > other.order;
        }
    };

    void enqueue(const bool forward, const char *data, const std::size_t size)
    {
        const auto now = Clock::now();
        const auto impairment = get();
        if (impairment.blackhole)
        {
            return;
        }
        auto release = now + std::chrono::milliseconds(impairment.delay_ms);
        if (forward)
        {
            if (impairment.forward_loss > 0 && uniform_(random_) < impairment.forward_loss)
            {
                return;
            }
            if (impairment.burst_every_ms > 0)
            {
                if (now >= next_burst_)
                {
                    burst_left_ = impairment.burst_packets;
                    next_burst_ = now + std::chrono::milliseconds(impairment.burst_every_ms);
                }
                if (burst_left_ > 0)
                {
                    --burst_left_;
                    return;
                }
            }
            if (impairment.forward_rate_bps > 0)
            {
                const auto start = std::max(now, link_free_);
                const auto backlog_bytes =
                    std::chrono::duration<double>(start - now).count() * impairment.forward_rate_bps / 8.0;
                if (backlog_bytes + static_cast<double>(size) > static_cast<double>(impairment.queue_limit_bytes))
                {
                    return;
                }
                link_free_ = start + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(
                                         static_cast<double>(size) * 8.0 / impairment.forward_rate_bps));
                release = link_free_ + std::chrono::milliseconds(impairment.delay_ms);
            }
            if (impairment.jitter_ms > 0)
            {
                release += std::chrono::microseconds(
                    static_cast<std::int64_t>(uniform_(random_) * impairment.jitter_ms * 1000.0));
            }
        }
        queue_.push(Pending{release, order_++, forward, std::vector<char>(data, data + size)});
    }

    void flush()
    {
        const auto now = Clock::now();
        while (!queue_.empty() && queue_.top().release <= now)
        {
            const auto &packet = queue_.top();
            if (packet.forward)
            {
                static_cast<void>(::sendto(back_, packet.data.data(), packet.data.size(), 0,
                                           reinterpret_cast<const sockaddr *>(&listener_), sizeof(listener_)));
            }
            else if (has_caller_)
            {
                static_cast<void>(::sendto(front_, packet.data.data(), packet.data.size(), 0,
                                           reinterpret_cast<const sockaddr *>(&caller_), sizeof(caller_)));
            }
            queue_.pop();
        }
    }

    void run()
    {
        std::array<char, 2048> buffer{};
        while (running_)
        {
            int timeout = 1;
            if (queue_.empty())
            {
                timeout = 5;
            }
            pollfd fds[2] = {{front_, POLLIN, 0}, {back_, POLLIN, 0}};
            static_cast<void>(::poll(fds, 2, timeout));
            if ((fds[0].revents & POLLIN) != 0)
            {
                sockaddr_in from{};
                socklen_t size = sizeof(from);
                const auto n = ::recvfrom(front_, buffer.data(), buffer.size(), MSG_DONTWAIT,
                                          reinterpret_cast<sockaddr *>(&from), &size);
                if (n > 0)
                {
                    caller_ = from;
                    has_caller_ = true;
                    enqueue(true, buffer.data(), static_cast<std::size_t>(n));
                }
            }
            if ((fds[1].revents & POLLIN) != 0)
            {
                const auto n = ::recv(back_, buffer.data(), buffer.size(), MSG_DONTWAIT);
                if (n > 0)
                {
                    enqueue(false, buffer.data(), static_cast<std::size_t>(n));
                }
            }
            flush();
        }
    }

    int front_ = -1;
    int back_ = -1;
    sockaddr_in address_{};
    sockaddr_in back_address_{};
    sockaddr_in listener_{};
    sockaddr_in caller_{};
    bool has_caller_ = false;
    std::atomic<bool> running_{false};
    std::thread thread_;
    std::mutex mutex_;
    Impairment impairment_;
    std::priority_queue<Pending, std::vector<Pending>, std::greater<>> queue_;
    std::uint64_t order_ = 0;
    Clock::time_point link_free_{};
    Clock::time_point next_burst_{};
    int burst_left_ = 0;
    std::mt19937_64 random_{20261001};
    std::uniform_real_distribution<double> uniform_{0.0, 1.0};
};

} // namespace robotweax_srt_telemetry_lab

#endif
