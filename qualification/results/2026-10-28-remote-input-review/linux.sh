#!/bin/bash
set -euo pipefail
export PYTHONPATH=/tmp/python
export ASAN_OPTIONS=detect_leaks=1:halt_on_error=1
export UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
cmake --build /tmp/remote-build --target robotweax_srt_telemetry_remote_draft_tests --parallel 4 > /review/linux-build.log 2>&1
ctest --test-dir /tmp/remote-build -R remote_draft -V > /review/linux-remote-tests.log 2>&1
/tmp/remote-build/experimental/remote_input/robotweax_srt_telemetry_remote_draft_tests --jsonl > /review/linux-golden.jsonl
ldd /tmp/remote-build/experimental/remote_input/robotweax_srt_telemetry_remote_draft_tests > /review/linux-loader.log
python3 /src/project/qualification/telemetry_remote/test_origin.py --neutral-probe /tmp/remote-build/experimental/remote_input/robotweax_srt_telemetry_remote_draft_tests -v > /review/bl-origin-tests.log 2>&1
python3 - <<'PY'
from pathlib import Path
import hashlib,json,subprocess
root=Path('/tmp/remote-build/experimental/remote_input')
proof={'asan_options':'detect_leaks=1:halt_on_error=1','ubsan_options':'halt_on_error=1:print_stacktrace=1','platform':subprocess.check_output(['uname','-srm'],text=True).strip(),'inputs':{}}
for name in ['robotweax_srt_telemetry_remote_draft_tests','librobotweax_srt_telemetry_remote_draft.a']:
 p=root/name
 assert '__asan_' in subprocess.check_output(['nm',str(p)],text=True,stderr=subprocess.DEVNULL)
 proof['inputs'][name]={'sha256':hashlib.sha256(p.read_bytes()).hexdigest(),'asan_instrumented':True}
Path('/review/linux-provenance.json').write_text(json.dumps(proof,indent=2)+'\n')
PY
cat /review/linux-remote-tests.log
cat /review/bl-origin-tests.log
