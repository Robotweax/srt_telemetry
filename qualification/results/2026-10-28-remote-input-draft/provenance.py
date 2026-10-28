from pathlib import Path
import hashlib,json,subprocess
exe=Path('/tmp/remote-build/experimental/remote_input/robotweax_srt_telemetry_remote_draft_tests')
lib=Path('/tmp/remote-build/experimental/remote_input/librobotweax_srt_telemetry_remote_draft.a')
proof={'platform':subprocess.check_output(['uname','-srm'],text=True).strip(),'asan':'detect_leaks=1:halt_on_error=1','ubsan':'halt_on_error=1:print_stacktrace=1','inputs':{}}
for p in [exe,lib]:
 assert '__asan_' in subprocess.check_output(['nm',str(p)],text=True,stderr=subprocess.DEVNULL)
 proof['inputs'][p.name]={'sha256':hashlib.sha256(p.read_bytes()).hexdigest(),'asan_instrumented':True}
for p in Path('/tmp/install').rglob('*'):
 assert not 'remote_draft' in p.name and p.name!='remote_input_draft.hpp',p
 if p.name=='RobotweaxSRTTelemetryTargets.cmake': assert 'remote_draft' not in p.read_text()
def symbols(p):
 return sorted(line.split()[-1] for line in subprocess.check_output(['nm','-D','--defined-only',str(p)],text=True).splitlines() if line.split()[-1].startswith('robotweax_srt_telemetry_'))
a=symbols(Path('/tmp/remote-build/librobotweax_srt_telemetry.so'))
b=symbols(Path('/tmp/baseline-build/librobotweax_srt_telemetry.so'))
assert a==b
proof['unchanged_core_c_symbols']=len(a)
Path('/evidence/linux-provenance.json').write_text(json.dumps(proof,indent=2)+'\n')
