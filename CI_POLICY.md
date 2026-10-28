# Private CI policy

Implements issue #63. Public Robotweax/srt is unchanged.

## Automatic coverage

One default compiler, unit/schema/recording and installed-consumer smoke, retained evidence, dashboard and operator contract validators. Prose-only changes skip compilation.

7 automatic jobs → 1 Linux core/contracts job; weekly full cron removed. These are selected runner-job counts, not measured billed minutes or currency savings.

Only ordinary root/docs prose is exempt from native work. Security policy, build inputs, dependency pins, unknown paths and missing comparison history select validation conservatively. Workflows always start; selected checks fail normally, including on timeout.

## Explicit qualification

```sh
gh workflow run ci.yml --ref <candidate> -f robotweax_srt_ref=<provider-ref>
```

Use a candidate branch/tag and record the run's immutable head SHA, suite and run URL. The automatic summary records GITHUB_SHA. A successful fast check is limited smoke coverage: skipped/unattempted suites are not passed qualification.

Full alternate compiler/macOS and all three provider tracks retain their original tests and caches, including fault-lab and observability E2E. Provider revisions are resolved by the original jobs. Automatic upstream drift and complete observability qualification are deferred.

Superseded PR runs cancel only within the same PR. Main/tag/manual runs have independent identities; no repository protection, credential, permission or release-approval settings are changed. Existing required-check consumers were inspected through branch protection and effective rules before this change.
