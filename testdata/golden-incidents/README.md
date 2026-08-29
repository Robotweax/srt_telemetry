# Golden incidents

This directory contains deterministic incident datasets and their manifests.
Do not commit customer data, private network inventories,
passphrases, key material, unredacted Stream IDs, or media payloads.

Each incident directory must validate against
`schema/golden-incident-manifest.schema.json` and retain exact provider
revisions, direction, fault seed, transport profile, expected evidence, and
relevant contradictions.

Synthetic fixtures must declare that they were not captured from a provider.
They prove contract behavior, not runtime qualification. Captured incidents
must retain exact artifacts and a reproducible fault boundary. Their changing
runtime gauges may prevent byte-for-byte regeneration, so reproduction is
judged against machine expectations and recorded proxy behavior.

Current incidents:

- [Incident 001: constant packet loss](incident-001-constant-loss/README.md)
- [Incident 002: captured Robotweax-to-Haivision constant loss](incident-002-captured-constant-loss-robotweax-haivision/README.md)
- [Incident 003: captured Haivision-to-Robotweax constant loss](incident-003-captured-constant-loss-haivision-robotweax/README.md)
- [Incident 004: captured Robotweax-to-Haivision burst loss](incident-004-captured-burst-loss-robotweax-haivision/README.md)
- [Incident 005: captured Haivision-to-Robotweax burst loss](incident-005-captured-burst-loss-haivision-robotweax/README.md)
- [Incident 006: real Robotweax-to-Haivision JSONL analysis](incident-006-jsonl-constant-loss-robotweax-haivision/README.md)
- [Incident 007: real Haivision-to-Robotweax JSONL analysis](incident-007-jsonl-constant-loss-haivision-robotweax/README.md)
- [Incident 008: Robotweax-to-Haivision RTT step](incident-008-rtt-step-robotweax-haivision/README.md)
- [Incident 009: Haivision-to-Robotweax RTT step](incident-009-rtt-step-haivision-robotweax/README.md)
- [Incident 010: Robotweax sender to paused Haivision receiver](incident-010-receiver-pause-robotweax-haivision/README.md)
- [Incident 011: Haivision sender to paused Robotweax receiver](incident-011-receiver-pause-haivision-robotweax/README.md)
- [Incident 012: Robotweax sender through reduced egress capacity](incident-012-bandwidth-reduction-robotweax-haivision/README.md)
- [Incident 013: Haivision sender through reduced egress capacity](incident-013-bandwidth-reduction-haivision-robotweax/README.md)
