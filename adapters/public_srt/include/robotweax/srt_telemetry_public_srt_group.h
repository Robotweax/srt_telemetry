#ifndef ROBOTWEAX_SRT_TELEMETRY_PUBLIC_SRT_GROUP_H
#define ROBOTWEAX_SRT_TELEMETRY_PUBLIC_SRT_GROUP_H

/* Group session: one SRT connection group (Broadcast or Main/Backup) and all
 * of its member sockets, read in one call per sampling tick through the public
 * SRT API that the host injects. Each call returns the group's own counters
 * (srt_bistats on the group handle), and for every member its state, weight,
 * token, addresses, path name and its own counters (srt_bistats on the member),
 * plus what changed since the previous call. See
 * docs/architecture/public-srt-group.md. */

#include <robotweax/srt_telemetry_public_srt.h>
#include <robotweax/srt_telemetry_public_srt_connection.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

    struct SRT_SocketGroupData_;
    struct ROBOTWEAX_SRT_GroupPathDataV1_;
    struct sockaddr;

    /* srt_group_data and srt_getsockname with plain integer socket types; the
     * host can pass its own SRT library's functions directly (SRTSOCKET is
     * int32_t in both providers). <robotweax/srt_telemetry_public_srt_host.h>
     * fills them. */
    typedef int (*robotweax_srt_telemetry_public_srt_group_data_fn)(
        int32_t group, struct SRT_SocketGroupData_ *members, size_t *count);
    /* The selected provider's optional robotweax_srt_group_path_data_v1.
     * The SDK never resolves or links this symbol itself. */
    typedef int (*robotweax_srt_telemetry_public_srt_group_path_data_v1_fn)(
        int32_t group, struct ROBOTWEAX_SRT_GroupPathDataV1_ *members, size_t *count);
    typedef int (*robotweax_srt_telemetry_public_srt_getsockname_fn)(int32_t socket,
                                                                     struct sockaddr *name,
                                                                     int *length);

/* Group types follow SRT_GROUP_TYPE. The host declares it: SRTO_GROUPTYPE is
 * not readable on every provider (Robotweax SRT 0.2.8 refuses it on an
 * accepted group). */
#define ROBOTWEAX_SRT_TELEMETRY_GROUP_TYPE_UNKNOWN 0u
#define ROBOTWEAX_SRT_TELEMETRY_GROUP_TYPE_BROADCAST 1u
#define ROBOTWEAX_SRT_TELEMETRY_GROUP_TYPE_BACKUP 2u

/* member_state values follow SRT_MEMBERSTATUS. Providers set them inside the
 * application's send and receive calls, and the receiver side differs: use
 * MEMBER_CARRYING, not RUNNING, to tell which member carries the stream. */
#define ROBOTWEAX_SRT_TELEMETRY_MEMBER_STATE_PENDING 0
#define ROBOTWEAX_SRT_TELEMETRY_MEMBER_STATE_IDLE 1
#define ROBOTWEAX_SRT_TELEMETRY_MEMBER_STATE_RUNNING 2
#define ROBOTWEAX_SRT_TELEMETRY_MEMBER_STATE_BROKEN 3

/* Member flags. */
/* First result that reports this member socket. */
#define ROBOTWEAX_SRT_TELEMETRY_MEMBER_NEW (UINT32_C(1) << 0)
/* The member is no longer listed. The entry carries its last known values and
 * is reported exactly once, in the first call with room for it; the session
 * then forgets the socket. A member listed again before its removal was
 * reported simply continues as the same member (same path name, counters
 * continue; no REMOVED, no NEW). A member that was never
 * reported (no room in any result) is not reported as removed either. */
#define ROBOTWEAX_SRT_TELEMETRY_MEMBER_REMOVED (UINT32_C(1) << 1)
/* member_state or socket_state differs from the previous call. */
#define ROBOTWEAX_SRT_TELEMETRY_MEMBER_STATE_CHANGED (UINT32_C(1) << 2)
/* The member's sent or received stream packets advanced since the previous
 * call: it carried stream data during this tick. Per direction, the unique
 * packet total counts when it is qualified, else the plain total. */
#define ROBOTWEAX_SRT_TELEMETRY_MEMBER_CARRYING (UINT32_C(1) << 3)
/* srt_bistats on the member failed in this call (native_result); raw and ext
 * hold the last good values and the deltas are empty. Haivision SRT stops
 * answering for a member as soon as it breaks. */
#define ROBOTWEAX_SRT_TELEMETRY_MEMBER_STATS_STALE (UINT32_C(1) << 4)
/* srt_bistats on the member has never succeeded; raw and ext are empty. */
#define ROBOTWEAX_SRT_TELEMETRY_MEMBER_STATS_NONE (UINT32_C(1) << 5)
/* socket_state is CONNECTED and member_state is not BROKEN. */
#define ROBOTWEAX_SRT_TELEMETRY_MEMBER_USABLE (UINT32_C(1) << 6)
/* The path name was set in this call (see path_source). */
#define ROBOTWEAX_SRT_TELEMETRY_MEMBER_PATH_ASSIGNED (UINT32_C(1) << 7)
/* A connected, not-yet-named member has unavailable, malformed or still
 * provisional path metadata. Its path remains empty and is retried next tick;
 * counters remain available. A cached name is never discarded. */
#define ROBOTWEAX_SRT_TELEMETRY_MEMBER_PATH_PENDING (UINT32_C(1) << 8)

/* Where a member's path name came from. A path is named once, at the first
 * call that sees the member CONNECTED, and never changes afterwards. */
#define ROBOTWEAX_SRT_TELEMETRY_PATH_SOURCE_NONE 0u
/* The host's path_name callback. */
#define ROBOTWEAX_SRT_TELEMETRY_PATH_SOURCE_HOST 1u
/* "token-<n>" from the caller's SRT_SOCKGROUPCONFIG.token. Stable across
 * reconnects only when the host passes the same token again. Not available on
 * the accepting side, where SRT reports -1. */
#define ROBOTWEAX_SRT_TELEMETRY_PATH_SOURCE_TOKEN 2u
/* "<local>><peer>" addresses ("?" for an unknown side). On the accepting side
 * the peer port changes with every reconnect, so a reconnected path gets a new
 * name. */
#define ROBOTWEAX_SRT_TELEMETRY_PATH_SOURCE_ADDRESSES 3u
/* "pathid-" plus lowercase hex of all negotiated opaque label bytes.
 * Scoped to this host-authorized group/session; untrusted metadata. */
#define ROBOTWEAX_SRT_TELEMETRY_PATH_SOURCE_IDENTIFIER 4u

/* Group flags. */
/* srt_bistats on the group handle succeeded; raw/ext/deltas are filled. It
 * fails before the group's first connection. */
#define ROBOTWEAX_SRT_TELEMETRY_GROUP_STATS (UINT32_C(1) << 0)
/* First call of this session that listed the members; no member or active
 * change is reported. A call whose srt_group_data fails does not count (its
 * group state still counts for GROUP_STATE_CHANGED). */
#define ROBOTWEAX_SRT_TELEMETRY_GROUP_FIRST_SAMPLE (UINT32_C(1) << 1)
/* More entries than member_capacity: required_member_count says how many
 * were due. Listed members that did not fit are not sampled in this call, so
 * their changes, NEW flag and deltas carry over to the next call with room;
 * removed members that did not fit stay pending (at most 1024, the oldest are
 * dropped). With member_capacity 0 no member is sampled and removals are not
 * kept: the call reports the group only. */
#define ROBOTWEAX_SRT_TELEMETRY_GROUP_TRUNCATED (UINT32_C(1) << 2)
/* This result reports a MEMBER_REMOVED entry, or a MEMBER_NEW entry after the
 * first listing. With member_capacity 0 it is never set. */
#define ROBOTWEAX_SRT_TELEMETRY_GROUP_MEMBERS_CHANGED (UINT32_C(1) << 3)
/* A member's state changed. */
#define ROBOTWEAX_SRT_TELEMETRY_GROUP_MEMBER_STATE_CHANGED (UINT32_C(1) << 4)
/* The set of carrying members changed (Broadcast), or active_member changed
 * (Main/Backup). Ticks in which no member carries data (an idle stream) never
 * count as a change. Never set for GROUP_TYPE_UNKNOWN. */
#define ROBOTWEAX_SRT_TELEMETRY_GROUP_ACTIVE_CHANGED (UINT32_C(1) << 5)
/* group_state differs from the previous call. */
#define ROBOTWEAX_SRT_TELEMETRY_GROUP_STATE_CHANGED (UINT32_C(1) << 6)

/* "[ipv6%scope]:port" with the longest IPv6 text form fits. */
#define ROBOTWEAX_SRT_TELEMETRY_GROUP_ADDRESS_CAPACITY 80u
/* Two addresses and the separator fit, so automatic names are never cut. */
#define ROBOTWEAX_SRT_TELEMETRY_GROUP_PATH_CAPACITY 160u

    /* What the path_name callback sees about a member. */
    typedef struct robotweax_srt_telemetry_group_member_identity_v1
    {
        uint32_t struct_size;
        uint32_t abi_version;
        int32_t group;
        int32_t socket;
        int32_t token;
        uint32_t weight;
        /* "ip:port" or "[ipv6]:port" ("[ipv6%scope]:port" for a scoped
         * address); empty when unknown. */
        char local_address[ROBOTWEAX_SRT_TELEMETRY_GROUP_ADDRESS_CAPACITY];
        char peer_address[ROBOTWEAX_SRT_TELEMETRY_GROUP_ADDRESS_CAPACITY];
    } robotweax_srt_telemetry_group_member_identity_v1;

    /* Names a member's path, e.g. "lte-1" or "path-a". Write a NUL-terminated
     * name of 1..capacity-1 characters from 0x21..0x7E (printable ASCII
     * without space) into name and return non-zero; return 0, or write an
     * invalid name, to use the automatic name (token, else addresses). Called
     * once per member socket, from group_session_sample, when the member is
     * first seen CONNECTED. Must not throw. */
    typedef int (*robotweax_srt_telemetry_public_srt_path_name_fn)(
        void *user_data, const robotweax_srt_telemetry_group_member_identity_v1 *member, char *name,
        size_t capacity);

    typedef struct robotweax_srt_telemetry_public_srt_group_config_v1
    {
        uint32_t struct_size;
        uint32_t abi_version;
        /* Group handle (srt_create_group, or srt_accept with SRTO_GROUPCONNECT). */
        int32_t group;
        robotweax_srt_telemetry_provider provider;
        /* Must include CAP_GROUP_MEMBERS_V1; copied into every raw sample. */
        uint64_t capabilities;
        /* Group handle fields: subsets of GROUP_CANDIDATE_FIELDS and
         * GROUP_EXT_CANDIDATE_FIELDS (srt_bistats on a group fills no more). */
        uint64_t group_qualified_fields;
        uint64_t group_qualified_ext_fields;
        /* Member socket fields, with the same rules as a statistics session
         * (config_v2) for this provider. */
        uint64_t member_qualified_fields;
        uint64_t member_qualified_ext_fields;
        robotweax_srt_telemetry_public_srt_bistats_fn bistats;
        robotweax_srt_telemetry_public_srt_group_data_fn group_data;
        robotweax_srt_telemetry_public_srt_getsockname_fn getsockname;
        robotweax_srt_telemetry_public_srt_getsockstate_fn getsockstate;
        /* GROUP_TYPE_*; decides how ACTIVE_CHANGED is derived. */
        uint32_t group_type;
        /* Must be zero. */
        uint32_t reserved;
        /* Optional (NULL: automatic names). */
        robotweax_srt_telemetry_public_srt_path_name_fn path_name;
        void *path_name_user_data;
    } robotweax_srt_telemetry_public_srt_group_config_v1;

    typedef struct robotweax_srt_telemetry_group_member_v1
    {
        uint32_t struct_size;
        uint32_t abi_version;
        int32_t socket;
        /* SRT_SOCKGROUPCONFIG.token; -1 on the accepting side. */
        int32_t token;
        /* SRT_SOCKSTATUS of the member as listed by the group. */
        int32_t socket_state;
        /* MEMBER_STATE_* (SRT_MEMBERSTATUS). */
        int32_t member_state;
        /* SRT_SOCKGROUPDATA.result: 0, or -1 after the last operation failed. */
        int32_t member_result;
        uint32_t weight;
        /* MEMBER_* flags. */
        uint32_t flags;
        /* PATH_SOURCE_*. */
        uint32_t path_source;
        /* srt_bistats return value of this call. */
        int32_t native_result;
        uint32_t reserved;
        /* Empty until the path is named. */
        char path[ROBOTWEAX_SRT_TELEMETRY_GROUP_PATH_CAPACITY];
        /* As srt_getsockname reports it. It is read again on every call until
         * a read succeeds while the member is CONNECTED (a connecting member
         * is bound to the wildcard), then kept. Robotweax SRT reports the bound wildcard,
         * e.g. 0.0.0.0:port, for a connected caller member too. Empty while
         * unknown. */
        char local_address[ROBOTWEAX_SRT_TELEMETRY_GROUP_ADDRESS_CAPACITY];
        /* Empty until the provider reports a peer address. */
        char peer_address[ROBOTWEAX_SRT_TELEMETRY_GROUP_ADDRESS_CAPACITY];
        robotweax_srt_telemetry_raw_sample_v1 raw;
        robotweax_srt_telemetry_delta_v1 delta;
        robotweax_srt_telemetry_ext_sample_v1 ext;
        robotweax_srt_telemetry_ext_delta_v1 ext_delta;
    } robotweax_srt_telemetry_group_member_v1;

    typedef struct robotweax_srt_telemetry_public_srt_group_result_v1
    {
        uint32_t struct_size;
        uint32_t abi_version;
        /* GROUP_* flags. */
        uint32_t flags;
        /* From the config. */
        uint32_t group_type;
        /* SRT_SOCKSTATUS of the group handle. */
        int32_t group_state;
        /* srt_bistats on the group and srt_group_data return values. */
        int32_t group_native_result;
        int32_t group_data_native_result;
        /* Main/Backup: the member that carried the most stream packets in the
         * latest tick in which any member carried data; -1 before that and
         * for other group types. It keeps naming a member that has since
         * been removed until another member carries. */
        int32_t active_member;
        /* Value of active_member before this call. */
        int32_t previous_active_member;
        /* Distinct members listed by srt_group_data in this call. */
        uint32_t listed_member_count;
        /* Listed members with MEMBER_USABLE (also those that did not fit). */
        uint32_t usable_member_count;
        /* Zero. */
        uint32_t reserved;

        /* Set by the caller: an array of member_capacity entries (may be
         * NULL when member_capacity is 0) and sizeof(group_member_v1). */
        robotweax_srt_telemetry_group_member_v1 *members;
        uint32_t member_capacity;
        uint32_t member_entry_size;
        /* Set by the call: entries filled in members (removed members first,
         * then the listed members in provider order), and the number that
         * would have been filled with enough room. */
        uint32_t member_count;
        uint32_t required_member_count;

        robotweax_srt_telemetry_raw_sample_v1 raw;
        robotweax_srt_telemetry_delta_v1 delta;
        robotweax_srt_telemetry_ext_sample_v1 ext;
        robotweax_srt_telemetry_ext_delta_v1 ext_delta;
    } robotweax_srt_telemetry_public_srt_group_result_v1;

    typedef struct robotweax_srt_telemetry_public_srt_group_session
        robotweax_srt_telemetry_public_srt_group_session;

    /* The host owns the group handle and the injected functions; they must
     * remain valid until the session is destroyed. Calls on one session are
     * externally synchronized. Returns INVALID_ARGUMENT when the handle is not
     * a group handle, a required function is missing, CAP_GROUP_MEMBERS_V1 is
     * absent or a field set exceeds its candidate mask. */
    ROBOTWEAX_SRT_TELEMETRY_PUBLIC_SRT_API robotweax_srt_telemetry_error
    robotweax_srt_telemetry_public_srt_group_session_create(
        const robotweax_srt_telemetry_public_srt_group_config_v1 *config,
        robotweax_srt_telemetry_public_srt_group_session **session);

    ROBOTWEAX_SRT_TELEMETRY_PUBLIC_SRT_API void
    robotweax_srt_telemetry_public_srt_group_session_destroy(
        robotweax_srt_telemetry_public_srt_group_session *session);

    /* Configure before the first valid sample invocation. A non-null getter
     * requires a Robotweax provider with CAP_GROUP_PATH_IDENTIFIERS_V1 qualified
     * at its exact revision/build. Resolve it from the same provider that owns
     * the group handles. NULL disables the optional lookup. Existing v1 layouts
     * and naming remain unchanged unless this function enables it. Returns
     * INVALID_ARGUMENT for a null session, late mutation or missing qualification.
     * Calls are externally synchronized, as with sample/destroy. */
    ROBOTWEAX_SRT_TELEMETRY_PUBLIC_SRT_API robotweax_srt_telemetry_error
    robotweax_srt_telemetry_public_srt_group_session_set_path_data_v1(
        robotweax_srt_telemetry_public_srt_group_session *session,
        robotweax_srt_telemetry_public_srt_group_path_data_v1_fn getter);

    /* One srt_getsockstate and one srt_bistats on the group, srt_group_data
     * (a second or third attempt only when the group outgrew the session's
     * buffer between calls), one srt_bistats per member, and srt_getsockname
     * until a member is CONNECTED. Statistics are read with clear=0 and
     * instantaneous=1. The caller sets struct_size, abi_version, members,
     * member_capacity and member_entry_size; the call clears and fills the
     * rest. Returns ABI_MISMATCH for a wrong member_entry_size,
     * PROVIDER_SAMPLE_FAILURE when srt_group_data fails (the group is closed
     * or bonding is not built in; group_state and the native results are
     * still filled) and BUFFER_TOO_SMALL with GROUP_TRUNCATED when entries
     * did not fit; the result is then valid except for the missing entries.
     * ALLOCATION_FAILURE leaves member_count 0 and loses that tick's deltas;
     * the next call reports what it would have reported (NEW, REMOVED,
     * STATE_CHANGED, PATH_ASSIGNED and the group's change flags) again. A failing srt_bistats on the group or a member does not fail the
     * call (see GROUP_STATS and MEMBER_STATS_STALE). */
    ROBOTWEAX_SRT_TELEMETRY_PUBLIC_SRT_API robotweax_srt_telemetry_error
    robotweax_srt_telemetry_public_srt_group_session_sample(
        robotweax_srt_telemetry_public_srt_group_session *session, uint64_t observed_at_unix_nano,
        uint64_t sample_monotonic_nano, robotweax_srt_telemetry_public_srt_group_result_v1 *result);

#ifdef __cplusplus
}
#endif

#endif
