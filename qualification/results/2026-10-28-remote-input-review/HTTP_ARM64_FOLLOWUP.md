# Issue-ready follow-up: HTTP timing/pagination under local ARM64 sanitizers

Prepared separately from the remote-input design. No GitHub issue or review reply
was posted by this work. Existing raw logs are retained at the immutable
[98cda94 evidence](https://github.com/Robotweax/srt_telemetry/tree/98cda94210696424bde9637abdfadc6a581dcced/qualification/results/2026-10-28-remote-input-draft).

## Observed locally

On Linux ARM64/Docker7.0.14-linuxkit, GCC Debug with
`-fsanitize=address,undefined -fno-omit-frame-pointer -g -O1`, ASan leak detection
and UBSan halt enabled, the unchanged HTTP suite failed its1s ping-flood response
bound. A serial candidate run observed about3.45s; an independent unmodified
main341adc0 archive observed3.63s in a paired HTTP/export run. That baseline also
failed the3,600-record history-pagination assertion. Full logs and exact source
hashes remain in the archive; no deadline or server code was changed.

The remote static library is not linked into Core/HTTP. Thus this is not evidence
that the remote draft changed HTTP, and it does not establish a universal server
regression or root cause.

## Independent maintainer result

The [review](https://github.com/Robotweax/srt_telemetry/pull/65#pullrequestreview-5685135114)
reports two passing unmodified-main Linux x86-64 sanitizer runs, including both
ping flood and history pagination. Those runs were performed by the reviewer,
not by this task. The difference suggests an environment-specific timing issue;
sanitizer slowdown is a hypothesis, not a demonstrated cause.

## Suggested investigation

- Reproduce the ping-flood and pagination assertions separately on ARM64 with
  exact toolchain, CPU allocation/load, sanitizer settings and request timings.
- Record the pagination response/status/body/continuation failure independently;
  it may require a cause beyond the ping-flood timing bound.
- Compare native ARM64, Docker ARM64 and x86-64 without weakening existing limits.
- Retain failure and passing evidence separately. Do not convert a skipped test
  or another machine's passing run into a green local sanitizer qualification.
