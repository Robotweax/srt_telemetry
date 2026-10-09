#include <robotweax/srt_telemetry_public_srt_group.h>

#include "public_srt_internal.hpp"
#include "public_srt_path_data.hpp"

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace detail = robotweax_srt_telemetry_detail;

namespace
{

constexpr int connected_state = 5; /* SRTS_CONNECTED */
constexpr std::int32_t group_handle_bit = INT32_C(1) << 30;
constexpr int group_data_attempts = 3;
constexpr std::size_t initial_buffer = 16;
/* Removed members waiting for room in a result; older ones are dropped. */
constexpr std::size_t max_pending_removals = 1024;
constexpr std::size_t max_path_records = 1024;

struct DeltaEngineDeleter
{
    void operator()(robotweax_srt_telemetry_delta_engine *engine) const noexcept
    {
        robotweax_srt_telemetry_delta_engine_destroy(engine);
    }
};
using DeltaEngine = std::unique_ptr<robotweax_srt_telemetry_delta_engine, DeltaEngineDeleter>;

/* What the session remembers about one member socket between calls. */
struct Member
{
    std::int32_t socket = -1;
    std::int32_t token = -1;
    std::int32_t socket_state = 0;
    std::int32_t member_state = 0;
    std::int32_t member_result = 0;
    std::uint32_t weight = 0;
    std::uint32_t path_source = ROBOTWEAX_SRT_TELEMETRY_PATH_SOURCE_NONE;
    std::string path;
    std::string local_address;
    bool local_final = false;
    std::string peer_address;
    /* Reported with MEMBER_NEW in a result that was handed out. */
    bool announced = false;
    /* STATE_CHANGED / PATH_ASSIGNED not yet in a result that was handed out
     * (the call that saw them failed). */
    std::uint32_t unreported_flags = 0;
    bool has_stats = false;
    robotweax_srt_telemetry_raw_sample_v1 last_raw{};
    robotweax_srt_telemetry_ext_sample_v1 last_ext{};
    DeltaEngine engine;
};

void init_counters(robotweax_srt_telemetry_raw_sample_v1 &raw, robotweax_srt_telemetry_delta_v1 &delta,
                   robotweax_srt_telemetry_ext_sample_v1 &ext,
                   robotweax_srt_telemetry_ext_delta_v1 &ext_delta)
{
    std::memset(&raw, 0, sizeof(raw));
    raw.struct_size = sizeof(raw);
    raw.abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
    std::memset(&ext, 0, sizeof(ext));
    ext.struct_size = sizeof(ext);
    ext.abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
    std::memset(&delta, 0, sizeof(delta));
    delta.struct_size = sizeof(delta);
    delta.abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
    std::memset(&ext_delta, 0, sizeof(ext_delta));
    ext_delta.struct_size = sizeof(ext_delta);
    ext_delta.abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
}

void init_member_entry(robotweax_srt_telemetry_group_member_v1 &entry)
{
    std::memset(&entry, 0, sizeof(entry));
    entry.struct_size = sizeof(entry);
    entry.abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
    init_counters(entry.raw, entry.delta, entry.ext, entry.ext_delta);
}

/* Sizes are chosen so that addresses and automatic path names always fit;
 * the bound only guards against a broken invariant. */
template <std::size_t N> void copy_text(char (&destination)[N], const std::string &source)
{
    const auto length = std::min(source.size(), N - 1);
    std::memcpy(destination, source.data(), length);
    destination[length] = '\0';
}

/* "ip:port", "[ipv6]:port" or "[ipv6%scope]:port"; empty for an unset or
 * unknown family. */
std::string address_text(const sockaddr_storage &address)
{
    char host[INET6_ADDRSTRLEN] = {};
    if (address.ss_family == AF_INET)
    {
        const auto *v4 = reinterpret_cast<const sockaddr_in *>(&address);
        if (inet_ntop(AF_INET, &v4->sin_addr, host, sizeof(host)) == nullptr)
        {
            return {};
        }
        return std::string(host) + ":" + std::to_string(ntohs(v4->sin_port));
    }
    if (address.ss_family == AF_INET6)
    {
        const auto *v6 = reinterpret_cast<const sockaddr_in6 *>(&address);
        if (inet_ntop(AF_INET6, &v6->sin6_addr, host, sizeof(host)) == nullptr)
        {
            return {};
        }
        std::string text = "[" + std::string(host);
        if (v6->sin6_scope_id != 0)
        {
            text += "%" + std::to_string(v6->sin6_scope_id);
        }
        return text + "]:" + std::to_string(ntohs(v6->sin6_port));
    }
    return {};
}

/* Host path names: 1..capacity-1 characters in 0x21..0x7E, NUL-terminated
 * within the buffer. */
bool valid_path_name(const char *name, const std::size_t capacity)
{
    const auto *end = static_cast<const char *>(std::memchr(name, '\0', capacity));
    if (end == nullptr || end == name)
    {
        return false;
    }
    return std::all_of(name, end, [](const char c) { return c >= 0x21 && c <= 0x7e; });
}

/* Stream packets of one direction: the unique total when it is qualified,
 * else the plain total. */
std::uint64_t direction_packets(const robotweax_srt_telemetry_delta_v1 &delta,
                                const std::uint64_t unique_field, const std::uint64_t unique_delta,
                                const std::uint64_t plain_field, const std::uint64_t plain_delta)
{
    const auto fields = delta.available_delta_fields;
    if ((fields & unique_field) != 0)
    {
        return unique_delta;
    }
    return (fields & plain_field) != 0 ? plain_delta : 0;
}

std::uint64_t stream_packets(const robotweax_srt_telemetry_delta_v1 &delta)
{
    return direction_packets(delta, ROBOTWEAX_SRT_TELEMETRY_FIELD_UNIQUE_SENT_PACKET_TOTAL,
                             delta.unique_sent_packet_delta,
                             ROBOTWEAX_SRT_TELEMETRY_FIELD_SENT_PACKET_TOTAL, delta.sent_packet_delta) +
           direction_packets(delta, ROBOTWEAX_SRT_TELEMETRY_FIELD_UNIQUE_RECEIVED_PACKET_TOTAL,
                             delta.unique_received_packet_delta,
                             ROBOTWEAX_SRT_TELEMETRY_FIELD_RECEIVED_PACKET_TOTAL,
                             delta.received_packet_delta);
}

/* Statistics config for the counter mapping. The socket is not used by the
 * mapping; members are sampled by their own socket. */
robotweax_srt_telemetry_public_srt_config_v2
stats_config(const robotweax_srt_telemetry_public_srt_group_config_v1 &config, const bool group)
{
    robotweax_srt_telemetry_public_srt_config_v2 stats{};
    stats.struct_size = sizeof(stats);
    stats.abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
    stats.socket = group ? config.group : -1;
    stats.provider = config.provider;
    stats.capabilities = config.capabilities;
    stats.qualified_fields = group ? config.group_qualified_fields : config.member_qualified_fields;
    stats.qualified_ext_fields =
        group ? config.group_qualified_ext_fields : config.member_qualified_ext_fields;
    stats.bistats = config.bistats;
    return stats;
}

/* A C host may pass any integer in an enum field; loading an out-of-range
 * value as the C++ enum type is undefined, so the bytes are read instead. */
bool known_provider_value(const robotweax_srt_telemetry_provider &provider)
{
    std::underlying_type_t<robotweax_srt_telemetry_provider> value{};
    static_assert(sizeof(value) == sizeof(provider));
    std::memcpy(&value, &provider, sizeof(value));
    return value == ROBOTWEAX_SRT_TELEMETRY_PROVIDER_HAIVISION_SRT ||
           value == ROBOTWEAX_SRT_TELEMETRY_PROVIDER_ROBOTWEAX_SRT;
}

bool valid_config(const robotweax_srt_telemetry_public_srt_group_config_v1 &config)
{
    const auto known_provider = known_provider_value(config.provider);
    const auto known_type = config.group_type == ROBOTWEAX_SRT_TELEMETRY_GROUP_TYPE_UNKNOWN ||
                            config.group_type == ROBOTWEAX_SRT_TELEMETRY_GROUP_TYPE_BROADCAST ||
                            config.group_type == ROBOTWEAX_SRT_TELEMETRY_GROUP_TYPE_BACKUP;
    const auto caps = config.capabilities;
    const auto has_common = (caps & ROBOTWEAX_SRT_TELEMETRY_CAP_COMMON_STATS_V1) != 0;
    const auto any_fields = (config.group_qualified_fields | config.group_qualified_ext_fields |
                             config.member_qualified_fields | config.member_qualified_ext_fields) != 0;
    const auto member_ext = config.member_qualified_ext_fields;
    const auto filter_ok =
        (member_ext & ROBOTWEAX_SRT_TELEMETRY_PUBLIC_SRT_EXT_FILTER_CANDIDATE_FIELDS) == 0 ||
        (caps & ROBOTWEAX_SRT_TELEMETRY_CAP_FEC_STATS_V1) != 0;
    return config.group >= 0 && (config.group & group_handle_bit) != 0 && known_provider &&
           known_type && config.reserved == 0 &&
           (caps & ROBOTWEAX_SRT_TELEMETRY_CAP_GROUP_MEMBERS_V1) != 0 && (!any_fields || has_common) &&
           config.bistats != nullptr && config.group_data != nullptr &&
           config.getsockname != nullptr && config.getsockstate != nullptr &&
           (config.group_qualified_fields &
            ~ROBOTWEAX_SRT_TELEMETRY_PUBLIC_SRT_GROUP_CANDIDATE_FIELDS) == 0 &&
           (config.group_qualified_ext_fields &
            ~ROBOTWEAX_SRT_TELEMETRY_PUBLIC_SRT_GROUP_EXT_CANDIDATE_FIELDS) == 0 &&
           (config.member_qualified_fields &
            ~ROBOTWEAX_SRT_TELEMETRY_PUBLIC_SRT_V1_5_5_CANDIDATE_FIELDS) == 0 &&
           (member_ext & ~detail::provider_ext_candidates(config.provider)) == 0 && filter_ok;
}

} // namespace

struct robotweax_srt_telemetry_public_srt_group_session
{
    robotweax_srt_telemetry_public_srt_group_config_v1 config{};
    robotweax_srt_telemetry_public_srt_config_v2 group_stats{};
    robotweax_srt_telemetry_public_srt_config_v2 member_stats{};
    DeltaEngine group_engine;
    /* In order of first appearance. */
    std::vector<std::unique_ptr<Member>> members;
    /* Removed members not reported yet (no room in an earlier result). */
    std::vector<std::unique_ptr<Member>> removed;
    std::vector<SRT_SOCKGROUPDATA> buffer;
    robotweax_srt_telemetry_public_srt_group_path_data_v1_fn path_data = nullptr;
    std::vector<ROBOTWEAX_SRT_GroupPathDataV1_> path_buffer;
    std::size_t path_count = 0;
    bool path_snapshot_available = false;
    bool sample_started = false;
    std::vector<std::int32_t> carrying;
    /* Reserved before sampling so update_active cannot throw. */
    std::vector<std::int32_t> carrying_scratch;
    std::int32_t active_member = -1;
    std::int32_t group_state = 0;
    bool state_known = false;
    bool listed = false;
};

namespace
{

using Session = robotweax_srt_telemetry_public_srt_group_session;
using Entry = robotweax_srt_telemetry_group_member_v1;
using Result = robotweax_srt_telemetry_public_srt_group_result_v1;

/* Reads the member list, growing the buffer when the group outgrew it. The
 * buffer is cleared before each read: a provider does not write peeraddr for
 * a member whose peer is not known yet (Haivision SRT while connecting). */
int read_group_data(Session &session, std::size_t &listed)
{
    int result = -1;
    for (int attempt = 0; attempt < group_data_attempts; ++attempt)
    {
        std::fill(session.buffer.begin(), session.buffer.end(), SRT_SOCKGROUPDATA{});
        auto count = session.buffer.size();
        result = session.config.group_data(session.config.group, session.buffer.data(), &count);
        if (result >= 0)
        {
            listed = std::min({static_cast<std::size_t>(result), count, session.buffer.size()});
            return result;
        }
        if (count <= session.buffer.size())
        {
            return result;
        }
        session.buffer.resize(count);
    }
    return result;
}

struct PathName
{
    std::string name;
    std::uint32_t source = ROBOTWEAX_SRT_TELEMETRY_PATH_SOURCE_NONE;
    bool pending = false;
};

/* Independent snapshots are joined by member ID, never by array position.
 * Three bounded attempts handle membership growth. A failed/oversized/malformed
 * snapshot is missing metadata, not an observed absence of a label. */
void read_path_data(Session &session)
{
    session.path_count = 0;
    session.path_snapshot_available = false;
    for (int attempt = 0; attempt < group_data_attempts; ++attempt)
    {
        std::fill(session.path_buffer.begin(), session.path_buffer.end(),
                  ROBOTWEAX_SRT_GroupPathDataV1_{});
        auto count = session.path_buffer.size();
        const auto result = session.path_data(session.config.group, session.path_buffer.data(), &count);
        if (result >= 0)
        {
            if (static_cast<std::size_t>(result) != count || count > session.path_buffer.size())
            {
                return;
            }
            session.path_count = count;
            session.path_snapshot_available = true;
            return;
        }
        if (count <= session.path_buffer.size() || count > max_path_records)
        {
            return;
        }
        session.path_buffer.resize(count);
    }
}

PathName identifier_for(const Session &session, const std::int32_t socket)
{
    if (!session.path_snapshot_available)
    {
        return {{}, ROBOTWEAX_SRT_TELEMETRY_PATH_SOURCE_NONE, true};
    }
    const ROBOTWEAX_SRT_GroupPathDataV1_ *found = nullptr;
    for (std::size_t i = 0; i < session.path_count; ++i)
    {
        const auto &record = session.path_buffer[i];
        if (record.member_id == socket)
        {
            if (found != nullptr)
            {
                return {{}, ROBOTWEAX_SRT_TELEMETRY_PATH_SOURCE_NONE, true};
            }
            found = &record;
        }
    }
    if (found == nullptr || found->identifier_length > 32 || (found->flags & ~3U) != 0 ||
        (found->identifier_length == 0 && found->flags != 0) ||
        !std::all_of(found->identifier + found->identifier_length, found->identifier + 32,
                     [](const auto byte) { return byte == 0; }))
    {
        return {{}, ROBOTWEAX_SRT_TELEMETRY_PATH_SOURCE_NONE, true};
    }
    if (found->identifier_length == 0)
    {
        return {}; /* An actual unlabeled record permits the ordinary fallback. */
    }
    if ((found->flags & 2U) == 0)
    {
        return {{}, ROBOTWEAX_SRT_TELEMETRY_PATH_SOURCE_NONE, true};
    }
    constexpr char hex[] = "0123456789abcdef";
    std::string name = "pathid-";
    name.reserve(7 + 2 * found->identifier_length);
    for (std::size_t i = 0; i < found->identifier_length; ++i)
    {
        const auto byte = found->identifier[i];
        name += hex[byte >> 4];
        name += hex[byte & 15];
    }
    return {std::move(name), ROBOTWEAX_SRT_TELEMETRY_PATH_SOURCE_IDENTIFIER};
}

/* Names the path of a member with the given (not yet committed) identity. */
PathName path_for(const Session &session, const std::int32_t socket, const std::int32_t token,
                  const std::uint32_t weight, const std::string &local, const std::string &peer)
{
    robotweax_srt_telemetry_group_member_identity_v1 identity{};
    identity.struct_size = sizeof(identity);
    identity.abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
    identity.group = session.config.group;
    identity.socket = socket;
    identity.token = token;
    identity.weight = weight;
    copy_text(identity.local_address, local);
    copy_text(identity.peer_address, peer);

    if (session.config.path_name != nullptr)
    {
        char name[ROBOTWEAX_SRT_TELEMETRY_GROUP_PATH_CAPACITY] = {};
        if (session.config.path_name(session.config.path_name_user_data, &identity, name,
                                     sizeof(name)) != 0 &&
            valid_path_name(name, sizeof(name)))
        {
            return {name, ROBOTWEAX_SRT_TELEMETRY_PATH_SOURCE_HOST};
        }
    }
    if (session.path_data != nullptr)
    {
        auto identifier = identifier_for(session, socket);
        if (identifier.pending || identifier.source != ROBOTWEAX_SRT_TELEMETRY_PATH_SOURCE_NONE)
        {
            return identifier;
        }
    }
    if (token >= 0)
    {
        return {"token-" + std::to_string(token), ROBOTWEAX_SRT_TELEMETRY_PATH_SOURCE_TOKEN};
    }
    return {(local.empty() ? std::string("?") : local) + ">" + (peer.empty() ? std::string("?") : peer),
            ROBOTWEAX_SRT_TELEMETRY_PATH_SOURCE_ADDRESSES};
}

void fill_entry_identity(const Member &member, Entry &entry)
{
    entry.socket = member.socket;
    entry.token = member.token;
    entry.socket_state = member.socket_state;
    entry.member_state = member.member_state;
    entry.member_result = member.member_result;
    entry.weight = member.weight;
    entry.path_source = member.path_source;
    copy_text(entry.path, member.path);
    copy_text(entry.local_address, member.local_address);
    copy_text(entry.peer_address, member.peer_address);
}

void fill_removed_entry(const Member &member, Entry &entry)
{
    init_member_entry(entry);
    entry.flags = ROBOTWEAX_SRT_TELEMETRY_MEMBER_REMOVED |
                  (member.has_stats ? ROBOTWEAX_SRT_TELEMETRY_MEMBER_STATS_STALE
                                    : ROBOTWEAX_SRT_TELEMETRY_MEMBER_STATS_NONE);
    if (member.has_stats)
    {
        entry.raw = member.last_raw;
        entry.ext = member.last_ext;
    }
    fill_entry_identity(member, entry);
}

/* Clears everything the call fills; keeps the caller's member array. */
void initialize_result(Result &result)
{
    auto *const members = result.members;
    const auto capacity = result.member_capacity;
    const auto entry_size = result.member_entry_size;
    std::memset(&result, 0, sizeof(result));
    result.struct_size = sizeof(result);
    result.abi_version = ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION;
    result.members = members;
    result.member_capacity = capacity;
    result.member_entry_size = entry_size;
    result.active_member = -1;
    result.previous_active_member = -1;
    init_counters(result.raw, result.delta, result.ext, result.ext_delta);
}

bool usable(const SRT_SOCKGROUPDATA &data)
{
    return static_cast<int>(data.sockstate) == connected_state &&
           static_cast<int>(data.memberstate) != ROBOTWEAX_SRT_TELEMETRY_MEMBER_STATE_BROKEN;
}

/* Samples one listed member into entry. Everything that can throw (text and
 * the host's path name) is computed before the member is changed, so a
 * failure leaves the member as it was. Returns the delta engine's error. */
robotweax_srt_telemetry_error sample_member(Session &session, Member &member,
                                            const SRT_SOCKGROUPDATA &data,
                                            const std::uint64_t observed_at_unix_nano,
                                            const std::uint64_t sample_monotonic_nano, Entry &entry)
{
    init_member_entry(entry);
    const auto socket_state = static_cast<std::int32_t>(data.sockstate);
    const auto member_state = static_cast<std::int32_t>(data.memberstate);

    auto peer = address_text(data.peeraddr);
    if (peer.empty())
    {
        peer = member.peer_address;
    }
    auto local = member.local_address;
    auto local_final = member.local_final;
    /* A connecting member is bound to the wildcard; the address is final once
     * the member is CONNECTED. */
    if (!local_final)
    {
        sockaddr_storage address{};
        int length = static_cast<int>(sizeof(address));
        if (session.config.getsockname(member.socket, reinterpret_cast<sockaddr *>(&address),
                                       &length) == 0)
        {
            auto text = address_text(address);
            if (!text.empty())
            {
                local = std::move(text);
                local_final = socket_state == connected_state;
            }
        }
    }
    PathName path;
    const auto name_path = member.path_source == ROBOTWEAX_SRT_TELEMETRY_PATH_SOURCE_NONE &&
                           socket_state == connected_state;
    if (name_path)
    {
        path = path_for(session, member.socket, data.token, data.weight, local, peer);
    }

    /* Commit: nothing below throws. Changes go through unreported_flags, which
     * only the end of a successful call clears. */
    if (!member.announced)
    {
        entry.flags |= ROBOTWEAX_SRT_TELEMETRY_MEMBER_NEW;
    }
    else if (socket_state != member.socket_state || member_state != member.member_state)
    {
        member.unreported_flags |= ROBOTWEAX_SRT_TELEMETRY_MEMBER_STATE_CHANGED;
    }
    member.socket_state = socket_state;
    member.member_state = member_state;
    member.member_result = data.result;
    member.token = data.token;
    member.weight = data.weight;
    member.peer_address = std::move(peer);
    member.local_address = std::move(local);
    member.local_final = local_final;
    if (name_path && path.source != ROBOTWEAX_SRT_TELEMETRY_PATH_SOURCE_NONE)
    {
        member.path = std::move(path.name);
        member.path_source = path.source;
        member.unreported_flags |= ROBOTWEAX_SRT_TELEMETRY_MEMBER_PATH_ASSIGNED;
    }
    if (path.pending)
    {
        entry.flags |= ROBOTWEAX_SRT_TELEMETRY_MEMBER_PATH_PENDING;
    }
    entry.flags |= member.unreported_flags;
    if (usable(data))
    {
        entry.flags |= ROBOTWEAX_SRT_TELEMETRY_MEMBER_USABLE;
    }

    auto error = ROBOTWEAX_SRT_TELEMETRY_OK;
    SRT_TRACEBSTATS statistics{};
    entry.native_result = session.config.bistats(member.socket, &statistics, 0, 1);
    if (entry.native_result == 0)
    {
        detail::map_raw(statistics, session.member_stats, observed_at_unix_nano,
                        sample_monotonic_nano, entry.raw);
        detail::map_ext(statistics, session.member_stats, entry.ext);
        error = robotweax_srt_telemetry_delta_engine_update_ext(member.engine.get(), &entry.raw,
                                                                &entry.ext, &entry.delta,
                                                                &entry.ext_delta);
        member.last_raw = entry.raw;
        member.last_ext = entry.ext;
        member.has_stats = true;
        if (stream_packets(entry.delta) > 0)
        {
            entry.flags |= ROBOTWEAX_SRT_TELEMETRY_MEMBER_CARRYING;
        }
    }
    else if (member.has_stats)
    {
        entry.flags |= ROBOTWEAX_SRT_TELEMETRY_MEMBER_STATS_STALE;
        entry.raw = member.last_raw;
        entry.ext = member.last_ext;
    }
    else
    {
        entry.flags |= ROBOTWEAX_SRT_TELEMETRY_MEMBER_STATS_NONE;
    }
    fill_entry_identity(member, entry);
    return error;
}

/* Main/Backup: the carrying member with the most stream packets. Broadcast:
 * the set of carrying members. Unknown type: nothing. Idle ticks keep the
 * previous answer. */
void update_active(Session &session, Result &result,
                   const std::vector<std::pair<std::int32_t, std::uint64_t>> &carrying)
{
    result.previous_active_member = session.active_member;
    result.active_member = session.active_member;
    if (carrying.empty() || session.config.group_type == ROBOTWEAX_SRT_TELEMETRY_GROUP_TYPE_UNKNOWN)
    {
        return;
    }
    if (session.config.group_type == ROBOTWEAX_SRT_TELEMETRY_GROUP_TYPE_BACKUP)
    {
        auto best = carrying.front();
        for (const auto &candidate : carrying)
        {
            if (candidate.second > best.second ||
                (candidate.second == best.second && candidate.first == session.active_member))
            {
                best = candidate;
            }
        }
        if (best.first != session.active_member)
        {
            result.flags |= ROBOTWEAX_SRT_TELEMETRY_GROUP_ACTIVE_CHANGED;
            session.active_member = best.first;
        }
        result.active_member = session.active_member;
        return;
    }
    /* carrying_scratch was reserved for every listed member before sampling. */
    auto &sockets = session.carrying_scratch;
    sockets.clear();
    for (const auto &entry : carrying)
    {
        sockets.push_back(entry.first);
    }
    std::sort(sockets.begin(), sockets.end());
    if (sockets != session.carrying)
    {
        result.flags |= ROBOTWEAX_SRT_TELEMETRY_GROUP_ACTIVE_CHANGED;
        session.carrying.swap(sockets);
    }
}

/* The listing with duplicates removed (a provider listing one socket twice),
 * in provider order. */
std::vector<std::size_t> distinct_listing(const Session &session, const std::size_t listed)
{
    std::vector<std::size_t> order;
    order.reserve(listed);
    for (std::size_t i = 0; i < listed; ++i)
    {
        const auto socket = session.buffer[i].id;
        const auto duplicate = std::any_of(order.begin(), order.end(), [&](const std::size_t j) {
            return session.buffer[j].id == socket;
        });
        if (!duplicate)
        {
            order.push_back(i);
        }
    }
    return order;
}

/* One call. Session changes that decide what later calls report (pending
 * removals, announced members, the first listing) are committed only at the
 * end, so a throw (std::bad_alloc) makes the next call report them again
 * instead of losing them. */
robotweax_srt_telemetry_error sample_locked(Session &session, const std::uint64_t observed_at_unix_nano,
                                            const std::uint64_t sample_monotonic_nano, Result &result)
{
    const auto &config = session.config;
    result.group_type = config.group_type;

    result.group_state = static_cast<std::int32_t>(config.getsockstate(config.group));
    if (session.state_known && result.group_state != session.group_state)
    {
        result.flags |= ROBOTWEAX_SRT_TELEMETRY_GROUP_STATE_CHANGED;
    }
    const auto commit_group_state = [&session, &result]() noexcept {
        session.group_state = result.group_state;
        session.state_known = true;
    };
    result.active_member = session.active_member;
    result.previous_active_member = session.active_member;

    std::size_t listed = 0;
    result.group_data_native_result = read_group_data(session, listed);
    if (result.group_data_native_result < 0)
    {
        commit_group_state();
        return ROBOTWEAX_SRT_TELEMETRY_PROVIDER_SAMPLE_FAILURE;
    }
    const auto first = !session.listed;
    if (first)
    {
        result.flags |= ROBOTWEAX_SRT_TELEMETRY_GROUP_FIRST_SAMPLE;
    }
    const auto order = distinct_listing(session, listed);
    result.listed_member_count = static_cast<std::uint32_t>(order.size());
    if (session.path_data != nullptr && result.member_capacity != 0)
    {
        read_path_data(session);
    }

    /* Resolve every listed socket to exactly one Member: a known member, a
     * pending removal that is listed again (it returns, and its removal is not
     * reported), or a new member. Each Member lives in exactly one of
     * session.members and session.removed at every point. */
    std::vector<Member *> listed_members;
    listed_members.reserve(order.size());
    session.members.reserve(session.members.size() + order.size());
    for (const auto index : order)
    {
        const auto socket = static_cast<std::int32_t>(session.buffer[index].id);
        const auto by_socket = [socket](const auto &m) { return m->socket == socket; };
        auto found = std::find_if(session.members.begin(), session.members.end(), by_socket);
        if (found == session.members.end())
        {
            const auto pending =
                std::find_if(session.removed.begin(), session.removed.end(), by_socket);
            if (pending != session.removed.end())
            {
                session.members.push_back(std::move(*pending));
                session.removed.erase(pending);
            }
            else
            {
                auto member = std::make_unique<Member>();
                member->socket = socket;
                robotweax_srt_telemetry_delta_engine *engine = nullptr;
                const auto error = robotweax_srt_telemetry_delta_engine_create(&engine);
                if (error != ROBOTWEAX_SRT_TELEMETRY_OK)
                {
                    return error;
                }
                member->engine.reset(engine);
                session.members.push_back(std::move(member));
            }
            found = std::prev(session.members.end());
        }
        listed_members.push_back(found->get());
    }

    /* Members no longer listed join the pending removals. */
    {
        std::vector<std::unique_ptr<Member>> kept;
        kept.reserve(session.members.size());
        session.removed.reserve(session.removed.size() + session.members.size());
        for (auto &member : session.members)
        {
            if (std::find(listed_members.begin(), listed_members.end(), member.get()) !=
                listed_members.end())
            {
                kept.push_back(std::move(member));
            }
            else if (member->announced)
            {
                session.removed.push_back(std::move(member));
            }
            /* else: never reported, so its removal is not reported either. */
        }
        session.members = std::move(kept);
    }
    /* Without a member array there is nowhere to report removals; a long
     * backlog keeps only the newest. */
    if (result.member_capacity == 0)
    {
        session.removed.clear();
    }
    else if (session.removed.size() > max_pending_removals)
    {
        session.removed.erase(session.removed.begin(),
                              session.removed.end() - static_cast<std::ptrdiff_t>(max_pending_removals));
        result.flags |= ROBOTWEAX_SRT_TELEMETRY_GROUP_TRUNCATED;
    }

    auto error = ROBOTWEAX_SRT_TELEMETRY_OK;
    {
        SRT_TRACEBSTATS statistics{};
        result.group_native_result = config.bistats(config.group, &statistics, 0, 1);
        if (result.group_native_result == 0)
        {
            detail::map_raw(statistics, session.group_stats, observed_at_unix_nano,
                            sample_monotonic_nano, result.raw);
            detail::map_ext(statistics, session.group_stats, result.ext);
            error = robotweax_srt_telemetry_delta_engine_update_ext(
                session.group_engine.get(), &result.raw, &result.ext, &result.delta,
                &result.ext_delta);
            result.flags |= ROBOTWEAX_SRT_TELEMETRY_GROUP_STATS;
        }
    }

    std::uint32_t written = 0;
    result.required_member_count =
        static_cast<std::uint32_t>(session.removed.size() + listed_members.size());

    /* Removed members first, so they are not starved by a full group. */
    std::size_t reported_removals = 0;
    for (; reported_removals < session.removed.size() && written < result.member_capacity;
         ++reported_removals)
    {
        fill_removed_entry(*session.removed[reported_removals], result.members[written++]);
    }

    /* Listed members that do not fit are not sampled at all: their changes,
     * NEW flag and deltas carry over to the next call with room. */
    std::vector<std::pair<std::int32_t, std::uint64_t>> carrying;
    carrying.reserve(listed_members.size());
    std::vector<Member *> reported;
    reported.reserve(listed_members.size());
    session.carrying_scratch.reserve(listed_members.size());
    for (std::size_t k = 0; k < listed_members.size(); ++k)
    {
        const auto &data = session.buffer[order[k]];
        if (usable(data))
        {
            ++result.usable_member_count;
        }
        if (written >= result.member_capacity)
        {
            continue;
        }
        auto &entry = result.members[written];
        const auto member_error = sample_member(session, *listed_members[k], data,
                                                observed_at_unix_nano, sample_monotonic_nano, entry);
        ++written;
        reported.push_back(listed_members[k]);
        if (error == ROBOTWEAX_SRT_TELEMETRY_OK)
        {
            error = member_error;
        }
        if ((entry.flags & ROBOTWEAX_SRT_TELEMETRY_MEMBER_STATE_CHANGED) != 0)
        {
            result.flags |= ROBOTWEAX_SRT_TELEMETRY_GROUP_MEMBER_STATE_CHANGED;
        }
        if ((entry.flags & ROBOTWEAX_SRT_TELEMETRY_MEMBER_CARRYING) != 0)
        {
            carrying.emplace_back(listed_members[k]->socket, stream_packets(entry.delta));
        }
    }
    if (!first)
    {
        update_active(session, result, carrying);
    }

    /* Commit (no throw from here on). */
    session.removed.erase(session.removed.begin(),
                          session.removed.begin() + static_cast<std::ptrdiff_t>(reported_removals));
    for (auto *member : reported)
    {
        member->announced = true;
        member->unreported_flags = 0;
    }
    session.listed = true;
    commit_group_state();
    result.member_count = written;

    /* Derived from the entries handed out, so a retried call reports it with
     * the entries it belongs to. */
    for (std::uint32_t i = 0; i < written; ++i)
    {
        const auto flags = result.members[i].flags;
        if ((flags & ROBOTWEAX_SRT_TELEMETRY_MEMBER_REMOVED) != 0 ||
            (!first && (flags & ROBOTWEAX_SRT_TELEMETRY_MEMBER_NEW) != 0))
        {
            result.flags |= ROBOTWEAX_SRT_TELEMETRY_GROUP_MEMBERS_CHANGED;
        }
    }

    if (written < result.required_member_count)
    {
        result.flags |= ROBOTWEAX_SRT_TELEMETRY_GROUP_TRUNCATED;
        if (error == ROBOTWEAX_SRT_TELEMETRY_OK)
        {
            error = ROBOTWEAX_SRT_TELEMETRY_BUFFER_TOO_SMALL;
        }
    }
    return error;
}

} // namespace

extern "C" robotweax_srt_telemetry_error robotweax_srt_telemetry_public_srt_group_session_create(
    const robotweax_srt_telemetry_public_srt_group_config_v1 *config,
    robotweax_srt_telemetry_public_srt_group_session **session)
{
    if (session == nullptr)
    {
        return ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT;
    }
    *session = nullptr;
    if (config == nullptr ||
        config->struct_size < sizeof(robotweax_srt_telemetry_public_srt_group_config_v1))
    {
        return ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT;
    }
    if (config->abi_version != ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION)
    {
        return ROBOTWEAX_SRT_TELEMETRY_ABI_MISMATCH;
    }
    if (!valid_config(*config))
    {
        return ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT;
    }
    try
    {
        auto created = std::make_unique<Session>();
        created->config = *config;
        created->config.struct_size = sizeof(created->config);
        created->group_stats = stats_config(*config, true);
        created->member_stats = stats_config(*config, false);
        robotweax_srt_telemetry_delta_engine *engine = nullptr;
        const auto error = robotweax_srt_telemetry_delta_engine_create(&engine);
        if (error != ROBOTWEAX_SRT_TELEMETRY_OK)
        {
            return error;
        }
        created->group_engine.reset(engine);
        created->buffer.resize(initial_buffer);
        *session = created.release();
        return ROBOTWEAX_SRT_TELEMETRY_OK;
    }
    catch (const std::bad_alloc &)
    {
        return ROBOTWEAX_SRT_TELEMETRY_ALLOCATION_FAILURE;
    }
}

extern "C" void robotweax_srt_telemetry_public_srt_group_session_destroy(
    robotweax_srt_telemetry_public_srt_group_session *session)
{
    delete session;
}

extern "C" robotweax_srt_telemetry_error
robotweax_srt_telemetry_public_srt_group_session_set_path_data_v1(
    robotweax_srt_telemetry_public_srt_group_session *session,
    robotweax_srt_telemetry_public_srt_group_path_data_v1_fn getter)
{
    if (session == nullptr || session->sample_started ||
        (getter != nullptr &&
         (session->config.provider != ROBOTWEAX_SRT_TELEMETRY_PROVIDER_ROBOTWEAX_SRT ||
          (session->config.capabilities & ROBOTWEAX_SRT_TELEMETRY_CAP_GROUP_PATH_IDENTIFIERS_V1) == 0)))
    {
        return ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT;
    }
    try
    {
        if (getter != nullptr)
        {
            session->path_buffer.resize(initial_buffer);
        }
        session->path_data = getter;
        return ROBOTWEAX_SRT_TELEMETRY_OK;
    }
    catch (const std::bad_alloc &)
    {
        return ROBOTWEAX_SRT_TELEMETRY_ALLOCATION_FAILURE;
    }
}

extern "C" robotweax_srt_telemetry_error robotweax_srt_telemetry_public_srt_group_session_sample(
    robotweax_srt_telemetry_public_srt_group_session *session,
    const std::uint64_t observed_at_unix_nano, const std::uint64_t sample_monotonic_nano,
    robotweax_srt_telemetry_public_srt_group_result_v1 *result)
{
    if (session == nullptr || result == nullptr)
    {
        return ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT;
    }
    if (result->struct_size < sizeof(*result) ||
        result->abi_version != ROBOTWEAX_SRT_TELEMETRY_ABI_VERSION ||
        (result->member_capacity != 0 && result->member_entry_size != sizeof(Entry)))
    {
        return ROBOTWEAX_SRT_TELEMETRY_ABI_MISMATCH;
    }
    if (result->member_capacity != 0 && result->members == nullptr)
    {
        return ROBOTWEAX_SRT_TELEMETRY_INVALID_ARGUMENT;
    }
    initialize_result(*result);
    session->sample_started = true;
    try
    {
        return sample_locked(*session, observed_at_unix_nano, sample_monotonic_nano, *result);
    }
    catch (const std::bad_alloc &)
    {
        result->member_count = 0;
        return ROBOTWEAX_SRT_TELEMETRY_ALLOCATION_FAILURE;
    }
}
