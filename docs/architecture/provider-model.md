# Provider and capability model

## Bounded implementation neutrality

The project supports two implementation families:

1. qualified Haivision SRT versions; and
2. qualified Robotweax SRT versions.

Both are observed through a selected public SRT API profile. The initial target
is `srt-1.5.5`. A compatible header or ABI layout alone does not prove that a
field has identical runtime meaning, availability, or evidence quality.

The qualified Haivision SRT version is v1.5.7 (`899348d8`) since 4 October
2026, the same reference the Network Lab uses for interoperability; it exposes
the `srt-1.5.5` statistics surface unchanged, and the field-coverage,
connection-event, embedding and fault-lab tests pass on it. Evidence recorded
with v1.5.5 before that date keeps its version.

Each adapter declaration records:

```yaml
provider: haivision-srt | robotweax-srt
provider_version: string
public_api_profile: srt-1.5.5
adapter_version: string
build_profile: string
capabilities: []
```

## Capability classes

### Common baseline

`stats.common.v1` contains only observations qualified for both target
implementations. A common name does not erase source semantics: sender loss and
receiver loss, for example, remain separate.

### Profile capabilities

Capabilities such as `stats.fec.v1` and `groups.members.v1` declare optional
surfaces. A field may only be emitted when the exact adapter/provider profile
has qualified it.

### Robotweax extensions

Robotweax SRT may expose evidence that is unavailable through the compatible
Haivision surface. Extensions must:

- use an installed, documented, versioned public C API;
- remain outside the frozen compatible `SRT_TRACEBSTATS` layout;
- have bounded sampling cost and no packet-hot-path allocation;
- define unavailable behavior for Haivision SRT;
- carry a distinct `robotweax.*` capability and semantic namespace; and
- update the Robotweax public header, manifest, symbol baseline, tests, and
  documentation as one change set.

`robotweax.groups.path-identifiers.v1` qualifies the optional 44-byte
`robotweax_srt_group_path_data_v1` contract at an exact development revision/build.
It requires explicit injection from the library that owns the handles; neither
provider text nor header presence enables it. Published Robotweax v0.2.8 and
Haivision v1.5.7 leave it unavailable. See [group path names](public-srt-group.md#path-names).

Private Robotweax C++ implementation objects are not an integration boundary.

## Adapter selection

The host explicitly selects or registers the adapter. `srt_getversion()` is
not a provider detector because Robotweax deliberately reports the compatible
SRT API version.

The SDK must not dynamically load or link a second SRT implementation into a
process. The public adapter calls the host-selected implementation; a
Robotweax-specific adapter may add separately qualified extension calls.

## Missing data

Missing data is represented by absence from the field-availability set. It is
never represented by numeric zero. RCA, alerts, and dashboards must propagate
that distinction.
