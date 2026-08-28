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
must retain exact artifacts and a reproducible fault boundary.

Current incidents:

- [Incident 001: constant packet loss](incident-001-constant-loss/README.md)
