# Contributing

Robotweax SRT Telemetry is currently contract-first. A code change must not
silently change the meaning of an existing metric, event, capability, or
data-quality state.

## Before changing telemetry semantics

1. Name the operator question or RCA hypothesis that needs the signal.
2. Identify the exact public source field or public Robotweax extension.
3. Record unit, side, temporality, reset behavior, lifecycle, and availability.
4. Define behavior for both Haivision SRT and Robotweax SRT.
5. Add deterministic positive, negative, reset, and missing-data tests.
6. Update the field map and schemas in the same change.

Never infer support from a provider name alone. Capabilities represent
qualified evidence at an exact provider version and build profile.

## Local validation

```sh
cmake -S . -B build -DROBOTWEAX_SRT_TELEMETRY_BUILD_TESTS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Keep builds out of the source tree and preserve compiler warnings. Do not add
media payloads, passphrases, key material, private infrastructure, customer
identifiers, or unredacted Stream IDs to fixtures.

Golden incidents must include a manifest, a digest-pinned sample dataset,
machine expectations, contradictions, and deterministic regeneration. Mark
synthetic fixtures explicitly; never present them as provider qualification.
