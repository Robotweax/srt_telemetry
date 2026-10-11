"""Validate exported and synthetic Standby RTT contract states, not a provider."""
import copy
import json
from pathlib import Path
import subprocess
import sys

try:
    import jsonschema
except ImportError:
    print('SKIP: install jsonschema to validate the optional snapshot contract')
    sys.exit(77)

schema = json.loads((Path(__file__).parents[2] / 'schema/snapshot-standby-rtt-v1.schema.json').read_text())
jsonschema.Draft202012Validator.check_schema(schema)
validator = jsonschema.Draft202012Validator(schema)
snapshot = json.loads(subprocess.check_output([sys.argv[1], '--snapshot'], text=True))
measured = snapshot['sources'][0]['standby_rtt']
validator.validate(measured)
for availability in ('read_failed', 'unavailable'):
    validator.validate({'version': 1, 'availability': availability})
    bad = {'version': 1, 'availability': availability, 'samples': '5'}
    assert not validator.is_valid(bad), 'failed/missing read cannot export old fields'
for outcome in ('pending', 'measured', 'timeout', 'send_skipped', 'paused', 'id_exhausted'):
    candidate = dict(measured, last_outcome=outcome)
    validator.validate(candidate)
absent_age = dict(measured, age_seconds=None, fresh=False, fresh_at_capture=False)
validator.validate(absent_age)
for key, value in (('samples', 5), ('samples', '0'), ('rtt_seconds', 0), ('admitted', False),
                   ('last_outcome', 'healthy'), ('fresh_at_capture', True)):
    bad = copy.deepcopy(measured)
    bad[key] = value
    assert not validator.is_valid(bad), f'inconsistent {key} accepted'
print('Standby RTT exported and synthetic schema cases passed')
