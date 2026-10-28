"""Independent draft export checks. No socket, provider or remote authorization."""
import copy
import json
from pathlib import Path
import subprocess
import sys
try:
    import jsonschema
except ImportError:
    print('SKIP: remote draft schema unqualified; install jsonschema')
    sys.exit(77)

base = Path(__file__).resolve().parent
schema = json.loads((base/'remote-input-line.schema.json').read_text())
jsonschema.Draft202012Validator.check_schema(schema)
validator = jsonschema.Draft202012Validator(schema)
lines = subprocess.check_output([sys.argv[1], '--jsonl'], text=True).splitlines()
assert len(lines) == 3
rows = [json.loads(line) for line in lines]
for row in rows:
    validator.validate(row)
header, missing, epoch_zero = rows
assert header['originKind'] == 'remote_media_endpoint'
assert header['maximumAgeNano'] == '100'
maximum = (1 << 64)-1
assert header['observer'] == {
    'node':'cloud-fixture','instance':'cloud-process-1','clockDomain':'cloud-monotonic-1',
}
assert header['producer'] == {
    'id':'producer-fixture','sourceId':'source/"back\\slash','role':'sender',
    'instance':None,'clockDomain':None,
}
assert header['origin'] == {
    'namespace':'fixture-origin','schema':'fixture/9.7','contextEncoding':'hex','context':'00ff225c',
}
# Origin versions are opaque identifiers, not decoded Telemetry/BL schema minors.
foreign = copy.deepcopy(header)
foreign['origin'] = {'namespace':'another-origin','schema':'not-understood/999.123',
                     'contextEncoding':'hex','context':'ff0000'}
validator.validate(foreign)
for row, sequence, mono, receipt, age in [
        (missing, maximum-1, maximum-2, 50, 20),
        (epoch_zero, maximum, maximum-1, 51, 19)]:
    assert row['segmentId'] == header['segmentId'] == 'cloud-segment-1'
    assert row['sequence'] == str(sequence)
    assert row['producerMonotonicNano'] == str(mono)
    assert row['firstReceipt']['monotonicNano'] == str(receipt)
    assert row['receiptAgeNano'] == str(age)
    assert row['exportedMonotonicNano'] == '70'
    assert row['historical'] is True and row['freshness'] == 'fresh'
    assert row['originPayload'] == {'encoding':'hex','bytes':'8300ff00'}
assert missing['producerWallTime'] is None
assert missing['firstReceipt']['wallTime'] is None
assert epoch_zero['producerWallTime'] == {
    'unixNano':'0','synchronization':'unsynchronized','maximumErrorNano':None,
    'provenance':'producer-clock-fixture',
}
assert epoch_zero['firstReceipt']['wallTime'] == {
    'unixNano':str(maximum),'synchronization':'synchronized','maximumErrorNano':'5',
    'provenance':'cloud-ptp-fixture',
}
assert missing['firstInSegment'] is True and epoch_zero['firstInSegment'] is False
assert not epoch_zero['sequenceDiscontinuity'] and not epoch_zero['equalProducerTime']

bad_rows = []
for value in [maximum, str(maximum+1), '-1', '01', '+1', '1.0', '0', '1\n']:
    bad_rows.append(dict(missing, sequence=value))
for key, value in [('schemaVersion','remote-input.draft1'),('historical',False),
                   ('producerWallTime',0),('inventedClock',None)]:
    bad_rows.append(dict(missing, **{key:value}))
bad = copy.deepcopy(epoch_zero)
bad['producerWallTime']['maximumErrorNano'] = '5'
bad_rows.append(bad)
bad = copy.deepcopy(epoch_zero)
bad['firstReceipt']['wallTime']['maximumErrorNano'] = None
bad_rows.append(bad)
for value in ['', '0', 'gg', '00' * 16385, '00\n']:
    bad = copy.deepcopy(missing)
    bad['originPayload']['bytes'] = value
    bad_rows.append(bad)
bad = copy.deepcopy(missing)
bad['originPayload']['type'] = 5
bad_rows.append(bad)
for value in ['null', 'a/b', '', 'a'*65, 'node\n']:
    bad = copy.deepcopy(header)
    bad['observer']['instance'] = value
    bad_rows.append(bad)
bad = copy.deepcopy(header)
bad['producer']['role'] = 'inferred-caller'
bad_rows.append(bad)
bad = copy.deepcopy(missing)
del bad['producerWallTime']
bad_rows.append(bad)
for context in ['0','gg','00'*4097,'00\n']:
    bad = copy.deepcopy(header); bad['origin']['context'] = context; bad_rows.append(bad)
for key in ['row','pair','sampling','nativeKind']:
    bad = copy.deepcopy(header); bad[key] = {}; bad_rows.append(bad)
for i, row in enumerate(bad_rows):
    assert list(validator.iter_errors(row)), i

integers = jsonschema.Draft202012Validator(schema['$defs']['u64'])
for digits in range(1, 23):
    for n in [10**digits-1, 10**digits, 10**digits+1]:
        assert integers.is_valid(str(n)) == (0 <= n <= maximum), n
for n in [0, maximum-1, maximum, maximum+1]:
    assert integers.is_valid(str(n)) == (0 <= n <= maximum), n

old = jsonschema.Draft202012Validator(json.loads(
    (base.parents[1]/'schema/telemetry-jsonl-line.schema.json').read_text()))
for row in rows:
    assert list(old.iter_errors(row)), 'draft must not masquerade as 1alpha1-4'
print(f'PASS3 closed draft lines, neutral context/u64s/opaque origin, null versus epoch0, '
      f'{len(bad_rows)} negatives; old recording schema rejects draft')
