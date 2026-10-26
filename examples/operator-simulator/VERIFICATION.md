# Verification — 26 October 2026

## Passed locally on the authorized MacBook

- Node 26.8.2 / npm 11.19.1.
- `npm run typecheck`, `npm run lint`, `npm test` (13 tests in two files), `npm run build`.
- Automated reducer contracts: preview isolation, duplicate Take, command phases, rejection recovery, navigation semantics, connected/no-media, stale/preview isolation, old session events, reset epochs/empty event queue, unknown response reconciliation and unmatched API failure.
- Automated React interactions: normal Take progression, rejection/recovery, stale stream metrics, navigation with a submitted command and reset.
- Actual MacBook in-app browser walkthrough: normal requested/accepted/applied/observed, applied with unknown downstream, rejected Take and recovery, staged reconnect, preview failure with media arriving, stale stream detail, unknown outcome and reconciliation, cancel arming via keyboard, visible focus ring. No use of a screenshot as a substitute for interaction.
- Visual inspection at default 1280×720, explicit compact desktop 1280×900 and narrow 720×900. Narrow document width equals viewport width (720 px); no horizontal overflow. Viewport override reset afterward.

## Screenshots

All screenshots contain synthetic data; full-page captures from the actual in-app browser. Not real video or transport evidence.

- [Operations desktop](evidence/operations-desktop.jpg)
- [Armed switching and keyboard focus](evidence/switching-armed-desktop.jpg)
- [Rejected switching desktop](evidence/rejected-desktop.jpg)
- [Rejected switching narrow](evidence/rejected-narrow.jpg)

## Limits and not run

No real endpoints, SRT streams, media decode, downstream receiver or performance tests. No full screen-reader/WCAG audit, cross-browser matrix or second-language QA. Automated UI tests use jsdom; browser walkthroughs above are separate manual tool-driven checks. Full app reload starts a new in-memory simulation. CI status is recorded in the draft PR/final handoff because it is not available before the commit is submitted.

The existing telemetry core/provider CI is unchanged. A path-scoped simulator workflow runs the four local checks independently, without private provider credentials or new infrastructure. No authentication changes or CI repairs outside this slice.
