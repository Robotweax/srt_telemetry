// Group session tests with injected srt_group_data, srt_bistats,
// srt_getsockname and srt_getsockstate (no SRT runtime).
#include <robotweax/srt_telemetry_public_srt_group.h>

#if __has_include(<srt/srt.h>)
#include <srt/srt.h>
#else
#include <srt.h>
#endif

#include "../src/public_srt_path_data.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <atomic>
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

/* Allocation failure injection: the n-th allocation after arming throws. */
namespace
{
std::atomic<long> allocations_until_failure{0};
} // namespace

void *operator new(std::size_t size)
{
    if (allocations_until_failure.load() > 0 && allocations_until_failure.fetch_sub(1) == 1)
    {
        throw std::bad_alloc();
    }
    if (void *memory = std::malloc(size == 0 ? 1 : size))
    {
        return memory;
    }
    throw std::bad_alloc();
}

void operator delete(void *memory) noexcept
{
    std::free(memory);
}

void operator delete(void *memory, std::size_t) noexcept
{
    std::free(memory);
}

void *operator new[](std::size_t size)
{
    return ::operator new(size);
}

void *operator new(std::size_t size, const std::nothrow_t &) noexcept
{
    try
    {
        return ::operator new(size);
    }
    catch (const std::bad_alloc &)
    {
        return nullptr;
    }
}

void *operator new[](std::size_t size, const std::nothrow_t &) noexcept
{
    return ::operator new(size, std::nothrow);
}

void operator delete(void *memory, const std::nothrow_t &) noexcept
{
    std::free(memory);
}

void operator delete[](void *memory, const std::nothrow_t &) noexcept
{
    std::free(memory);
}

void operator delete[](void *memory) noexcept
{
    std::free(memory);
}

void operator delete[](void *memory, std::size_t) noexcept
{
    std::free(memory);
}

namespace
{

static_assert(std::is_same_v<decltype(&srt_group_data),
                             robotweax_srt_telemetry_public_srt_group_data_fn>);
static_assert(std::is_same_v<decltype(&srt_getsockname),
                             robotweax_srt_telemetry_public_srt_getsockname_fn>);
static_assert(std::is_same_v<decltype(&srt_bistats), robotweax_srt_telemetry_public_srt_bistats_fn>);

int failures = 0;

void check(const bool condition, const std::string_view message)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

constexpr std::int32_t group_id = (INT32_C(1) << 30) | 77;
constexpr std::uint64_t base_ns = UINT64_C(1'000'000'000);
constexpr std::uint64_t tick_ns = UINT64_C(100'000'000);

/* ------------------------------------------------------------ fake SRT */

struct FakeMember
{
    SRT_SOCKGROUPDATA data{};
    /* Counters; bistats fails while stats_ok is false. */
    std::int64_t sent = 0;
    std::int64_t received = 0;
    bool stats_ok = true;
    std::string local_ip;
    int local_port = 0;
    bool name_ok = true;
    /* Like Haivision SRT before the peer is known: peeraddr is not written. */
    bool peer_unknown = false;
};

struct Fake
{
    std::vector<FakeMember> members;
    int group_state = 5;
    bool group_data_ok = true;
    bool group_stats_ok = true;
    std::int64_t group_sent_unique = 0;
    std::int64_t group_received_unique = 0;
    int group_data_calls = 0;
    /* Reports one more member than fits on every call. */
    bool always_grow = false;
    /* Lists the first member twice. */
    bool duplicate_first = false;
    int bistats_clear = -1;
    int bistats_instantaneous = -1;
    std::map<std::int32_t, int> getsockname_calls;
};

Fake fake;

sockaddr_storage address(const char *ip, const int port)
{
    sockaddr_storage storage{};
    if (std::strchr(ip, ':') != nullptr)
    {
        auto *v6 = reinterpret_cast<sockaddr_in6 *>(&storage);
        v6->sin6_family = AF_INET6;
        v6->sin6_port = htons(static_cast<std::uint16_t>(port));
        std::string text(ip);
        const auto percent = text.find('%');
        if (percent != std::string::npos)
        {
            v6->sin6_scope_id = static_cast<std::uint32_t>(std::stoul(text.substr(percent + 1)));
            text.resize(percent);
        }
        inet_pton(AF_INET6, text.c_str(), &v6->sin6_addr);
    }
    else
    {
        auto *v4 = reinterpret_cast<sockaddr_in *>(&storage);
        v4->sin_family = AF_INET;
        v4->sin_port = htons(static_cast<std::uint16_t>(port));
        inet_pton(AF_INET, ip, &v4->sin_addr);
    }
    return storage;
}

FakeMember &add_member(const std::int32_t socket, const int token, const std::uint16_t weight,
                       const char *peer_ip, const int peer_port, const char *local_ip,
                       const int local_port)
{
    FakeMember member;
    member.data.id = socket;
    member.data.token = token;
    member.data.weight = weight;
    member.data.sockstate = SRTS_CONNECTED;
    member.data.memberstate = SRT_GST_IDLE;
    member.data.result = 0;
    member.data.peeraddr = address(peer_ip, peer_port);
    member.local_ip = local_ip;
    member.local_port = local_port;
    fake.members.push_back(member);
    return fake.members.back();
}

FakeMember *find(const std::int32_t socket)
{
    for (auto &member : fake.members)
    {
        if (member.data.id == socket)
        {
            return &member;
        }
    }
    return nullptr;
}

void remove_member(const std::int32_t socket)
{
    for (auto it = fake.members.begin(); it != fake.members.end(); ++it)
    {
        if (it->data.id == socket)
        {
            fake.members.erase(it);
            return;
        }
    }
}

/* Same contract as srt_group_data: size in *count; error when it does not fit. */
int fake_group_data(const SRTSOCKET group, SRT_SOCKGROUPDATA *output, size_t *count)
{
    ++fake.group_data_calls;
    if (!fake.group_data_ok || group != group_id || count == nullptr)
    {
        return SRT_ERROR;
    }
    const auto capacity = *count;
    std::vector<const FakeMember *> listing;
    for (const auto &member : fake.members)
    {
        listing.push_back(&member);
    }
    if (fake.duplicate_first && !fake.members.empty())
    {
        listing.push_back(&fake.members.front());
    }
    *count = fake.always_grow ? capacity + 1 : listing.size();
    if (output == nullptr)
    {
        return 0;
    }
    if (fake.always_grow || listing.size() > capacity)
    {
        return SRT_ERROR;
    }
    for (std::size_t i = 0; i < listing.size(); ++i)
    {
        const auto previous_peer = output[i].peeraddr;
        output[i] = listing[i]->data;
        if (listing[i]->peer_unknown)
        {
            output[i].peeraddr = previous_peer;
        }
    }
    return static_cast<int>(listing.size());
}

int fake_bistats(const SRTSOCKET socket, SRT_TRACEBSTATS *statistics, const int clear,
                 const int instantaneous)
{
    fake.bistats_clear = clear;
    fake.bistats_instantaneous = instantaneous;
    *statistics = {};
    if (socket == group_id)
    {
        if (!fake.group_stats_ok)
        {
            return SRT_ERROR;
        }
        statistics->pktSentUniqueTotal = fake.group_sent_unique;
        statistics->pktRecvUniqueTotal = fake.group_received_unique;
        statistics->byteSentUniqueTotal = static_cast<std::uint64_t>(fake.group_sent_unique) * 1316U;
        statistics->pktRcvDropTotal = 3;
        statistics->msTimeStamp = 5'000;
        return 0;
    }
    const auto *member = find(socket);
    if (member == nullptr || !member->stats_ok)
    {
        return SRT_ERROR;
    }
    statistics->pktSentTotal = member->sent;
    statistics->pktSentUniqueTotal = member->sent;
    statistics->pktRecvTotal = member->received;
    statistics->pktRecvUniqueTotal = member->received;
    statistics->byteSentTotal = static_cast<std::uint64_t>(member->sent) * 1316U;
    statistics->msRTT = 20.0 + member->data.id % 10;
    return 0;
}

int fake_getsockname(const SRTSOCKET socket, sockaddr *name, int *length)
{
    ++fake.getsockname_calls[socket];
    const auto *member = find(socket);
    if (member == nullptr || !member->name_ok || name == nullptr || length == nullptr)
    {
        return SRT_ERROR;
    }
    const auto storage = address(member->local_ip.c_str(), member->local_port);
    std::memcpy(name, &storage, sizeof(storage));
    *length = static_cast<int>(sizeof(storage));
    return 0;
}

int fake_getsockstate(const std::int32_t socket)
{
    return socket == group_id ? fake.group_state : SRTS_NONEXIST;
}

/* ------------------------------------------------------------- helpers */

robotweax_srt_telemetry_public_srt_group_config_v1 config_for(const std::uint32_t group_type)
{
    robotweax_srt_telemetry_public_srt_group_config_v1 config{};
    config.struct_size = sizeof(config);
    config.abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
    config.group = group_id;
    config.provider = ROBOTWEAX_SRT_TELEMETRY_PROVIDER_ROBOTWEAX_SRT;
    config.capabilities =
        ROBOTWEAX_SRT_TELEMETRY_CAP_COMMON_STATS_V1 | ROBOTWEAX_SRT_TELEMETRY_CAP_GROUP_MEMBERS_V1;
    config.group_qualified_fields = ROBOTWEAX_SRT_TELEMETRY_PUBLIC_SRT_GROUP_CANDIDATE_FIELDS;
    config.group_qualified_ext_fields = ROBOTWEAX_SRT_TELEMETRY_PUBLIC_SRT_GROUP_EXT_CANDIDATE_FIELDS;
    config.member_qualified_fields = ROBOTWEAX_SRT_TELEMETRY_PUBLIC_SRT_V1_5_5_CANDIDATE_FIELDS;
    config.member_qualified_ext_fields = ROBOTWEAX_SRT_TELEMETRY_PUBLIC_SRT_EXT_COMMON_CANDIDATE_FIELDS;
    config.bistats = &fake_bistats;
    config.group_data = &fake_group_data;
    config.getsockname = &fake_getsockname;
    config.getsockstate = &fake_getsockstate;
    config.group_type = group_type;
    return config;
}

struct SessionDeleter
{
    void operator()(robotweax_srt_telemetry_public_srt_group_session *session) const
    {
        robotweax_srt_telemetry_public_srt_group_session_destroy(session);
    }
};
using SessionPtr = std::unique_ptr<robotweax_srt_telemetry_public_srt_group_session, SessionDeleter>;

SessionPtr make_session(const robotweax_srt_telemetry_public_srt_group_config_v1 &config)
{
    robotweax_srt_telemetry_public_srt_group_session *session = nullptr;
    const auto error = robotweax_srt_telemetry_public_srt_group_session_create(&config, &session);
    check(error == ROBOTWEAX_SRT_TELEMETRY_OK && session != nullptr, "group session creation");
    return SessionPtr(session);
}

using Entry = robotweax_srt_telemetry_group_member_v1;

/* A result with a caller-owned member array. */
struct Result : robotweax_srt_telemetry_public_srt_group_result_v1
{
    std::vector<Entry> storage;
};

std::unique_ptr<Result> fresh_result(const std::uint32_t capacity = 16)
{
    auto result = std::make_unique<Result>();
    robotweax_srt_telemetry_public_srt_group_result_v1 &base = *result;
    std::memset(&base, 0, sizeof(base));
    result->struct_size = sizeof(base);
    result->abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
    result->storage.resize(capacity);
    result->members = result->storage.data();
    result->member_capacity = capacity;
    result->member_entry_size = sizeof(Entry);
    return result;
}

struct Ticker
{
    int tick = 0;
    robotweax_srt_telemetry_error sample(robotweax_srt_telemetry_public_srt_group_session *session,
                                         Result &result)
    {
        const auto t = static_cast<std::uint64_t>(tick++);
        return robotweax_srt_telemetry_public_srt_group_session_sample(
            session, UINT64_C(1'700'000'000'000'000'000) + t * tick_ns, base_ns + t * tick_ns,
            &result);
    }
};

const Entry *entry_for(const Result &result, const std::int32_t socket)
{
    for (std::uint32_t i = 0; i < result.member_count; ++i)
    {
        if (result.members[i].socket == socket)
        {
            return &result.members[i];
        }
    }
    return nullptr;
}

bool has(const std::uint32_t flags, const std::uint32_t flag)
{
    return (flags & flag) == flag;
}

void reset_fake()
{
    fake = Fake{};
}

/* --------------------------------------------------------------- tests */

void test_config_validation()
{
    reset_fake();
    robotweax_srt_telemetry_public_srt_group_session *session = nullptr;
    const auto expect = [&](robotweax_srt_telemetry_public_srt_group_config_v1 config,
                            const robotweax_srt_telemetry_error error, const std::string_view what) {
        session = nullptr;
        check(robotweax_srt_telemetry_public_srt_group_session_create(&config, &session) == error &&
                  (session != nullptr) == (error == ROBOTWEAX_SRT_TELEMETRY_OK),
              what);
        robotweax_srt_telemetry_public_srt_group_session_destroy(session);
    };
    const auto good = config_for(ROBOTWEAX_SRT_TELEMETRY_GROUP_TYPE_BACKUP);

    auto config = good;
    config.capabilities = ROBOTWEAX_SRT_TELEMETRY_CAP_COMMON_STATS_V1;
    expect(config, ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT, "rejects a config without CAP_GROUP_MEMBERS_V1");
    config = good;
    config.group = 77;
    expect(config, ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT, "rejects a plain socket as group handle");
    config = good;
    config.group = -1;
    expect(config, ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT, "rejects an invalid handle");
    config = good;
    config.group_qualified_fields |= ROBOTWEAX_SRT_TELEMETRY_FIELD_RTT_SECONDS;
    expect(config, ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT,
           "rejects group fields that srt_bistats on a group does not fill");
    config = good;
    config.group_qualified_ext_fields |= ROBOTWEAX_SRT_TELEMETRY_EXT_FIELD_MSS_BYTES;
    expect(config, ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT, "rejects group ext fields outside the group mask");
    config = good;
    config.member_qualified_ext_fields |=
        ROBOTWEAX_SRT_TELEMETRY_EXT_FIELD_ESTIMATED_LINK_CAPACITY_BITS_PER_SECOND;
    expect(config, ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT,
           "rejects the other provider's mbpsBandwidth interpretation for members");
    config = good;
    config.member_qualified_ext_fields |= ROBOTWEAX_SRT_TELEMETRY_PUBLIC_SRT_EXT_FILTER_CANDIDATE_FIELDS;
    expect(config, ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT, "rejects member FEC fields without CAP_FEC_STATS_V1");
    config = good;
    config.capabilities = ROBOTWEAX_SRT_TELEMETRY_CAP_GROUP_MEMBERS_V1;
    expect(config, ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT, "rejects qualified fields without CAP_COMMON_STATS_V1");
    config = good;
    config.group_data = nullptr;
    expect(config, ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT, "requires srt_group_data");
    config = good;
    config.getsockname = nullptr;
    expect(config, ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT, "requires srt_getsockname");
    config = good;
    config.getsockstate = nullptr;
    expect(config, ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT, "requires srt_getsockstate");
    config = good;
    config.bistats = nullptr;
    expect(config, ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT, "requires srt_bistats");
    config = good;
    config.group_type = 7;
    expect(config, ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT, "rejects an unknown group type");
    config = good;
    config.reserved = 1;
    expect(config, ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT, "rejects a non-zero reserved field");
    config = good;
    config.provider = ROBOTWEAX_SRT_TELEMETRY_PROVIDER_UNKNOWN;
    expect(config, ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT, "rejects an unknown provider");
    config = good;
    {
        const std::underlying_type_t<robotweax_srt_telemetry_provider> invalid = 99;
        std::memcpy(&config.provider, &invalid, sizeof(invalid)); /* as a C host could */
    }
    expect(config, ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT, "rejects an out-of-range provider");
    config = good;
    config.abi_version = 99;
    expect(config, ROBOTWEAX_SRT_TELEMETRY_ABI_MISMATCH, "reports an ABI mismatch");
    config = good;
    config.struct_size = 8;
    expect(config, ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT, "rejects a truncated config");
    check(robotweax_srt_telemetry_public_srt_group_session_create(&good, nullptr) ==
              ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT,
          "requires a session pointer");
    check(robotweax_srt_telemetry_public_srt_group_session_create(nullptr, &session) ==
              ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT,
          "requires a config");

    config = good;
    config.group_qualified_fields = 0;
    config.group_qualified_ext_fields = 0;
    config.member_qualified_fields = 0;
    config.member_qualified_ext_fields = 0;
    config.capabilities = ROBOTWEAX_SRT_TELEMETRY_CAP_GROUP_MEMBERS_V1;
    expect(config, ROBOTWEAX_SRT_TELEMETRY_OK, "accepts a state-only session");
}

void test_result_validation()
{
    reset_fake();
    auto session = make_session(config_for(ROBOTWEAX_SRT_TELEMETRY_GROUP_TYPE_BACKUP));
    auto result = fresh_result();
    result->abi_version = 2;
    check(robotweax_srt_telemetry_public_srt_group_session_sample(session.get(), 1, 1, result.get()) ==
              ROBOTWEAX_SRT_TELEMETRY_ABI_MISMATCH,
          "result ABI mismatch");
    result = fresh_result();
    result->struct_size = 16;
    check(robotweax_srt_telemetry_public_srt_group_session_sample(session.get(), 1, 1, result.get()) ==
              ROBOTWEAX_SRT_TELEMETRY_ABI_MISMATCH,
          "truncated result");
    result = fresh_result();
    result->member_entry_size = 8;
    check(robotweax_srt_telemetry_public_srt_group_session_sample(session.get(), 1, 1, result.get()) ==
              ROBOTWEAX_SRT_TELEMETRY_ABI_MISMATCH,
          "wrong member entry size");
    result = fresh_result();
    result->members = nullptr;
    check(robotweax_srt_telemetry_public_srt_group_session_sample(session.get(), 1, 1, result.get()) ==
              ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT,
          "member capacity without an array");
    result = fresh_result(0);
    result->members = nullptr;
    result->member_entry_size = 0;
    check(robotweax_srt_telemetry_public_srt_group_session_sample(session.get(), 1, 1, result.get()) ==
              ROBOTWEAX_SRT_TELEMETRY_OK &&
              result->member_count == 0 && result->required_member_count == 0,
          "an empty group fits in no array");
    check(robotweax_srt_telemetry_public_srt_group_session_sample(nullptr, 1, 1, result.get()) ==
              ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT,
          "null session");
    check(robotweax_srt_telemetry_public_srt_group_session_sample(session.get(), 1, 1, nullptr) ==
              ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT,
          "null result");
}

/* Main/Backup on the caller side: members, paths, counters, failover,
 * removal, as seen in the live probe of both providers. */
void test_backup_caller_failover()
{
    reset_fake();
    constexpr std::int32_t primary = 1001;
    constexpr std::int32_t backup = 1000;
    add_member(primary, 1, 20, "10.0.1.2", 9000, "10.0.1.1", 41000);
    add_member(backup, 2, 10, "10.0.2.2", 9000, "10.0.2.1", 42000);
    fake.group_sent_unique = 50;
    find(primary)->sent = 50;
    find(primary)->data.memberstate = SRT_GST_RUNNING;

    auto session = make_session(config_for(ROBOTWEAX_SRT_TELEMETRY_GROUP_TYPE_BACKUP));
    auto result = fresh_result();
    Ticker ticker;

    check(ticker.sample(session.get(), *result) == ROBOTWEAX_SRT_TELEMETRY_OK, "first sample");
    check(fake.bistats_clear == 0 && fake.bistats_instantaneous == 1,
          "statistics are read with clear=0, instantaneous=1");
    check(has(result->flags, ROBOTWEAX_SRT_TELEMETRY_GROUP_FIRST_SAMPLE) &&
              has(result->flags, ROBOTWEAX_SRT_TELEMETRY_GROUP_STATS),
          "first sample flags");
    check(!has(result->flags, ROBOTWEAX_SRT_TELEMETRY_GROUP_MEMBERS_CHANGED) &&
              !has(result->flags, ROBOTWEAX_SRT_TELEMETRY_GROUP_ACTIVE_CHANGED) &&
              !has(result->flags, ROBOTWEAX_SRT_TELEMETRY_GROUP_STATE_CHANGED),
          "the first sample reports no group change");
    check(result->group_type == ROBOTWEAX_SRT_TELEMETRY_GROUP_TYPE_BACKUP && result->group_state == 5,
          "group type and state");
    check(result->listed_member_count == 2 && result->member_count == 2 &&
              result->usable_member_count == 2,
          "member counts");
    check(result->active_member == -1 && result->previous_active_member == -1,
          "no active member before data has been seen moving");
    check(result->raw.unique_sent_packet_total == 50 &&
              (result->raw.available_fields & ROBOTWEAX_SRT_TELEMETRY_FIELD_UNIQUE_SENT_PACKET_TOTAL) != 0,
          "group unique sent counter");
    check((result->raw.available_fields & ROBOTWEAX_SRT_TELEMETRY_FIELD_SENT_PACKET_TOTAL) == 0,
          "group fields stay within the group mask");
    check((result->raw.capabilities & ROBOTWEAX_SRT_TELEMETRY_CAP_GROUP_MEMBERS_V1) != 0,
          "group samples carry CAP_GROUP_MEMBERS_V1");
    check(result->ext.connection_age_seconds == 5.0, "group connection age");

    const auto *p = entry_for(*result, primary);
    const auto *b = entry_for(*result, backup);
    check(p != nullptr && b != nullptr, "both members reported");
    if (p == nullptr || b == nullptr)
    {
        return;
    }
    check(p->struct_size == sizeof(*p) && p->raw.struct_size == sizeof(p->raw), "member struct sizes");
    check(has(p->flags, ROBOTWEAX_SRT_TELEMETRY_MEMBER_NEW) &&
              has(p->flags, ROBOTWEAX_SRT_TELEMETRY_MEMBER_PATH_ASSIGNED) &&
              has(p->flags, ROBOTWEAX_SRT_TELEMETRY_MEMBER_USABLE),
          "new member flags");
    check(!has(p->flags, ROBOTWEAX_SRT_TELEMETRY_MEMBER_CARRYING),
          "carrying needs two samples");
    check(std::string(p->path) == "token-1" &&
              p->path_source == ROBOTWEAX_SRT_TELEMETRY_PATH_SOURCE_TOKEN,
          "caller path from token");
    check(std::string(b->path) == "token-2", "second caller path");
    check(std::string(p->local_address) == "10.0.1.1:41000" &&
              std::string(p->peer_address) == "10.0.1.2:9000",
          "member addresses");
    check(p->token == 1 && p->weight == 20 && p->member_state == SRT_GST_RUNNING &&
              p->socket_state == SRTS_CONNECTED,
          "member identity fields");
    check(b->member_state == SRT_GST_IDLE && b->weight == 10, "standby member");
    check(p->raw.sent_packet_total == 50 && p->raw.rtt_seconds > 0.0, "member counters");
    check((p->raw.capabilities & ROBOTWEAX_SRT_TELEMETRY_CAP_GROUP_MEMBERS_V1) != 0,
          "member samples carry CAP_GROUP_MEMBERS_V1");
    const auto name_calls = fake.getsockname_calls[primary];

    /* Tick 2: primary carries. */
    fake.group_sent_unique = 100;
    find(primary)->sent = 100;
    ticker.sample(session.get(), *result);
    p = entry_for(*result, primary);
    b = entry_for(*result, backup);
    check(p != nullptr && has(p->flags, ROBOTWEAX_SRT_TELEMETRY_MEMBER_CARRYING) &&
              b != nullptr && !has(b->flags, ROBOTWEAX_SRT_TELEMETRY_MEMBER_CARRYING),
          "primary carries");
    check(p != nullptr && !has(p->flags, ROBOTWEAX_SRT_TELEMETRY_MEMBER_NEW) &&
              !has(p->flags, ROBOTWEAX_SRT_TELEMETRY_MEMBER_PATH_ASSIGNED),
          "flags are per call");
    check(p != nullptr && p->delta.sent_packet_delta == 50, "member delta");
    check(result->delta.unique_sent_packet_delta == 50, "group delta");
    check(result->active_member == primary && result->previous_active_member == -1 &&
              has(result->flags, ROBOTWEAX_SRT_TELEMETRY_GROUP_ACTIVE_CHANGED),
          "active member established");
    check(fake.getsockname_calls[primary] == name_calls,
          "the local address is read once");

    /* Tick 3: idle stream: nothing moves, nothing changes. */
    ticker.sample(session.get(), *result);
    check(result->active_member == primary &&
              !has(result->flags, ROBOTWEAX_SRT_TELEMETRY_GROUP_ACTIVE_CHANGED),
          "an idle tick keeps the active member");

    /* Tick 4: primary path cut. Haivision keeps it listed as BROKEN and stops
     * answering srt_bistats; the backup takes over. */
    auto *pm = find(primary);
    pm->data.memberstate = SRT_GST_BROKEN;
    pm->data.result = -1;
    pm->stats_ok = false;
    auto *bm = find(backup);
    bm->data.memberstate = SRT_GST_RUNNING;
    bm->sent = 30;
    fake.group_sent_unique = 130;
    ticker.sample(session.get(), *result);
    p = entry_for(*result, primary);
    b = entry_for(*result, backup);
    check(p != nullptr && has(p->flags, ROBOTWEAX_SRT_TELEMETRY_MEMBER_STATE_CHANGED) &&
              has(p->flags, ROBOTWEAX_SRT_TELEMETRY_MEMBER_STATS_STALE) &&
              !has(p->flags, ROBOTWEAX_SRT_TELEMETRY_MEMBER_USABLE),
          "broken primary: state change, stale stats, not usable");
    check(p != nullptr && p->raw.sent_packet_total == 100 && p->delta.available_delta_fields == 0 &&
              p->native_result == SRT_ERROR && p->member_result == -1,
          "stale stats keep the last good counters and have no delta");
    check(b != nullptr && has(b->flags, ROBOTWEAX_SRT_TELEMETRY_MEMBER_CARRYING) &&
              has(b->flags, ROBOTWEAX_SRT_TELEMETRY_MEMBER_STATE_CHANGED),
          "backup carries");
    check(result->active_member == backup && result->previous_active_member == primary &&
              has(result->flags, ROBOTWEAX_SRT_TELEMETRY_GROUP_ACTIVE_CHANGED) &&
              has(result->flags, ROBOTWEAX_SRT_TELEMETRY_GROUP_MEMBER_STATE_CHANGED),
          "active member switch");
    check(result->usable_member_count == 1, "one usable member");

    /* Tick 5: the provider drops the broken member from the list. */
    remove_member(primary);
    bm->sent = 60;
    ticker.sample(session.get(), *result);
    check(result->listed_member_count == 1 && result->member_count == 2 &&
              result->required_member_count == 2,
          "removed member reported");
    p = entry_for(*result, primary);
    check(p != nullptr && has(p->flags, ROBOTWEAX_SRT_TELEMETRY_MEMBER_REMOVED) &&
              has(p->flags, ROBOTWEAX_SRT_TELEMETRY_MEMBER_STATS_STALE) &&
              p->raw.sent_packet_total == 100 && std::string(p->path) == "token-1" &&
              p->member_state == SRT_GST_BROKEN,
          "removed member carries its last values");
    check(result->members[0].socket == primary && result->members[1].socket == backup,
          "removed members come first");
    check(has(result->flags, ROBOTWEAX_SRT_TELEMETRY_GROUP_MEMBERS_CHANGED) &&
              !has(result->flags, ROBOTWEAX_SRT_TELEMETRY_GROUP_ACTIVE_CHANGED),
          "removal is a membership change, not an active change");

    /* Tick 6: reported once. */
    bm->sent = 90;
    ticker.sample(session.get(), *result);
    check(result->member_count == 1 && entry_for(*result, primary) == nullptr &&
              !has(result->flags, ROBOTWEAX_SRT_TELEMETRY_GROUP_MEMBERS_CHANGED),
          "a removed member is reported once");

    /* Tick 7: replacement member for the cut path, same token. */
    auto &replacement = add_member(1002, 1, 20, "10.0.1.2", 9000, "10.0.1.1", 41100);
    replacement.data.sockstate = SRTS_CONNECTING;
    replacement.data.memberstate = SRT_GST_PENDING;
    replacement.stats_ok = false;
    bm->sent = 120;
    ticker.sample(session.get(), *result);
    const auto *r = entry_for(*result, 1002);
    check(r != nullptr && has(r->flags, ROBOTWEAX_SRT_TELEMETRY_MEMBER_NEW) &&
              has(r->flags, ROBOTWEAX_SRT_TELEMETRY_MEMBER_STATS_NONE) && r->path[0] == '\0' &&
              r->path_source == ROBOTWEAX_SRT_TELEMETRY_PATH_SOURCE_NONE,
          "a connecting member has no path yet");
    check(has(result->flags, ROBOTWEAX_SRT_TELEMETRY_GROUP_MEMBERS_CHANGED), "member added");

    /* Tick 8: it connects and is named like the path it replaces. */
    auto *rm = find(1002);
    rm->data.sockstate = SRTS_CONNECTED;
    rm->data.memberstate = SRT_GST_IDLE;
    rm->stats_ok = true;
    bm->sent = 150;
    ticker.sample(session.get(), *result);
    r = entry_for(*result, 1002);
    check(r != nullptr && std::string(r->path) == "token-1" &&
              has(r->flags, ROBOTWEAX_SRT_TELEMETRY_MEMBER_PATH_ASSIGNED) &&
              has(r->flags, ROBOTWEAX_SRT_TELEMETRY_MEMBER_STATE_CHANGED),
          "reused token keeps the path name");
}

/* The accepting side: token -1, so paths are named by host or addresses. */
int path_name_calls = 0;
int host_path_name(void *user_data, const robotweax_srt_telemetry_group_member_identity_v1 *member,
                   char *name, const size_t capacity)
{
    ++path_name_calls;
    const auto *names = static_cast<const std::map<std::string, std::string> *>(user_data);
    const auto it = names->find(member->peer_address);
    if (it == names->end() || member->struct_size != sizeof(*member) || member->group != group_id)
    {
        return 0;
    }
    std::snprintf(name, capacity, "%s", it->second.c_str());
    return 1;
}

void test_listener_path_names()
{
    reset_fake();
    add_member(2001, -1, 20, "10.0.1.1", 41000, "10.0.0.9", 9000);
    add_member(2002, -1, 10, "10.0.2.1", 42000, "10.0.0.9", 9000);
    add_member(2003, -1, 5, "2001:db8::7", 43000, "2001:db8::9", 9000);
    add_member(2004, -1, 5, "10.0.4.1", 44000, "10.0.0.9", 9000);
    find(2004)->name_ok = false;

    std::map<std::string, std::string> names{{"10.0.1.1:41000", "lte-1"},
                                             {"10.0.2.1:42000", "bad name"}};
    auto config = config_for(ROBOTWEAX_SRT_TELEMETRY_GROUP_TYPE_BROADCAST);
    config.path_name = &host_path_name;
    config.path_name_user_data = &names;
    auto session = make_session(config);
    auto result = fresh_result();
    Ticker ticker;
    path_name_calls = 0;
    ticker.sample(session.get(), *result);

    const auto *a = entry_for(*result, 2001);
    const auto *b = entry_for(*result, 2002);
    const auto *c = entry_for(*result, 2003);
    const auto *d = entry_for(*result, 2004);
    check(a != nullptr && std::string(a->path) == "lte-1" &&
              a->path_source == ROBOTWEAX_SRT_TELEMETRY_PATH_SOURCE_HOST,
          "host path name");
    check(b != nullptr && std::string(b->path) == "10.0.0.9:9000>10.0.2.1:42000" &&
              b->path_source == ROBOTWEAX_SRT_TELEMETRY_PATH_SOURCE_ADDRESSES,
          "an invalid host name falls back to addresses (no token on the accepting side)");
    check(c != nullptr && std::string(c->path) == "[2001:db8::9]:9000>[2001:db8::7]:43000" &&
              std::string(c->peer_address) == "[2001:db8::7]:43000",
          "IPv6 addresses");
    check(d != nullptr && std::string(d->path) == "?>10.0.4.1:44000" && d->local_address[0] == '\0',
          "unknown local address");
    check(path_name_calls == 4, "the host is asked once per member");
    ticker.sample(session.get(), *result);
    check(path_name_calls == 4, "and not again");
    d = entry_for(*result, 2004);
    check(d != nullptr && std::string(d->path) == "?>10.0.4.1:44000",
          "a path name never changes");
    check(fake.getsockname_calls[2004] == 2, "an unknown local address is retried");
}

/* Broadcast: the carrying set, not a single active member. */
void test_broadcast_carrying_set()
{
    reset_fake();
    add_member(3001, 1, 1, "10.0.1.2", 9000, "10.0.1.1", 41000);
    add_member(3002, 2, 1, "10.0.2.2", 9000, "10.0.2.1", 42000);
    auto session = make_session(config_for(ROBOTWEAX_SRT_TELEMETRY_GROUP_TYPE_BROADCAST));
    auto result = fresh_result();
    Ticker ticker;
    ticker.sample(session.get(), *result);

    find(3001)->sent = 10;
    find(3002)->sent = 10;
    ticker.sample(session.get(), *result);
    check(has(result->flags, ROBOTWEAX_SRT_TELEMETRY_GROUP_ACTIVE_CHANGED) &&
              result->active_member == -1,
          "both members start carrying");
    find(3001)->sent = 20;
    find(3002)->sent = 20;
    ticker.sample(session.get(), *result);
    check(!has(result->flags, ROBOTWEAX_SRT_TELEMETRY_GROUP_ACTIVE_CHANGED), "same carrying set");
    find(3002)->sent = 30;
    ticker.sample(session.get(), *result);
    check(has(result->flags, ROBOTWEAX_SRT_TELEMETRY_GROUP_ACTIVE_CHANGED), "one member stops");
    ticker.sample(session.get(), *result);
    check(!has(result->flags, ROBOTWEAX_SRT_TELEMETRY_GROUP_ACTIVE_CHANGED), "idle tick");
    find(3001)->sent = 30;
    find(3002)->sent = 40;
    ticker.sample(session.get(), *result);
    check(has(result->flags, ROBOTWEAX_SRT_TELEMETRY_GROUP_ACTIVE_CHANGED), "both carry again");
}

/* Main/Backup while both members carry (a better-weight candidate probed in
 * parallel): the member with more packets is active; ties keep the current. */
void test_backup_parallel_probe()
{
    reset_fake();
    add_member(4001, 1, 20, "10.0.1.2", 9000, "10.0.1.1", 41000);
    add_member(4002, 2, 10, "10.0.2.2", 9000, "10.0.2.1", 42000);
    auto session = make_session(config_for(ROBOTWEAX_SRT_TELEMETRY_GROUP_TYPE_BACKUP));
    auto result = fresh_result();
    Ticker ticker;
    ticker.sample(session.get(), *result);
    find(4002)->sent = 50;
    ticker.sample(session.get(), *result);
    check(result->active_member == 4002, "backup active");
    find(4001)->sent = 5;
    find(4002)->sent = 100;
    ticker.sample(session.get(), *result);
    check(result->active_member == 4002 &&
              !has(result->flags, ROBOTWEAX_SRT_TELEMETRY_GROUP_ACTIVE_CHANGED),
          "a probe with fewer packets does not switch");
    find(4001)->sent = 55;
    find(4002)->sent = 150;
    ticker.sample(session.get(), *result);
    check(result->active_member == 4002, "a tie keeps the current active member");
    find(4001)->sent = 105;
    ticker.sample(session.get(), *result);
    check(result->active_member == 4001 &&
              has(result->flags, ROBOTWEAX_SRT_TELEMETRY_GROUP_ACTIVE_CHANGED),
          "failback");
}

void test_group_states_and_failures()
{
    reset_fake();
    add_member(5001, 1, 1, "10.0.1.2", 9000, "10.0.1.1", 41000);
    fake.group_stats_ok = false; /* not connected yet */
    fake.group_state = SRTS_CONNECTING;
    auto session = make_session(config_for(ROBOTWEAX_SRT_TELEMETRY_GROUP_TYPE_BACKUP));
    auto result = fresh_result();
    Ticker ticker;
    check(ticker.sample(session.get(), *result) == ROBOTWEAX_SRT_TELEMETRY_OK,
          "a group without statistics still samples its members");
    check(!has(result->flags, ROBOTWEAX_SRT_TELEMETRY_GROUP_STATS) &&
              result->group_native_result == SRT_ERROR && result->raw.available_fields == 0,
          "group statistics unavailable");
    fake.group_stats_ok = true;
    fake.group_state = SRTS_CONNECTED;
    ticker.sample(session.get(), *result);
    check(has(result->flags, ROBOTWEAX_SRT_TELEMETRY_GROUP_STATE_CHANGED) &&
              result->group_state == SRTS_CONNECTED,
          "group connected");
    ticker.sample(session.get(), *result);
    check(!has(result->flags, ROBOTWEAX_SRT_TELEMETRY_GROUP_STATE_CHANGED), "no change");
    fake.group_state = SRTS_BROKEN;
    fake.group_data_ok = false;
    check(ticker.sample(session.get(), *result) == ROBOTWEAX_SRT_TELEMETRY_PROVIDER_SAMPLE_FAILURE,
          "a failing srt_group_data fails the call");
    check(has(result->flags, ROBOTWEAX_SRT_TELEMETRY_GROUP_STATE_CHANGED) &&
              result->group_state == SRTS_BROKEN && result->group_data_native_result == SRT_ERROR &&
              result->member_count == 0,
          "the failure still reports the group state");
}

int count_flag(const Result &result, const std::uint32_t flag)
{
    int count = 0;
    for (std::uint32_t i = 0; i < result.member_count; ++i)
    {
        count += has(result.members[i].flags, flag) ? 1 : 0;
    }
    return count;
}

void test_growth_and_truncation()
{
    reset_fake();
    for (int i = 0; i < 20; ++i)
    {
        add_member(6000 + i, i, 1, "10.0.1.2", 9000, "10.0.1.1", 41000 + i);
    }
    auto session = make_session(config_for(ROBOTWEAX_SRT_TELEMETRY_GROUP_TYPE_BROADCAST));
    auto small = fresh_result(4);
    Ticker ticker;
    fake.group_data_calls = 0;
    check(ticker.sample(session.get(), *small) == ROBOTWEAX_SRT_TELEMETRY_BUFFER_TOO_SMALL,
          "a group larger than the member array");
    check(fake.group_data_calls == 2, "the member buffer grows after one too-small read");
    check(small->listed_member_count == 20 && small->member_count == 4 &&
              small->required_member_count == 20 && small->usable_member_count == 20 &&
              has(small->flags, ROBOTWEAX_SRT_TELEMETRY_GROUP_TRUNCATED) &&
              has(small->flags, ROBOTWEAX_SRT_TELEMETRY_GROUP_STATS),
          "the result is valid apart from the members that did not fit");
    check(count_flag(*small, ROBOTWEAX_SRT_TELEMETRY_MEMBER_NEW) == 4, "four members reported");

    /* The four reported members go away; sixteen stay listed. */
    fake.members.erase(fake.members.begin(), fake.members.begin() + 4);
    fake.group_data_calls = 0;
    check(ticker.sample(session.get(), *small) == ROBOTWEAX_SRT_TELEMETRY_BUFFER_TOO_SMALL &&
              fake.group_data_calls == 1,
          "one read once the buffer fits");
    check(small->member_count == 4 && small->required_member_count == 20 &&
              count_flag(*small, ROBOTWEAX_SRT_TELEMETRY_MEMBER_REMOVED) == 4,
          "removals are reported before listed members");
    check(has(small->flags, ROBOTWEAX_SRT_TELEMETRY_GROUP_MEMBERS_CHANGED), "members changed");

    ticker.sample(session.get(), *small);
    check(small->member_count == 4 && count_flag(*small, ROBOTWEAX_SRT_TELEMETRY_MEMBER_NEW) == 4 &&
              count_flag(*small, ROBOTWEAX_SRT_TELEMETRY_MEMBER_REMOVED) == 0 &&
              has(small->flags, ROBOTWEAX_SRT_TELEMETRY_GROUP_MEMBERS_CHANGED),
          "members that never fitted are reported NEW once they fit, with the membership flag");

    auto large = fresh_result(32);
    check(ticker.sample(session.get(), *large) == ROBOTWEAX_SRT_TELEMETRY_OK &&
              large->member_count == 16 && count_flag(*large, ROBOTWEAX_SRT_TELEMETRY_MEMBER_NEW) == 12,
          "a larger array reports the rest as NEW, nothing lost");

    /* A member that was never reported is not reported as removed. */
    add_member(6100, 50, 1, "10.0.1.2", 9000, "10.0.1.1", 43000);
    auto none = fresh_result(16);
    ticker.sample(session.get(), *none); /* 17 listed: 6100 does not fit */
    remove_member(6100);
    ticker.sample(session.get(), *large);
    check(entry_for(*large, 6100) == nullptr && large->required_member_count == 16,
          "no removal for a member the host never saw");
}

void test_capacity_zero()
{
    reset_fake();
    add_member(6200, 1, 1, "10.0.1.2", 9000, "10.0.1.1", 41000);
    add_member(6201, 2, 1, "10.0.2.2", 9000, "10.0.2.1", 42000);
    auto session = make_session(config_for(ROBOTWEAX_SRT_TELEMETRY_GROUP_TYPE_BACKUP));
    auto full = fresh_result(4);
    auto group_only = fresh_result(0);
    group_only->members = nullptr;
    Ticker ticker;
    ticker.sample(session.get(), *full);
    remove_member(6200);
    check(ticker.sample(session.get(), *group_only) == ROBOTWEAX_SRT_TELEMETRY_BUFFER_TOO_SMALL &&
              group_only->member_count == 0 && group_only->listed_member_count == 1 &&
              !has(group_only->flags, ROBOTWEAX_SRT_TELEMETRY_GROUP_MEMBERS_CHANGED),
          "capacity 0: the group only, no member flags");
    ticker.sample(session.get(), *group_only);
    check(!has(group_only->flags, ROBOTWEAX_SRT_TELEMETRY_GROUP_MEMBERS_CHANGED) &&
              group_only->required_member_count == 1,
          "the removal is not kept");
    ticker.sample(session.get(), *full);
    check(full->member_count == 1 && entry_for(*full, 6200) == nullptr,
          "no backlog appears when an array is supplied again");
}

void test_relisted_pending_removal()
{
    reset_fake();
    add_member(6300, 1, 1, "10.0.1.2", 9000, "10.0.1.1", 41000);
    add_member(6301, 2, 1, "10.0.2.2", 9000, "10.0.2.1", 42000);
    auto session = make_session(config_for(ROBOTWEAX_SRT_TELEMETRY_GROUP_TYPE_BACKUP));
    auto two = fresh_result(2);
    auto one = fresh_result(1);
    Ticker ticker;
    ticker.sample(session.get(), *two);
    const auto saved = fake.members;
    fake.members.clear();
    ticker.sample(session.get(), *one); /* reports one removal; the other stays pending */
    check(one->member_count == 1 && count_flag(*one, ROBOTWEAX_SRT_TELEMETRY_MEMBER_REMOVED) == 1,
          "one removal reported");
    const auto reported = one->members[0].socket;
    const auto pending = reported == 6300 ? 6301 : 6300;
    for (const auto &member : saved)
    {
        if (member.data.id == pending)
        {
            fake.members.push_back(member);
        }
    }
    ticker.sample(session.get(), *two);
    const auto *m = entry_for(*two, pending);
    check(two->member_count == 1 && m != nullptr &&
              !has(m->flags, ROBOTWEAX_SRT_TELEMETRY_MEMBER_REMOVED) &&
              !has(m->flags, ROBOTWEAX_SRT_TELEMETRY_MEMBER_NEW),
          "a pending removal listed again continues: one entry, neither REMOVED nor NEW");
    ticker.sample(session.get(), *two);
    check(two->member_count == 1 && count_flag(*two, ROBOTWEAX_SRT_TELEMETRY_MEMBER_REMOVED) == 0,
          "and its removal is never reported");
}

/* For every allocation inside one call, fail it and check that the next call
 * still reports the removal and the new member exactly once. */
void test_allocation_failure_sweep()
{
    int failures_seen = 0;
    for (long k = 1; k < 1000; ++k)
    {
        reset_fake();
        add_member(8000, 1, 1, "10.0.1.2", 9000, "10.0.1.1", 41000);
        add_member(8001, 2, 1, "10.0.2.2", 9000, "10.0.2.1", 42000);
        auto session = make_session(config_for(ROBOTWEAX_SRT_TELEMETRY_GROUP_TYPE_BACKUP));
        auto result = fresh_result(4);
        Ticker ticker;
        ticker.sample(session.get(), *result);
        find(8001)->sent = 10;
        ticker.sample(session.get(), *result);

        remove_member(8000);
        add_member(8002, 3, 1, "10.0.3.2", 9000, "10.0.3.1", 43000);
        find(8001)->sent = 20;
        find(8001)->data.memberstate = SRT_GST_RUNNING; /* a state change */
        fake.group_state = SRTS_BROKEN;                 /* a group state change */
        int removed = 0;
        int added = 0;
        int old_new = 0;
        int state_changed = 0;
        int path_assigned = 0;
        int group_state_changed = 0;
        int members_changed = 0;
        int member_state_changed = 0;
        const auto tally = [&](const Result &r) {
            for (std::uint32_t i = 0; i < r.member_count; ++i)
            {
                const auto &e = r.members[i];
                removed += e.socket == 8000 && has(e.flags, ROBOTWEAX_SRT_TELEMETRY_MEMBER_REMOVED);
                added += e.socket == 8002 && has(e.flags, ROBOTWEAX_SRT_TELEMETRY_MEMBER_NEW);
                old_new += e.socket == 8001 && has(e.flags, ROBOTWEAX_SRT_TELEMETRY_MEMBER_NEW);
                state_changed +=
                    e.socket == 8001 && has(e.flags, ROBOTWEAX_SRT_TELEMETRY_MEMBER_STATE_CHANGED);
                path_assigned +=
                    e.socket == 8002 && has(e.flags, ROBOTWEAX_SRT_TELEMETRY_MEMBER_PATH_ASSIGNED);
            }
            group_state_changed += has(r.flags, ROBOTWEAX_SRT_TELEMETRY_GROUP_STATE_CHANGED);
            members_changed += has(r.flags, ROBOTWEAX_SRT_TELEMETRY_GROUP_MEMBERS_CHANGED);
            member_state_changed += has(r.flags, ROBOTWEAX_SRT_TELEMETRY_GROUP_MEMBER_STATE_CHANGED);
        };
        const auto all_once = [&] {
            return removed == 1 && added == 1 && old_new == 0 && state_changed == 1 &&
                   path_assigned == 1 && group_state_changed == 1 && members_changed == 1 &&
                   member_state_changed == 1;
        };
        allocations_until_failure = k;
        const auto error = ticker.sample(session.get(), *result);
        const bool fired = allocations_until_failure.load() == 0;
        allocations_until_failure = 0;
        if (!fired)
        {
            check(error == ROBOTWEAX_SRT_TELEMETRY_OK, "the sweep ends with a clean call");
            tally(*result);
            check(all_once(), "clean call reports the changes once");
            break;
        }
        ++failures_seen;
        check(error == ROBOTWEAX_SRT_TELEMETRY_ALLOCATION_FAILURE && result->member_count == 0,
              "a failed allocation is reported and leaves no entries");
        check(ticker.sample(session.get(), *result) == ROBOTWEAX_SRT_TELEMETRY_OK,
              "the call after a failed allocation succeeds");
        tally(*result);
        check(all_once(), "after allocation failure " + std::to_string(k) +
                              ": every change reported once, nothing lost or duplicated");
        check(result->member_count == 3, "entries after recovery: " + std::to_string(result->member_count) + " at k=" + std::to_string(k));
    }
    check(failures_seen > 5, "the sweep reached allocations inside the call");
}

void test_group_data_attempts_exhausted()
{
    reset_fake();
    add_member(6500, 1, 1, "10.0.1.2", 9000, "10.0.1.1", 41000);
    fake.always_grow = true;
    auto session = make_session(config_for(ROBOTWEAX_SRT_TELEMETRY_GROUP_TYPE_BACKUP));
    auto result = fresh_result();
    Ticker ticker;
    fake.group_data_calls = 0;
    check(ticker.sample(session.get(), *result) == ROBOTWEAX_SRT_TELEMETRY_PROVIDER_SAMPLE_FAILURE &&
              fake.group_data_calls == 3,
          "a group that keeps growing fails after three reads");
}

void test_duplicate_listing()
{
    reset_fake();
    add_member(6600, 1, 1, "10.0.1.2", 9000, "10.0.1.1", 41000);
    add_member(6601, 2, 1, "10.0.2.2", 9000, "10.0.2.1", 42000);
    fake.duplicate_first = true;
    auto session = make_session(config_for(ROBOTWEAX_SRT_TELEMETRY_GROUP_TYPE_BACKUP));
    auto result = fresh_result();
    Ticker ticker;
    ticker.sample(session.get(), *result);
    check(result->listed_member_count == 2 && result->member_count == 2 &&
              result->usable_member_count == 2,
          "a socket listed twice counts once");
}

/* Haivision SRT lists a connecting member before it knows the peer and does
 * not write peeraddr for it: no stale address may leak from another slot. */
void test_peer_address_not_reused()
{
    reset_fake();
    add_member(6700, 1, 1, "10.0.1.2", 9000, "10.0.1.1", 41000);
    auto session = make_session(config_for(ROBOTWEAX_SRT_TELEMETRY_GROUP_TYPE_BACKUP));
    auto result = fresh_result();
    Ticker ticker;
    ticker.sample(session.get(), *result);
    remove_member(6700);
    auto &connecting = add_member(6701, 2, 1, "10.0.9.9", 9000, "0.0.0.0", 42000);
    connecting.data.sockstate = SRTS_CONNECTING;
    connecting.peer_unknown = true;
    ticker.sample(session.get(), *result);
    const auto *m = entry_for(*result, 6701);
    check(m != nullptr && m->peer_address[0] == '\0',
          "a member without a peer address reports none, not the previous slot's");
}

/* A connecting caller member is bound to the wildcard; the address is kept
 * only once it is CONNECTED. */
void test_local_address_after_connect()
{
    reset_fake();
    auto &member = add_member(6800, 1, 1, "10.0.1.2", 9000, "0.0.0.0", 41000);
    member.data.sockstate = SRTS_CONNECTING;
    auto session = make_session(config_for(ROBOTWEAX_SRT_TELEMETRY_GROUP_TYPE_BACKUP));
    auto result = fresh_result();
    Ticker ticker;
    ticker.sample(session.get(), *result);
    const auto *m = entry_for(*result, 6800);
    check(m != nullptr && std::string(m->local_address) == "0.0.0.0:41000",
          "a connecting member shows its bound address");
    auto *fm = find(6800);
    fm->data.sockstate = SRTS_CONNECTED;
    fm->local_ip = "10.0.1.1";
    ticker.sample(session.get(), *result);
    m = entry_for(*result, 6800);
    check(m != nullptr && std::string(m->local_address) == "10.0.1.1:41000",
          "the connected address replaces it");
    check(m != nullptr && std::string(m->path) == "token-1", "path named on connect");
    fm->local_ip = "10.0.7.7";
    const auto calls = fake.getsockname_calls[6800];
    ticker.sample(session.get(), *result);
    m = entry_for(*result, 6800);
    check(m != nullptr && std::string(m->local_address) == "10.0.1.1:41000" &&
              fake.getsockname_calls[6800] == calls,
          "and is kept once connected");
}

void test_first_listing_failure()
{
    reset_fake();
    add_member(6900, 1, 1, "10.0.1.2", 9000, "10.0.1.1", 41000);
    fake.group_data_ok = false;
    auto session = make_session(config_for(ROBOTWEAX_SRT_TELEMETRY_GROUP_TYPE_BACKUP));
    auto result = fresh_result();
    Ticker ticker;
    check(ticker.sample(session.get(), *result) == ROBOTWEAX_SRT_TELEMETRY_PROVIDER_SAMPLE_FAILURE,
          "first listing fails");
    check(!has(result->flags, ROBOTWEAX_SRT_TELEMETRY_GROUP_FIRST_SAMPLE), "no first sample yet");
    fake.group_data_ok = true;
    ticker.sample(session.get(), *result);
    check(has(result->flags, ROBOTWEAX_SRT_TELEMETRY_GROUP_FIRST_SAMPLE) &&
              !has(result->flags, ROBOTWEAX_SRT_TELEMETRY_GROUP_MEMBERS_CHANGED),
          "the first successful listing is the first sample");
}

int filling_path_name(void *user_data, const robotweax_srt_telemetry_group_member_identity_v1 *,
                      char *name, const size_t capacity)
{
    const auto mode = *static_cast<const int *>(user_data);
    if (mode == 0)
    {
        std::memset(name, 'x', capacity); /* no NUL */
    }
    else if (mode == 1)
    {
        name[0] = '\0'; /* empty */
    }
    else
    {
        std::memset(name, 'p', capacity - 1); /* longest valid name */
        name[capacity - 1] = '\0';
    }
    return 1;
}

void test_path_name_bounds()
{
    for (int mode = 0; mode < 3; ++mode)
    {
        reset_fake();
        add_member(7100, 4, 1, "10.0.1.2", 9000, "10.0.1.1", 41000);
        auto config = config_for(ROBOTWEAX_SRT_TELEMETRY_GROUP_TYPE_BACKUP);
        config.path_name = &filling_path_name;
        config.path_name_user_data = &mode;
        auto session = make_session(config);
        auto result = fresh_result();
        Ticker ticker;
        ticker.sample(session.get(), *result);
        const auto *m = entry_for(*result, 7100);
        if (mode < 2)
        {
            check(m != nullptr && std::string(m->path) == "token-4",
                  "an unterminated or empty host name falls back");
        }
        else
        {
            check(m != nullptr && std::strlen(m->path) == ROBOTWEAX_SRT_TELEMETRY_GROUP_PATH_CAPACITY - 1 &&
                      m->path_source == ROBOTWEAX_SRT_TELEMETRY_PATH_SOURCE_HOST,
                  "the longest host name is kept whole");
        }
    }
}

/* The longest automatic name: two scoped IPv6 addresses, never truncated. */
void test_long_ipv6_path()
{
    reset_fake();
    add_member(7200, -1, 1, "ffff:ffff:ffff:ffff:ffff:ffff:ffff:fffe%4294967295", 65535,
               "ffff:ffff:ffff:ffff:ffff:ffff:ffff:fffd%4294967294", 65534);
    auto session = make_session(config_for(ROBOTWEAX_SRT_TELEMETRY_GROUP_TYPE_BROADCAST));
    auto result = fresh_result();
    Ticker ticker;
    ticker.sample(session.get(), *result);
    const auto *m = entry_for(*result, 7200);
    const std::string expected = "[ffff:ffff:ffff:ffff:ffff:ffff:ffff:fffd%4294967294]:65534>"
                                 "[ffff:ffff:ffff:ffff:ffff:ffff:ffff:fffe%4294967295]:65535";
    check(m != nullptr && std::string(m->path) == expected, "full scoped IPv6 path name");
}

void test_unknown_type_and_removed_active()
{
    reset_fake();
    add_member(7300, 1, 1, "10.0.1.2", 9000, "10.0.1.1", 41000);
    add_member(7301, 2, 1, "10.0.2.2", 9000, "10.0.2.1", 42000);
    auto session = make_session(config_for(ROBOTWEAX_SRT_TELEMETRY_GROUP_TYPE_UNKNOWN));
    auto result = fresh_result();
    Ticker ticker;
    ticker.sample(session.get(), *result);
    find(7300)->sent = 10;
    ticker.sample(session.get(), *result);
    check(!has(result->flags, ROBOTWEAX_SRT_TELEMETRY_GROUP_ACTIVE_CHANGED) &&
              result->active_member == -1,
          "an unknown group type reports no active changes");
    const auto *m = entry_for(*result, 7300);
    check(m != nullptr && has(m->flags, ROBOTWEAX_SRT_TELEMETRY_MEMBER_CARRYING),
          "but still marks carrying members");

    reset_fake();
    add_member(7400, 1, 1, "10.0.1.2", 9000, "10.0.1.1", 41000);
    auto &never = add_member(7401, 2, 1, "10.0.2.2", 9000, "10.0.2.1", 42000);
    never.stats_ok = false;
    auto backup = make_session(config_for(ROBOTWEAX_SRT_TELEMETRY_GROUP_TYPE_BACKUP));
    Ticker other;
    other.sample(backup.get(), *result);
    find(7400)->sent = 10;
    other.sample(backup.get(), *result);
    check(result->active_member == 7400, "active member");
    remove_member(7400);
    remove_member(7401);
    other.sample(backup.get(), *result);
    check(result->active_member == 7400 &&
              !has(result->flags, ROBOTWEAX_SRT_TELEMETRY_GROUP_ACTIVE_CHANGED),
          "a removed active member stays active until another member carries");
    m = entry_for(*result, 7401);
    check(m != nullptr && has(m->flags, ROBOTWEAX_SRT_TELEMETRY_MEMBER_REMOVED) &&
              has(m->flags, ROBOTWEAX_SRT_TELEMETRY_MEMBER_STATS_NONE),
          "a removed member that never had statistics");
}

void test_carrying_per_direction()
{
    reset_fake();
    add_member(7500, 1, 1, "10.0.1.2", 9000, "10.0.1.1", 41000);
    auto config = config_for(ROBOTWEAX_SRT_TELEMETRY_GROUP_TYPE_BACKUP);
    config.member_qualified_fields = ROBOTWEAX_SRT_TELEMETRY_FIELD_UNIQUE_SENT_PACKET_TOTAL |
                                     ROBOTWEAX_SRT_TELEMETRY_FIELD_RECEIVED_PACKET_TOTAL;
    config.member_qualified_ext_fields = 0;
    auto session = make_session(config);
    auto result = fresh_result();
    Ticker ticker;
    ticker.sample(session.get(), *result);
    find(7500)->received = 10;
    ticker.sample(session.get(), *result);
    const auto *m = entry_for(*result, 7500);
    check(m != nullptr && has(m->flags, ROBOTWEAX_SRT_TELEMETRY_MEMBER_CARRYING),
          "the receive direction counts with only the plain received total qualified");
}

void test_state_only_session()
{
    reset_fake();
    add_member(7001, 1, 1, "10.0.1.2", 9000, "10.0.1.1", 41000);
    auto config = config_for(ROBOTWEAX_SRT_TELEMETRY_GROUP_TYPE_BACKUP);
    config.capabilities = ROBOTWEAX_SRT_TELEMETRY_CAP_GROUP_MEMBERS_V1;
    config.group_qualified_fields = 0;
    config.group_qualified_ext_fields = 0;
    config.member_qualified_fields = 0;
    config.member_qualified_ext_fields = 0;
    auto session = make_session(config);
    auto result = fresh_result();
    Ticker ticker;
    ticker.sample(session.get(), *result);
    find(7001)->sent = 10;
    ticker.sample(session.get(), *result);
    const auto *m = entry_for(*result, 7001);
    check(m != nullptr && m->raw.available_fields == 0 &&
              !has(m->flags, ROBOTWEAX_SRT_TELEMETRY_MEMBER_CARRYING) &&
              std::string(m->path) == "token-1",
          "without qualified fields: states and paths only, no carrying");
}

std::vector<ROBOTWEAX_SRT_GroupPathDataV1_> path_records;
int path_calls = 0;
bool path_failure = false;
std::size_t path_growth = 0;
bool path_bad_count = false;

int fake_path_data(const std::int32_t group, ROBOTWEAX_SRT_GroupPathDataV1_ *output,
                   std::size_t *count)
{
    ++path_calls;
    check(group == group_id, "path getter receives the owning group");
    const auto capacity = *count;
    *count = path_growth != 0 ? path_growth : path_records.size();
    if (path_failure || path_growth != 0 || *count > capacity)
    {
        return -1;
    }
    std::copy(path_records.begin(), path_records.end(), output);
    return path_bad_count ? static_cast<int>(*count + 1) : static_cast<int>(*count);
}

ROBOTWEAX_SRT_GroupPathDataV1_ label_record(const std::int32_t socket,
                                            const std::vector<std::uint8_t> &bytes,
                                            const std::uint32_t flags = 2)
{
    ROBOTWEAX_SRT_GroupPathDataV1_ record{};
    record.member_id = socket;
    record.flags = bytes.empty() ? 0 : flags;
    record.identifier_length = static_cast<std::uint32_t>(bytes.size());
    std::copy(bytes.begin(), bytes.end(), record.identifier);
    return record;
}

void test_path_identifiers()
{
    reset_fake();
    add_member(8100, -1, 1, "10.0.1.2", 40001, "10.0.1.1", 9000);
    add_member(8101, 17, 1, "10.0.2.2", 40002, "10.0.2.1", 9000);
    auto config = config_for(ROBOTWEAX_SRT_TELEMETRY_GROUP_TYPE_BROADCAST);
    auto unqualified = make_session(config);
    check(robotweax_srt_telemetry_public_srt_group_session_set_path_data_v1(
              unqualified.get(), fake_path_data) == ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT,
          "getter needs exact capability qualification");
    check(robotweax_srt_telemetry_public_srt_group_session_set_path_data_v1(nullptr, nullptr) ==
              ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT,
          "null session rejected");
    config.capabilities |= ROBOTWEAX_SRT_TELEMETRY_CAP_GROUP_PATH_IDENTIFIERS_V1;
    config.provider = ROBOTWEAX_SRT_TELEMETRY_PROVIDER_HAIVISION_SRT;
    auto other = make_session(config);
    check(robotweax_srt_telemetry_public_srt_group_session_set_path_data_v1(
              other.get(), fake_path_data) == ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT,
          "Haivision does not implement this private extension");
    config.provider = ROBOTWEAX_SRT_TELEMETRY_PROVIDER_ROBOTWEAX_SRT;
    auto session = make_session(config);
    check(robotweax_srt_telemetry_public_srt_group_session_set_path_data_v1(
              session.get(), fake_path_data) == ROBOTWEAX_SRT_TELEMETRY_OK,
          "qualified getter enabled");
    path_records = {label_record(8101, {'b'}), label_record(8100, {0, 0xff, 'a'})};
    path_calls = 0;
    path_failure = true;
    path_growth = 0;
    path_bad_count = false;
    auto result = fresh_result();
    Ticker ticker;
    check(ticker.sample(session.get(), *result) == ROBOTWEAX_SRT_TELEMETRY_OK,
          "missing path metadata does not fail statistics");
    const auto *m = entry_for(*result, 8100);
    check(m != nullptr && m->path[0] == '\0' &&
              has(m->flags, ROBOTWEAX_SRT_TELEMETRY_MEMBER_PATH_PENDING) &&
              !has(m->flags, ROBOTWEAX_SRT_TELEMETRY_MEMBER_PATH_ASSIGNED),
          "temporary failure does not freeze an address fallback");
    path_failure = false;
    ticker.sample(session.get(), *result);
    m = entry_for(*result, 8100);
    check(m != nullptr && std::string(m->path) == "pathid-00ff61" &&
              m->path_source == ROBOTWEAX_SRT_TELEMETRY_PATH_SOURCE_IDENTIFIER && m->token == -1 &&
              has(m->flags, ROBOTWEAX_SRT_TELEMETRY_MEMBER_PATH_ASSIGNED) &&
              !has(m->flags, ROBOTWEAX_SRT_TELEMETRY_MEMBER_PATH_PENDING),
          "binary receiver label is escaped, joined by socket ID and token stays local");
    check(std::string(entry_for(*result, 8101)->path) == "pathid-62" && path_calls == 2,
          "one snapshot per tick, not per member; negotiated label precedes token fallback");
    check(robotweax_srt_telemetry_public_srt_group_session_set_path_data_v1(session.get(), nullptr) ==
              ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT,
          "late getter mutation rejected");
    add_member(8102, -1, 1, "10.0.1.2", 41001, "10.0.1.1", 9000);
    path_records.push_back(label_record(8102, {0, 0xff, 'a'}));
    path_records[1] = label_record(8100, {'x'});
    ticker.sample(session.get(), *result);
    check(std::string(entry_for(*result, 8100)->path) == "pathid-00ff61" &&
              std::string(entry_for(*result, 8102)->path) == "pathid-00ff61" &&
              entry_for(*result, 8100)->socket != entry_for(*result, 8102)->socket,
          "same-label replacement overlap preserves distinct sockets and cached names");
    find(8100)->data.sockstate = SRTS_BROKEN;
    find(8100)->data.memberstate = SRT_GST_BROKEN;
    path_failure = true;
    ticker.sample(session.get(), *result);
    check(std::string(entry_for(*result, 8100)->path) == "pathid-00ff61",
          "broken member retains cached label despite getter failure");
    remove_member(8100);
    ticker.sample(session.get(), *result);
    check(has(entry_for(*result, 8100)->flags, ROBOTWEAX_SRT_TELEMETRY_MEMBER_REMOVED) &&
              std::string(entry_for(*result, 8100)->path) == "pathid-00ff61",
          "removal reports the final cached identity");
    path_failure = false;
    ticker.sample(session.get(), *result);
    check(entry_for(*result, 8100) == nullptr, "no permanent removed-label history");

    /* Each invalid state must remain unknown, then recover without recreating
     * the session. Exercise absence, provisional offer and malformed metadata. */
    for (int malformed = 0; malformed < 8; ++malformed)
    {
        reset_fake();
        add_member(8200, -1, 1, "10.0.1.2", 40001, "10.0.1.1", 9000);
        auto retry = make_session(config);
        robotweax_srt_telemetry_public_srt_group_session_set_path_data_v1(retry.get(), fake_path_data);
        path_records = {label_record(8200, {'a'})};
        path_growth = 0;
        path_bad_count = false;
        switch (malformed)
        {
        case 0: path_records.clear(); break;
        case 1: path_records[0].flags = 1; break;
        case 2: path_records[0].identifier_length = 33; break;
        case 3: path_records[0].identifier[31] = 1; break;
        case 4: path_records[0].flags = 4; break;
        case 5: path_records.push_back(path_records[0]); break;
        case 6: path_growth = 1025; break;
        case 7: path_bad_count = true; break;
        }
        ticker.sample(retry.get(), *result);
        check(has(entry_for(*result, 8200)->flags, ROBOTWEAX_SRT_TELEMETRY_MEMBER_PATH_PENDING),
              "invalid or missing record is not a fallback name");
        path_growth = 0;
        path_bad_count = false;
        path_records = {label_record(8200, std::vector<std::uint8_t>(32, 0xff))};
        ticker.sample(retry.get(), *result);
        check(std::string(entry_for(*result, 8200)->path) == "pathid-" + std::string(64, 'f'),
              "retry accepts all 32 opaque bytes without truncation");
    }
    reset_fake();
    add_member(8300, 22, 1, "10.0.1.2", 40001, "10.0.1.1", 9000);
    auto unlabeled = make_session(config);
    robotweax_srt_telemetry_public_srt_group_session_set_path_data_v1(unlabeled.get(), fake_path_data);
    path_records = {label_record(8300, {})};
    ticker.sample(unlabeled.get(), *result);
    check(std::string(entry_for(*result, 8300)->path) == "token-22",
          "observed absence of a label keeps the existing token fallback");
    /* Growth within the budget is retried, including reordered unrelated IDs. */
    auto growing = make_session(config);
    robotweax_srt_telemetry_public_srt_group_session_set_path_data_v1(growing.get(), fake_path_data);
    path_records.assign(20, label_record(9999, {}));
    path_records[19] = label_record(8300, {'z'});
    path_calls = 0;
    ticker.sample(growing.get(), *result);
    check(path_calls == 2 && std::string(entry_for(*result, 8300)->path) == "pathid-7a",
          "bounded capacity growth recovers the requested record");
    int mode = 2;
    config.path_name = filling_path_name;
    config.path_name_user_data = &mode;
    auto custom = make_session(config);
    robotweax_srt_telemetry_public_srt_group_session_set_path_data_v1(custom.get(), fake_path_data);
    path_failure = true;
    ticker.sample(custom.get(), *result);
    check(entry_for(*result, 8300)->path_source == ROBOTWEAX_SRT_TELEMETRY_PATH_SOURCE_HOST &&
              !has(entry_for(*result, 8300)->flags, ROBOTWEAX_SRT_TELEMETRY_MEMBER_PATH_PENDING),
          "valid host override precedes unavailable peer metadata");
    path_failure = false;
    auto group_only = make_session(config);
    robotweax_srt_telemetry_public_srt_group_session_set_path_data_v1(group_only.get(), fake_path_data);
    auto zero = fresh_result(0);
    path_calls = 0;
    ticker.sample(group_only.get(), *zero);
    check(path_calls == 0, "group-only sampling does not read member path metadata");
    auto allocation = make_session(config);
    allocations_until_failure = 1;
    const auto error = robotweax_srt_telemetry_public_srt_group_session_set_path_data_v1(
        allocation.get(), fake_path_data);
    allocations_until_failure = 0;
    check(error == ROBOTWEAX_SRT_TELEMETRY_ALLOCATION_FAILURE,
          "setter allocation failure is contained at the C boundary");
    path_calls = 0;
    ticker.sample(allocation.get(), *result);
    check(path_calls == 0, "failed setter preserves disabled lookup");
}

} // namespace

int main()
{
    test_config_validation();
    test_result_validation();
    test_backup_caller_failover();
    test_listener_path_names();
    test_broadcast_carrying_set();
    test_backup_parallel_probe();
    test_group_states_and_failures();
    test_growth_and_truncation();
    test_capacity_zero();
    test_relisted_pending_removal();
    test_allocation_failure_sweep();
    test_group_data_attempts_exhausted();
    test_duplicate_listing();
    test_peer_address_not_reused();
    test_local_address_after_connect();
    test_first_listing_failure();
    test_path_name_bounds();
    test_long_ipv6_path();
    test_unknown_type_and_removed_active();
    test_carrying_per_direction();
    test_state_only_session();
    test_path_identifiers();
    if (failures != 0)
    {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "public SRT group tests passed\n";
    return 0;
}
