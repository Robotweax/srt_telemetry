# Security

Telemetry is metadata, but it can still expose network topology, customer
identity, Stream IDs, hostnames, and operational behavior.

The project must not collect media payload, passphrases, derived keys, key
material, or secret Stream ID content by default. Every new attribute requires
a classification and redaction decision.

Please report suspected vulnerabilities through the repository's private
GitHub security-reporting channel. Do not place vulnerability details, secrets,
packet captures, or customer data in a public issue.
