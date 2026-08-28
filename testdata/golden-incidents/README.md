# Golden incidents

This directory will contain deterministic incident datasets and their
manifests. Do not commit customer data, private network inventories,
passphrases, key material, unredacted Stream IDs, or media payloads.

Each incident directory must validate against
`schema/golden-incident-manifest.schema.json` and retain exact provider
revisions, direction, fault seed, transport profile, expected evidence, and
relevant contradictions.
