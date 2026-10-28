#!/bin/bash
set -euo pipefail
export PYTHONPATH=/pip
python3 -m pip install --only-binary=:all: --retries 2 --timeout 20 --target /tmp/python opentelemetry-proto==1.45.0 jsonschema==4.26.0 > /evidence/linux-python-install.log 2>&1
export PYTHONPATH=/tmp/python
export ASAN_OPTIONS=detect_leaks=1:halt_on_error=1
export UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
flags='-fsanitize=address,undefined -fno-omit-frame-pointer -g -O1'
cmake -S /telemetry-source -B /tmp/remote-build -DCMAKE_BUILD_TYPE=Debug -DBUILD_SHARED_LIBS=ON -DROBOTWEAX_SRT_TELEMETRY_BUILD_TESTS=ON -DROBOTWEAX_SRT_TELEMETRY_REQUIRE_OTLP_PROTO_TEST=ON -DROBOTWEAX_SRT_TELEMETRY_BUILD_HTTP_SERVER=ON "-DCMAKE_CXX_FLAGS=$flags" "-DCMAKE_C_FLAGS=$flags" "-DCMAKE_EXE_LINKER_FLAGS=$flags" "-DCMAKE_SHARED_LINKER_FLAGS=$flags" -DCMAKE_INSTALL_PREFIX=/tmp/install > /evidence/linux-final-configure.log 2>&1
cmake --build /tmp/remote-build --parallel 5 > /evidence/linux-final-build.log 2>&1
set +e
ctest --test-dir /tmp/remote-build --output-on-failure --parallel 1 > /evidence/linux-final-tests.log 2>&1
candidate=$?
set -e
ctest --test-dir /tmp/remote-build -R remote_draft -V > /evidence/linux-remote-tests.log 2>&1
/tmp/remote-build/experimental/remote_input/robotweax_srt_telemetry_remote_draft_tests --jsonl > /evidence/linux-golden.jsonl
ldd /tmp/remote-build/experimental/remote_input/robotweax_srt_telemetry_remote_draft_tests > /evidence/linux-loader.log
cmake --install /tmp/remote-build > /evidence/linux-install.log 2>&1
cmake -S /telemetry-source/sdk/tests/package_consumer -B /tmp/consumer -DCMAKE_PREFIX_PATH=/tmp/install "-DCMAKE_CXX_FLAGS=$flags" "-DCMAKE_C_FLAGS=$flags" "-DCMAKE_EXE_LINKER_FLAGS=$flags" > /evidence/linux-consumer.log 2>&1
cmake --build /tmp/consumer --parallel 3 >> /evidence/linux-consumer.log 2>&1
/tmp/consumer/robotweax_srt_telemetry_package_consumer >> /evidence/linux-consumer.log 2>&1
/tmp/consumer/robotweax_srt_telemetry_http_package_consumer >> /evidence/linux-consumer.log 2>&1
cmake -S /telemetry-source/build-base-source -B /tmp/baseline-build -DCMAKE_BUILD_TYPE=Debug -DBUILD_SHARED_LIBS=ON -DROBOTWEAX_SRT_TELEMETRY_BUILD_TESTS=ON -DROBOTWEAX_SRT_TELEMETRY_REQUIRE_OTLP_PROTO_TEST=ON "-DCMAKE_CXX_FLAGS=$flags" "-DCMAKE_C_FLAGS=$flags" "-DCMAKE_EXE_LINKER_FLAGS=$flags" "-DCMAKE_SHARED_LINKER_FLAGS=$flags" > /evidence/linux-baseline-build.log 2>&1
cmake --build /tmp/baseline-build --parallel 5 --target robotweax_srt_telemetry_http_tests robotweax_srt_telemetry_export_tests >> /evidence/linux-baseline-build.log 2>&1
set +e
ctest --test-dir /tmp/baseline-build -R '^robotweax_srt_telemetry_(http_tests|export_tests)$' --output-on-failure --parallel 2 > /evidence/linux-baseline-parallel.log 2>&1
baseline=$?
set -e
printf '{"candidate_serial_exit":%s,"baseline_parallel_exit":%s}\n' "$candidate" "$baseline" > /evidence/linux-result.json
python3 /evidence/provenance.py
cmake -S /telemetry-source -B /tmp/default-off -DROBOTWEAX_SRT_TELEMETRY_BUILD_TESTS=OFF -DROBOTWEAX_SRT_TELEMETRY_BUILD_HTTP_SERVER=OFF -DCMAKE_EXPORT_COMPILE_COMMANDS=ON > /evidence/linux-default-off.log 2>&1
python3 - <<'PY'
from pathlib import Path
import json
assert not any('/experimental/' in c['file'] for c in json.loads(Path('/tmp/default-off/compile_commands.json').read_text()))
print('PASS production default-OFF graph and uninstalled experimental API')
PY
cat /evidence/linux-result.json
exit "$candidate"
