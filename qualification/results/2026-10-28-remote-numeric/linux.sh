#!/bin/bash
set -euo pipefail
export PYTHONPATH=/tmp/python
export ASAN_OPTIONS=detect_leaks=1:halt_on_error=1
export UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
cmake -S /telemetry-source -B /tmp/remote-build -DBUILD_SHARED_LIBS=ON -DROBOTWEAX_SRT_TELEMETRY_BUILD_TESTS=ON -DROBOTWEAX_SRT_TELEMETRY_BUILD_HTTP_SERVER=ON -DROBOTWEAX_SRT_TELEMETRY_REQUIRE_OTLP_PROTO_TEST=ON -DPython3_EXECUTABLE=/usr/bin/python3 -DCMAKE_BUILD_TYPE=Debug '-DCMAKE_CXX_FLAGS=-fsanitize=address,undefined -fno-omit-frame-pointer -g -O1' '-DCMAKE_C_FLAGS=-fsanitize=address,undefined -fno-omit-frame-pointer -g -O1' > /numeric-evidence/linux-configure.log 2>&1
cmake --build /tmp/remote-build --parallel 4 > /numeric-evidence/linux-build.log 2>&1
ctest --test-dir /tmp/remote-build -E http --output-on-failure --parallel 4 > /numeric-evidence/linux-core-tests.log 2>&1
ctest --test-dir /tmp/remote-build -R 'remote_numeric|counter_trace|remote_draft|delta' -V > /numeric-evidence/linux-focused.log 2>&1
ldd /tmp/remote-build/experimental/remote_input/robotweax_srt_telemetry_counter_trace > /numeric-evidence/linux-loader.log
python3 - <<'PY'
from pathlib import Path
import hashlib,json,subprocess
root=Path('/tmp/remote-build')
proof={'asan_options':'detect_leaks=1:halt_on_error=1','ubsan_options':'halt_on_error=1:print_stacktrace=1','platform':subprocess.check_output(['uname','-srm'],text=True).strip(),'inputs':{}}
for f in ['experimental/remote_input/robotweax_srt_telemetry_remote_numeric_tests','experimental/remote_input/librobotweax_srt_telemetry_remote_draft.a','experimental/remote_input/robotweax_srt_telemetry_counter_trace','librobotweax_srt_telemetry.so']:
 p=root/f
 assert '__asan_' in subprocess.check_output(['nm',str(p)],text=True,stderr=subprocess.DEVNULL)
 proof['inputs'][f]={'sha256':hashlib.sha256(p.read_bytes()).hexdigest(),'asan_instrumented':True}
Path('/numeric-evidence/linux-provenance.json').write_text(json.dumps(proof,indent=2)+'\n')
PY
tail -n 5 /numeric-evidence/linux-core-tests.log
tail -n 5 /numeric-evidence/linux-focused.log
