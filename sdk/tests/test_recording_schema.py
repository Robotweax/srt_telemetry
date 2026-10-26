"""Validates the 1alpha2, 1alpha3 and 1alpha4 recordings the recorder writes against the
JSONL line schema, and that the schema refuses malformed 1alpha3 headers."""
import copy
import json
from pathlib import Path
import subprocess
import sys
import tempfile

try:
    import jsonschema
except ImportError:
    print('SKIP: install jsonschema to validate the recording contract')
    sys.exit(77)

schema = json.loads((Path(__file__).parents[2] / 'schema/telemetry-jsonl-line.schema.json').read_text())
jsonschema.Draft202012Validator.check_schema(schema)
validator = jsonschema.Draft202012Validator(schema)
counts = {}
headers = {}
for executable in sys.argv[1:]:
    with tempfile.TemporaryDirectory() as directory:
        subprocess.run([executable, '--jsonl', directory], check=True, stdout=subprocess.DEVNULL)
        for path in sorted(Path(directory).glob('*.jsonl')):
            for line in path.read_text().splitlines():
                obj = json.loads(line)
                validator.validate(obj)
                counts[obj['schemaVersion']] = counts.get(obj['schemaVersion'], 0) + 1
                if obj['kind'] == 'robotweax-srt-telemetry-recording' and obj['schemaVersion'] in ('1alpha3', '1alpha4'):
                    headers[path.stem] = obj
assert counts.get('1alpha2', 0) > 10, counts
assert counts.get('1alpha3', 0) > 10, counts
assert counts.get('1alpha4', 0) >= 4, counts
assert set(headers) == {'plain', 'group', 'member', 'observer', 'unknown'}, headers.keys()

# Synthetic refusals: each must fail validation.
bad = [
    ('plain', 'groupId', 5),            # a plain socket has no group
    ('plain', 'pathLabel', '6c7465'),   # nor a path label
    ('group', 'pathLabel', '6c7465'),   # a group row has no label
    ('member', 'pathLabel', '6C7465'),  # lowercase hex only
    ('member', 'pathLabel', '6c7'),     # whole bytes
    ('member', 'groupId', -1),          # null, not -1
    ('plain', 'sessionRef', ''),        # 1..256 printable
    ('plain', 'sessionRef', 'a\tb'),
    ('plain', 'connectedAtMonotonicNano', None),  # both or neither
    ('observer', 'observerNode', 'null'),       # the word null is not a node
    ('observer', 'observerNode', ''),
    ('observer', 'observerNode', 'edge fra'),
    ('observer', 'observerInstance', 'x' * 65),
]
for name, key, value in bad:
    obj = copy.deepcopy(headers[name])
    obj[key] = value
    if validator.is_valid(obj):
        raise AssertionError(f'{name}: {key}={value!r} should be refused')
for name in headers:
    obj = copy.deepcopy(headers[name])
    del obj['connectedAtUnixNano']
    assert not validator.is_valid(obj), 'connectedAtUnixNano is required in 1alpha3'
for key in ('observerNode', 'observerInstance'):
    obj = copy.deepcopy(headers['observer'])
    del obj[key]
    assert not validator.is_valid(obj), f'{key} is required in 1alpha4'
    obj = copy.deepcopy(headers['plain'])
    obj[key] = 'edge-fra-2'
    assert not validator.is_valid(obj), f'{key} is not a 1alpha3 field'
print(f'{counts} recording lines validate against the schema; {len(bad)} malformed headers refused')
