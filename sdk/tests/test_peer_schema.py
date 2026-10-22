"""Validate the exported optional peer metadata against its schema, plus
synthetic cases the schema must refuse."""
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

schema = json.loads((Path(__file__).parents[2] / 'schema/snapshot-peer-v1.schema.json').read_text())
jsonschema.Draft202012Validator.check_schema(schema)
validator = jsonschema.Draft202012Validator(schema)
snapshot = json.loads(subprocess.check_output([sys.argv[1], '--snapshot'], text=True))
peers = [source['peer'] for source in snapshot['sources']]
states = set()
for peer in peers:
    validator.validate(peer)
    states.add((peer['identity']['availability'], peer['handshake']['availability']))
assert states == {('available', 'available'), ('available', 'unavailable'), ('unknown', 'unavailable'),
                  ('read_failed', 'unavailable'), ('unavailable', 'unavailable')}, states
full = next(p for p in peers if p['handshake']['availability'] == 'available')
cases = [
    ('identity', 'name', 'Robotweax SRT '),      # trailing space
    ('identity', 'name', ' Robotweax'),          # leading space
    ('identity', 'name', 'x' * 33),              # too long
    ('identity', 'release', '0.3 0'),            # space in a release
    ('identity', 'availability', 'maybe'),
    ('handshake', 'flags_raw', '0xBF'),
    ('handshake', 'srt_version', '1.5'),
    ('handshake', 'srt_version_raw', '1.5.7'),
    ('handshake', 'receiver_delay_seconds', -1),
]
for part, key, value in cases:
    bad = copy.deepcopy(full)
    bad[part][key] = value
    assert not validator.is_valid(bad), f'{part}.{key}={value!r} accepted'
ill_formed = copy.deepcopy(full)
ill_formed['handshake']['srt_version'] = None
validator.validate(ill_formed)
missing = copy.deepcopy(full)
del missing['identity']['release']
assert not validator.is_valid(missing), 'available identity without release key accepted'
extra = {'version': 1, 'identity': {'availability': 'unknown', 'name': 'x'},
         'handshake': {'availability': 'unavailable'}}
assert not validator.is_valid(extra), 'name on an unknown identity accepted'
extra = {'version': 1, 'identity': {'availability': 'unavailable'},
         'handshake': {'availability': 'unavailable', 'srt_version': '1.5.7'}}
assert not validator.is_valid(extra), 'handshake values without availability accepted'
print('peer metadata exported and synthetic schema cases passed')
