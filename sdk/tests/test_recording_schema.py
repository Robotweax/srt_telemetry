"""Validates 1alpha2 recordings the recorder writes against the JSONL line schema."""
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
with tempfile.TemporaryDirectory() as directory:
    subprocess.run([sys.argv[1], '--jsonl', directory], check=True, stdout=subprocess.DEVNULL)
    count = 0
    for name in ('member.jsonl', 'group.jsonl'):
        for line in (Path(directory) / name).read_text().splitlines():
            validator.validate(json.loads(line))
            count += 1
assert count > 10, count
print(f'{count} 1alpha2 recording lines validate against the schema')
