#!/usr/bin/env python3
"""Offline case packaging for the existing recorder and incident analyzer (stdlib only)."""
import argparse
import copy
import hashlib
import hmac
import io
import json
import math
import os
from pathlib import Path
import re
import sys
import tempfile
import uuid
import zipfile
import zlib

import analyze_incident as analyzer

ROOT = Path(__file__).resolve().parents[1]
MAX_BYTES = 16 * 1024 * 1024
MAX_FILE_BYTES = 8 * 1024 * 1024
MAX_MANIFEST_BYTES = 128 * 1024
MAX_RECORDINGS = 8
MAX_SAMPLES = 2048
FORMAT = 'robotweax-flight-recorder-case'
VERSION = '1.0'
EVIDENCE_CLASSES = ('synthetic-fixture', 'retained-capture', 'new-experiment')
RAW_FIELDS = tuple('sent_packet_total received_packet_total unique_sent_packet_total unique_received_packet_total sender_loss_packet_total receiver_loss_packet_total retransmitted_packet_total sender_drop_packet_total receiver_drop_packet_total receiver_undecryptable_packet_total sent_wire_byte_total received_wire_byte_total unique_sent_wire_byte_total unique_received_wire_byte_total retransmitted_wire_byte_total rtt_seconds flight_size_packets flow_window_packets congestion_window_packets available_send_buffer_bytes available_receive_buffer_bytes send_buffer_bytes send_buffer_seconds receive_buffer_bytes receive_buffer_seconds sender_tsbpd_delay_seconds receiver_tsbpd_delay_seconds'.split())
DELTA_FIELDS = tuple(x.replace('_total', '_delta') for x in RAW_FIELDS[:15])
RATE_FIELDS = tuple('sent_wire_bits_per_second received_wire_bits_per_second unique_sent_wire_bits_per_second unique_received_wire_bits_per_second'.split())
SCHEMA = json.loads((ROOT / 'schema/telemetry-jsonl-line.schema.json').read_text())
CASE_SCHEMA = json.loads((ROOT / 'schema/flight-recorder-case.schema.json').read_text())


class CaseError(ValueError):
    pass


def require(condition, message):
    if not condition:
        raise CaseError(message)


def json_bytes(value):
    return (json.dumps(value, ensure_ascii=False, allow_nan=False, sort_keys=True, separators=(',', ':')) + '\n').encode('utf-8')


def strict_json(data):
    def pairs(items):
        result = {}
        for key, value in items:
            require(key not in result, 'duplicate JSON key')
            result[key] = value
        return result
    try:
        return json.loads(data, object_pairs_hook=pairs,
                          parse_constant=lambda _: (_ for _ in ()).throw(CaseError('non-finite JSON number')))
    except (ValueError, UnicodeError, RecursionError) as error:
        raise CaseError('invalid JSON: ' + str(error)) from error


def digest(data):
    return hashlib.sha256(data).hexdigest()


def validate_contract(value, rule):
    """Small validator for the explicit keywords used in the shipped case schema."""
    kinds = rule.get('type', [])
    kinds = [kinds] if isinstance(kinds, str) else kinds
    actual = ('null' if value is None else 'boolean' if type(value) is bool else
              'integer' if type(value) is int else 'number' if type(value) is float else
              'string' if isinstance(value, str) else 'array' if isinstance(value, list) else
              'object' if isinstance(value, dict) else 'invalid')
    require(not kinds or actual in kinds or actual == 'integer' and 'number' in kinds, 'invalid case field type')
    if 'const' in rule:
        require(type(value) is type(rule['const']) and value == rule['const'], 'invalid case constant')
    if 'enum' in rule:
        require(value in rule['enum'], 'invalid case enum')
    if actual == 'object':
        require(set(rule.get('required', [])) <= set(value), 'missing case fields')
        require(set(value) <= set(rule['properties']), 'unknown case fields')
        for key, item in value.items():
            validate_contract(item, rule['properties'][key])
    elif actual == 'array':
        require(rule.get('minItems', 0) <= len(value) <= rule.get('maxItems', 32768), 'case item budget exceeded')
        for item in value:
            validate_contract(item, rule['items'])
    elif actual == 'string':
        require(rule.get('minLength', 0) <= len(value) <= rule.get('maxLength', 1024), 'case string budget exceeded')
        if 'pattern' in rule:
            require(re.fullmatch(rule['pattern'], value) is not None, 'invalid case identifier')
    elif actual in ('integer', 'number'):
        require((type(value) is int or math.isfinite(value)) and rule.get('minimum', 0) <= value <= rule.get('maximum', 2**64-1), 'invalid case number')


def bounded_read(path, limit=MAX_FILE_BYTES):
    with Path(path).open('rb') as source:
        data = source.read(limit + 1)
    require(len(data) <= limit, 'input exceeds byte budget')
    return data


def validate_object(value, definition):
    """Validate the flat existing telemetry schema, without changing its meanings."""
    require(isinstance(value, dict), 'telemetry object required')
    require(set(definition['required']) <= set(value), 'missing telemetry fields')
    require(set(value) <= set(definition['properties']), 'unknown telemetry fields')
    for key, item in value.items():
        rule = definition['properties'][key]
        if '$ref' in rule:
            rule = SCHEMA['$defs'][rule['$ref'].rsplit('/', 1)[-1]]
        if 'properties' in rule:
            validate_object(item, rule)
            continue
        types = rule.get('type', [])
        types = [types] if isinstance(types, str) else types
        if item is None:
            require('null' in types, 'unexpected null: ' + key)
            continue
        if types:
            okay = (('integer' in types and type(item) is int) or
                    ('number' in types and type(item) in (int, float)) or
                    ('string' in types and isinstance(item, str)))
            require(okay, 'invalid field type: ' + key)
        if type(item) in (int, float):
            require((type(item) is int or math.isfinite(item)) and item >= rule.get('minimum', 0), 'invalid metric: ' + key)
            require(item <= rule.get('maximum', 2**64-1), 'metric outside range: ' + key)
        if isinstance(item, str):
            require(rule.get('minLength', 0) <= len(item) <= rule.get('maxLength', 1024), 'invalid string length')
        if 'enum' in rule:
            require(item in rule['enum'], 'invalid enum: ' + key)
        if 'const' in rule:
            require(item == rule['const'], 'unsupported contract: ' + key)


def parse_recording(data):
    require(len(data) <= MAX_FILE_BYTES, 'recording exceeds byte budget')
    lines = data.splitlines(keepends=True)
    require(1 <= len(lines) <= MAX_SAMPLES + 1, 'recording exceeds sample budget')
    documents = []
    for index, line in enumerate(lines):
        require(line.endswith(b'\n') and len(line) <= 32768, 'invalid or oversized JSONL line')
        document = strict_json(line)
        validate_object(document, SCHEMA['$defs']['recording' if index == 0 else 'sample'])
        documents.append(document)
    # Reuse the existing importer for stream/role/index/count/provider semantics.
    with tempfile.TemporaryDirectory(prefix='flight-validate-') as directory:
        path = Path(directory) / 'recording.jsonl'
        path.write_bytes(data)
        try:
            header, samples = analyzer.load_jsonl_file(path)
        except RuntimeError as error:
            raise CaseError(str(error)) from error
    for sample in samples:
        for section, mask, fields in [('raw', 'available_fields', RAW_FIELDS),
                                       ('delta', 'available_delta_fields', DELTA_FIELDS),
                                       ('delta', 'available_rates', RATE_FIELDS)]:
            values = sample[section]
            require(values[mask] < 1 << len(fields), 'unknown availability bits')
            for bit, field in enumerate(fields):
                require(bool(values[mask] & (1 << bit)) == (values[field] is not None),
                        'availability/value mismatch: ' + field)
        require(sample['delta']['quality_flags'] < 16, 'unknown quality flags')
    return {'header': header, 'samples': samples}


def recording_bytes(recording):
    return b''.join(json_bytes(x) for x in [recording['header']] + recording['samples'])


def atomic_write(path, writer):
    """Publish a complete file without overwriting. Failed writes leave no final case."""
    path = Path(path)
    require(not path.exists(), 'output already exists')
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, name = tempfile.mkstemp(prefix='.flight-', suffix='.tmp', dir=path.parent)
    try:
        os.fchmod(fd, 0o600)
        with os.fdopen(fd, 'wb') as stream:
            writer(stream)
            stream.flush()
            os.fsync(stream.fileno())
        require(Path(name).stat().st_size <= MAX_BYTES, 'output exceeds byte budget')
        os.link(name, path)  # Atomic no-replace publication on the qualified local filesystem.
    finally:
        Path(name).unlink(missing_ok=True)


def safe_version(value):
    return value if re.fullmatch(r'v?\d+\.\d+\.\d+(?:-\d+-g[0-9a-f]{7,40})?', value) else 'redacted'


def pseudonymizer():
    key = os.urandom(32)
    return lambda value: 'id-' + hmac.new(key, str(value).encode(), hashlib.sha256).hexdigest()[:20]


def make_case(paths, evidence_class, aligned=False, trigger_index=None, source_manifest=None,
              capture_metadata=None, private=False):
    require(evidence_class in EVIDENCE_CLASSES, 'unknown evidence class')
    require(1 <= len(paths) <= MAX_RECORDINGS, 'one to eight recordings required')
    source_data = [bounded_read(path) for path in paths]
    require(sum(map(len, source_data)) <= MAX_BYTES - MAX_MANIFEST_BYTES, 'case exceeds byte budget')
    recordings = {f'recordings/endpoint-{i+1}.jsonl': parse_recording(data) for i, data in enumerate(source_data)}
    case_id = str(uuid.uuid4())
    endpoints, sessions = [], {}
    for i, (name, rec) in enumerate(recordings.items()):
        h = rec['header']
        session_id = h['flowId']
        session = sessions.setdefault(session_id, {'sessionId': session_id, 'endpoints': []})
        endpoint = {'endpointId': h['streamId'], 'role': h['role'], 'connectionEpoch': 1,
                    'recording': name, 'clock': {'kind': 'orchestrator-logical' if aligned else 'endpoint-local',
                    'domainId': session_id if aligned else h['streamId'], 'mapping': 'paired-sample-index' if aligned else 'none',
                    'uncertaintyNano': None},
                    'configuration': {'profile': 'undeclared', 'receiverTsbpdDelaySeconds': next((s['raw']['receiver_tsbpd_delay_seconds'] for s in rec['samples'] if s['raw']['receiver_tsbpd_delay_seconds'] is not None), None)},
                    'buildProfileSha256': digest(h['buildProfile'].encode())}
        session['endpoints'].append(endpoint)
        endpoints.append(endpoint)
    sources = [{'kind': 'original-recording', 'sha256': digest(data)} for data in source_data]
    if source_manifest:
        source = bounded_read(source_manifest, MAX_MANIFEST_BYTES)
        sources.append({'kind': 'source-manifest', 'sha256': digest(source)})
        origin = strict_json(source)
        # Historical retained artifacts have real counters but logical, fabricated timestamps.
        if aligned:
            require(origin.get('capture', {}).get('environment', {}).get('timingSource') == 'orchestrator-logical-one-second', 'source does not establish logical pairing')
            require(all(rec['header']['flowId'] == origin.get('incidentId') for rec in recordings.values()), 'manifest does not identify these recordings')
            for data, rec in zip(source_data, recordings.values()):
                require(origin.get('recordings', {}).get(rec['header']['role'], {}).get('sha256') == digest(data), 'source recording digest mismatch')
    capture = None
    if capture_metadata:
        require(not aligned, 'native metadata uses coordinator brackets, not logical time')
        capture_data = bounded_read(capture_metadata, MAX_MANIFEST_BYTES)
        sources.append({'kind': 'capture-metadata', 'sha256': digest(capture_data)})
        capture = strict_json(capture_data)
    require(not aligned or source_manifest, 'paired logical alignment requires a digest-pinned source manifest')
    manifest = {'format': FORMAT, 'formatVersion': VERSION, 'caseId': case_id,
                'evidenceClass': evidence_class, 'sessions': list(sessions.values()),
                'alignment': {'kind': 'paired-sample-index' if aligned else 'none', 'uncertaintyNano': None},
                'triggers': [], 'events': [], 'gaps': [],
                'provenance': {'sources': sources, 'analyzerSha256': digest((ROOT / 'tools/analyze_incident.py').read_bytes()),
                               'integrity': 'sha256-content-integrity-not-authenticity'},
                'privacy': {'mode': 'private' if private else 'support', 'mediaPayloads': False, 'credentials': False},
                'budgets': {'maxCaseBytes': MAX_BYTES, 'maxRecordingBytes': MAX_FILE_BYTES, 'maxSamplesPerRecording': MAX_SAMPLES, 'maxRecordings': MAX_RECORDINGS},
                'files': []}
    if trigger_index is not None:
        require(type(trigger_index) is int and trigger_index >= 0, 'invalid trigger index')
        require(all(trigger_index < len(rec['samples']) for rec in recordings.values()), 'trigger outside retained samples')
        manifest['triggers'] = [{'kind': 'manual', 'sampleIndex': trigger_index, 'timeDomain': 'logical-window' if aligned else 'endpoint-local'}]
    for endpoint in endpoints:
        rec = recordings[endpoint['recording']]
        manifest['events'].append({'kind': 'session-start', 'endpointId': endpoint['endpointId'], 'connectionEpoch': 1, 'sampleIndex': 0})
        if rec['header']['overwrittenCount']:
            manifest['gaps'].append({'kind': 'prehistory-overwritten', 'endpointId': endpoint['endpointId'], 'count': rec['header']['overwrittenCount']})
    case = {'manifest': manifest, 'recordings': recordings}
    if capture is not None:
        apply_capture_metadata(case, capture)
    return case if private else redact_case(case)


def apply_capture_metadata(case, capture):
    """Adapt our instrumented host's digest-pinned observation brackets, never its ground truth diagnosis."""
    m = case['manifest']
    require(capture.get('schemaVersion') == 1 and capture.get('evidenceClass') == 'new-experiment', 'unsupported capture metadata')
    require(m['evidenceClass'] == 'new-experiment', 'capture evidence class mismatch')
    require(len(capture.get('epochs', [])) == 1, 'pack one capture epoch at a time')
    epoch = capture['epochs'][0]
    epoch_match = re.fullmatch(r'epoch-([1-9][0-9]*)', epoch['epochId'])
    require(epoch_match is not None, 'unsupported native epoch ID')
    epoch_id = int(epoch_match.group(1))
    clock = capture['clock']
    require(clock.get('kind') == 'host-monotonic' and clock.get('mapping') == 'per-endpoint-offset-intervals-from-command-brackets', 'capture has no measured clock mapping')
    require(len(clock.get('brackets', [])) <= MAX_SAMPLES * 2, 'capture bracket budget exceeded')
    cfg = capture['configuration']
    require(cfg.get('transport') == 'live' and cfg.get('api') == 'message' and cfg.get('encryption') == 'none' and cfg.get('mediaPayloadRetained') is False, 'unsupported initial capture profile')
    m['alignment'] = {'kind': 'coordinator-brackets', 'uncertaintyNano': 0, 'windows': []}
    m['events'], m['triggers'] = [], []
    for session in m['sessions']:
        require(session['sessionId'] == capture['flowId'], 'capture flow mismatch')
        for endpoint in session['endpoints']:
            role = endpoint['role']
            rec = case['recordings'][endpoint['recording']]
            source_index = list(case['recordings']).index(endpoint['recording'])
            source_hash = m['provenance']['sources'][source_index]['sha256']
            require(capture['files'][epoch[role]]['sha256'] == source_hash, 'native recording digest mismatch')
            require(rec['header']['configuredProvider'] == capture['provider']['name'] and rec['header']['providerRevision'] == capture['provider']['revision'], 'native provider provenance mismatch')
            endpoint['connectionEpoch'] = epoch_id
            endpoint['clock'] = {'kind': 'host-monotonic', 'domainId': clock['endpointDomains'][role], 'mapping': 'coordinator-brackets', 'uncertaintyNano': clock['uncertaintyNano']}
            endpoint['configuration']['profile'] = 'live-message-ipv4-loopback-clear'
            for field in ('payloadBytes', 'samplingNominalHz', 'preRecords', 'postMilliseconds', 'capacityRecordsPerEndpoint'):
                endpoint['configuration'][field] = cfg[field]
            options = cfg.get('effectiveSocketOptions', {}).get(role)
            if options:
                require(options['transport'] == 'live' and options['messageApi'] is True, 'effective native profile mismatch')
                endpoint['configuration'].update({key: options[key] for key in ('messageApi', 'senderTsbpdDelaySeconds', 'receiverTsbpdDelaySeconds')})
            provider = capture['provider']
            if 'librarySha256' in provider:
                endpoint['build'] = {'librarySha256': provider['librarySha256'], 'peerSha256': provider['peerSha256'],
                                     'publicApiVersionHex': provider['observedRuntimeProfiles'][role]['publicApiVersionHex']}
            by_native = {b['endpointMonotonicNano'][role]: b for b in clock['brackets']}
            require(len(by_native) == len(clock['brackets']), 'duplicate native clock bracket')
            for sample in rec['samples']:
                stamp = sample['raw']['sample_monotonic_nano']
                require(stamp in by_native, 'retained sample has no capture bracket')
                b = by_native[stamp]
                begin, end = b['beginMonotonicNano'], b['endMonotonicNano']
                require(type(begin) is int and type(end) is int and 0 <= begin <= end, 'invalid coordinator bracket')
                require(b['endpointToCoordinatorOffsetNano'][role] == [begin-stamp, end-stamp], 'invalid clock offset interval')
                require(b['beginUnixNano'] <= sample['raw']['observed_at_unix_nano'] <= b['endUnixNano'], 'wall sample outside coordinator bracket')
                m['alignment']['windows'].append({'endpointId': endpoint['endpointId'], 'sampleIndex': sample['sampleIndex'], 'startNano': begin, 'endNano': end, 'sampleMonotonicNano': stamp})
                m['alignment']['uncertaintyNano'] = max(m['alignment']['uncertaintyNano'], end-begin)
            state = epoch['window'][role]
            if state['preOverwrittenCount']:
                m['gaps'].append({'kind': 'prehistory-overwritten', 'endpointId': endpoint['endpointId'], 'count': state['preOverwrittenCount']})
            endpoint['captureWindow'] = {key: state[key] for key in ('preRecordCount', 'preOverwrittenCount', 'recordCount', 'allocatedRecords', 'allocatedRecordBytes', 'triggerMonotonicNano', 'deadlineMonotonicNano', 'finishReason')}
            if rec['samples']:
                first = rec['samples'][0]['raw']['sample_monotonic_nano']
                last = rec['samples'][-1]['raw']['sample_monotonic_nano']
                endpoint['captureWindow'].update(firstRetainedMonotonicNano=first, lastRetainedMonotonicNano=last,
                    retainedPreNano=max(0, state['triggerMonotonicNano']-first), retainedPostNano=max(0, last-state['triggerMonotonicNano']))
            m['triggers'].append({'kind': 'manual', 'endpointId': endpoint['endpointId'], 'connectionEpoch': epoch_id,
                                  'sampleIndex': state['preRecordCount'], 'timeDomain': 'endpoint-local',
                                  'timeNano': state['triggerMonotonicNano'], 'deadlineNano': state['deadlineMonotonicNano'],
                                  'coalescedCount': state['triggerCount']-1})
            m['events'].append({'kind': 'session-start', 'endpointId': endpoint['endpointId'], 'connectionEpoch': epoch_id})
            if state['finishReason'] != 'deadline':
                m['gaps'].append({'kind': 'post-window-truncated', 'endpointId': endpoint['endpointId']})


def redact_case(case):
    """Rebuild exported identities; never embed original capture files or arbitrary text."""
    case = copy.deepcopy(case)
    manifest = case['manifest']
    pseudo = pseudonymizer()
    manifest['caseId'] = str(uuid.uuid4())
    manifest['privacy']['mode'] = 'support'
    for session in manifest['sessions']:
        session['sessionId'] = pseudo(session['sessionId'])
        for endpoint in session['endpoints']:
            endpoint['endpointId'] = pseudo(endpoint['endpointId'])
            endpoint['clock']['domainId'] = pseudo(endpoint['clock']['domainId'])
    for event in manifest['events'] + manifest['gaps'] + manifest['triggers']:
        if 'endpointId' in event:
            event['endpointId'] = pseudo(event['endpointId'])
    for window in manifest['alignment'].get('windows', []):
        window['endpointId'] = pseudo(window['endpointId'])
    for rec in case['recordings'].values():
        h = rec['header']
        h['streamId'], h['flowId'] = pseudo(h['streamId']), pseudo(h['flowId'])
        h['providerVersion'] = safe_version(h['providerVersion'])
        if not re.fullmatch('[0-9a-f]{40}', h['providerRevision']):
            h['providerRevision'] = 'redacted'
        h['buildProfile'] = 'redacted-profile-see-sha256'
        for sample in rec['samples']:
            sample['streamId'] = h['streamId']
    return case


def write_case(case, output):
    manifest = copy.deepcopy(case['manifest'])
    files = {name: recording_bytes(rec) for name, rec in case['recordings'].items()}
    for data in files.values():
        parse_recording(data)
    manifest['files'] = [{'path': name, 'sizeBytes': len(data), 'sha256': digest(data), 'kind': 'telemetry-jsonl'} for name, data in files.items()]
    files['manifest.json'] = json_bytes(manifest)
    validate_manifest(manifest, {k: v for k, v in files.items() if k != 'manifest.json'})
    validate_associations({'manifest': manifest, 'recordings': case['recordings']})
    require(len(files['manifest.json']) <= MAX_MANIFEST_BYTES and sum(map(len, files.values())) <= MAX_BYTES - 65536, 'case exceeds byte budget')
    def write(stream):
        with zipfile.ZipFile(stream, 'w', compression=zipfile.ZIP_STORED) as archive:
            for name, data in files.items():
                archive.writestr(name, data)
    atomic_write(output, write)


def read_case(path):
    data = bounded_read(path, MAX_BYTES)
    try:
        with zipfile.ZipFile(io.BytesIO(data)) as archive:
            entries = archive.infolist()
            require(2 <= len(entries) <= MAX_RECORDINGS + 1, 'archive member budget exceeded')
            names = [entry.filename for entry in entries]
            require(len(set(names)) == len(names), 'duplicate archive member')
            require('manifest.json' in names, 'missing manifest')
            require(sum(entry.file_size for entry in entries) <= MAX_BYTES, 'expanded archive budget exceeded')
            files = {}
            for entry in entries:
                require(entry.filename == 'manifest.json' or re.fullmatch(r'recordings/endpoint-[1-8]\.jsonl', entry.filename), 'unsafe or unknown archive path')
                require(entry.file_size <= (MAX_MANIFEST_BYTES if entry.filename == 'manifest.json' else MAX_FILE_BYTES), 'archive file budget exceeded')
                require(not entry.flag_bits & 1 and entry.compress_type in (zipfile.ZIP_STORED, zipfile.ZIP_DEFLATED), 'unsupported archive encoding')
                require((entry.external_attr >> 16) & 0o170000 != 0o120000, 'symlinks forbidden')
                files[entry.filename] = archive.read(entry)
    except (zipfile.BadZipFile, RuntimeError, OSError, NotImplementedError, zlib.error) as error:
        raise CaseError('unreadable case archive: ' + str(error)) from error
    manifest = strict_json(files.pop('manifest.json'))
    validate_manifest(manifest, files)
    recordings = {name: parse_recording(content) for name, content in files.items()}
    case = {'manifest': manifest, 'recordings': recordings}
    validate_associations(case)
    return case


def validate_manifest(m, files):
    validate_contract(m, CASE_SCHEMA)
    require(isinstance(m, dict), 'manifest must be an object')
    expected = {'format', 'formatVersion', 'caseId', 'evidenceClass', 'sessions', 'alignment', 'triggers', 'events', 'gaps', 'provenance', 'privacy', 'budgets', 'files'}
    require(set(m) == expected, 'unexpected or missing manifest fields')
    require(m['format'] == FORMAT and m['formatVersion'] == VERSION, 'unsupported case format version')
    require(isinstance(m['caseId'], str) and len(m['caseId']) <= 128, 'invalid case ID')
    require(m['evidenceClass'] in EVIDENCE_CLASSES, 'invalid evidence class')
    require(isinstance(m['files'], list) and len(m['files']) == len(files), 'file inventory mismatch')
    require({f['path'] for f in m['files']} == set(files), 'file inventory mismatch')
    for item in m['files']:
        content = files[item['path']]
        require(item['sizeBytes'] == len(content) and item['sha256'] == digest(content), 'file integrity mismatch')
    require(isinstance(m['sessions'], list) and 1 <= len(m['sessions']) <= MAX_RECORDINGS, 'invalid sessions')
    for field in ('triggers', 'events', 'gaps'):
        require(isinstance(m[field], list) and len(m[field]) <= 256, 'event budget exceeded')
    require(m['alignment'].get('kind') in ('none', 'paired-sample-index', 'coordinator-brackets'), 'unsupported alignment')
    require(m['privacy'] in ({'mode': mode, 'mediaPayloads': False, 'credentials': False} for mode in ('private', 'support')), 'unsupported privacy mode')


def validate_associations(case):
    seen, identities = set(), set()
    for session in case['manifest']['sessions']:
        require(isinstance(session.get('endpoints'), list), 'missing endpoints')
        for endpoint in session['endpoints']:
            name = endpoint['recording']
            require(name in case['recordings'] and name not in seen, 'duplicate or missing endpoint recording')
            seen.add(name)
            h = case['recordings'][name]['header']
            require(h['flowId'] == session['sessionId'] and h['streamId'] == endpoint['endpointId'] and h['role'] == endpoint['role'], 'endpoint association mismatch')
            require(type(endpoint['connectionEpoch']) is int and endpoint['connectionEpoch'] > 0, 'invalid connection epoch')
            identity = (endpoint['endpointId'], endpoint['connectionEpoch'])
            require(identity not in identities, 'duplicate endpoint epoch')
            identities.add(identity)
            clock = endpoint['clock']
            require(clock.get('kind') in ('endpoint-local', 'orchestrator-logical', 'host-monotonic'), 'unknown clock kind')
            require(isinstance(clock.get('domainId'), str) and clock['domainId'], 'missing clock domain')
            uncertainty = clock.get('uncertaintyNano')
            require(uncertainty is None or type(uncertainty) is int and uncertainty >= 0, 'invalid clock uncertainty')
    require(seen == set(case['recordings']), 'unassociated recording')


def assess_case(case):
    m = case['manifest']
    reasons, timeline, streams = [], [], {}
    endpoints = [e for s in m['sessions'] for e in s['endpoints']]
    logical = m['alignment']['kind'] == 'paired-sample-index'
    coordinated = m['alignment']['kind'] == 'coordinator-brackets'
    aligned = logical or coordinated
    windows = {(w['endpointId'], w['sampleIndex']): w for w in m['alignment'].get('windows', [])}
    if coordinated:
        if len(windows) != len(m['alignment'].get('windows', [])):
            reasons.append('Doppelte Zeitzuordnung für ein Messfenster.')
        if not any(s['kind'] == 'capture-metadata' for s in m['provenance']['sources']):
            reasons.append('Herkunft der gemessenen Uhrzuordnung fehlt.')
    if logical and not any(s['kind'] == 'source-manifest' for s in m['provenance']['sources']):
        reasons.append('Herkunft der logischen Paarung fehlt.')
    if not aligned:
        reasons.append('Keine belegte Zuordnung gemeinsamer Messfenster; unabhängige Uhren bleiben getrennt.')
    if len(m['sessions']) != 1 or len(endpoints) != 2:
        reasons.append('Die erste Analyse benötigt genau eine Verbindungsepoche mit Sender und Empfänger.')
    if len({e['connectionEpoch'] for e in endpoints}) != 1:
        reasons.append('Unterschiedliche Verbindungsepochen werden nicht miteinander korreliert.')
    if any(gap['kind'] != 'prehistory-overwritten' for gap in m['gaps']):
        reasons.append('Erfassungslücken oder überschriebenen Vorlauf vor einer Diagnose prüfen.')
    if any(event['kind'] in ('reconnect', 'configuration-change', 'clock-boundary', 'counter-reset', 'sample-failure') for event in m['events']):
        reasons.append('Ein explizites Ereignis markiert einen Verbindungs-, Konfigurations- oder Messzustandswechsel.')
    for endpoint in endpoints:
        recording = case['recordings'][endpoint['recording']]
        expected_mapping = 'paired-sample-index' if logical else 'coordinator-brackets' if coordinated else 'none'
        if endpoint['clock']['mapping'] != expected_mapping:
            reasons.append('Uhrmodell und deklarierte Paarung widersprechen sich.')
        expected_kind = 'orchestrator-logical' if logical else 'host-monotonic' if coordinated else 'endpoint-local'
        if endpoint['clock']['kind'] != expected_kind:
            reasons.append('Uhrtyp und deklarierte Paarung widersprechen sich.')
        role = endpoint['role']
        if role in streams:
            reasons.append('Mehrere Aufzeichnungen derselben Rolle werden nicht über Epochen hinweg kombiniert.')
        streams[role] = recording
        previous = None
        previous_window_end = None
        for sample in recording['samples']:
            raw, delta = sample['raw'], sample['delta']
            flags = [name for bit, name in analyzer.QUALITY_FLAGS.items() if delta['quality_flags'] & bit]
            if delta['quality_flags'] & 14:
                reasons.append('Reset, Providerwechsel oder Rücksprung der monotonen Uhr begrenzt die Auswertung.')
            if delta['counter_reset_fields']:
                reasons.append('Ein Rücksetzen von Zählern begrenzt die Auswertung.')
            if previous:
                dm = raw['sample_monotonic_nano'] - previous['sample_monotonic_nano']
                dw = raw['observed_at_unix_nano'] - previous['observed_at_unix_nano']
                if dm <= 0:
                    flags.append('monotonic-clock-boundary')
                    reasons.append('Nicht steigende monotone Zeit bildet eine Grenze.')
                if abs(dw - dm) > 100000000:  # Declared 100 ms detection threshold, not a sync guarantee.
                    flags.append('wall-clock-jump')
                    reasons.append('Ein Wanduhrsprung macht die zeitliche Zuordnung unsicher.')
                if delta['interval_nano'] != max(dm, 0) and not delta['quality_flags']:
                    flags.append('interval-mismatch')
                    reasons.append('Intervall und monotone Zeit widersprechen sich.')
                for field, delta_field in zip(RAW_FIELDS[:15], DELTA_FIELDS):
                    old, new, change = previous[field], raw[field], delta[delta_field]
                    if old is not None and new is not None:
                        if new < old:
                            flags.append('counter-reset-observed')
                            reasons.append('Fallende Lebenszeitzähler bilden eine Grenze.')
                        elif change is not None and not delta['quality_flags'] and new - old != change:
                            flags.append('counter-delta-mismatch')
                            reasons.append('Rohzähler und übermittelte Differenz widersprechen sich.')
            if raw['provider'] != recording['header']['configuredProvider']:
                reasons.append('Providerwechsel innerhalb einer Verbindungsepoche.')
            previous = raw
            row = {'endpointId': endpoint['endpointId'], 'role': role, 'connectionEpoch': endpoint['connectionEpoch'],
                             'sampleIndex': sample['sampleIndex'], 'windowIndex': sample['sampleIndex'] if aligned else None,
                             'domainId': endpoint['clock']['domainId'], 'timeNano': raw['sample_monotonic_nano'],
                             'uncertaintyNano': endpoint['clock']['uncertaintyNano'], 'quality': flags, 'raw': raw, 'delta': delta}
            if coordinated:
                window = windows.get((endpoint['endpointId'], sample['sampleIndex']))
                if window is None or window['sampleMonotonicNano'] != raw['sample_monotonic_nano']:
                    reasons.append('Zeitzuordnung eines aufbewahrten Samples fehlt oder widerspricht der Aufzeichnung.')
                else:
                    start, end = window['startNano'], window['endNano']
                    if end < start or previous_window_end is not None and start <= previous_window_end:
                        reasons.append('Überlappende oder rückläufige Messfenster erlauben keine geordnete Auswertung.')
                    previous_window_end = end
                    row.update(windowStartNano=start, windowEndNano=end, uncertaintyNano=end-start)
            timeline.append(row)
    analysis = {'selectedHypothesis': {'id': None, 'assessment': 'insufficient-evidence', 'rationale': 'Unzureichende Daten'}, 'hypotheses': [], 'nonClaims': []}
    if not reasons:
        try:
            analyzer.validate_streams(streams)
            # Common indexes alone are insufficient: the declared harness must retain its shared logical stamps.
            if logical:
                for sender, receiver in zip(streams['sender']['samples'], streams['receiver']['samples']):
                    require(sender['raw']['sample_monotonic_nano'] == receiver['raw']['sample_monotonic_nano'], 'paired logical timestamps disagree')
            if coordinated:
                for index in range(len(streams['sender']['samples'])):
                    pair = [windows[(e['endpointId'], index)] for e in endpoints]
                    require(pair[0]['startNano'] == pair[1]['startNano'] and pair[0]['endNano'] == pair[1]['endNano'], 'endpoints do not share a coordinator sample window')
            source = {'format': 'robotweax-srt-telemetry-jsonl-v1', 'incidentId': None, 'failureClass': None,
                      'flowId': m['sessions'][0]['sessionId'], 'artifacts': m['files'], 'streams': streams, 'nonClaims': []}
            analysis = analyzer.build_analysis(source)
        except (RuntimeError, CaseError) as error:
            reasons.append(str(error))
    if reasons:
        analysis['selectedHypothesis'] = {'id': None, 'assessment': 'insufficient-evidence', 'rationale': '; '.join(dict.fromkeys(reasons))}
    case['analysis'] = analysis
    case['assessment'] = {'status': analysis['selectedHypothesis']['assessment'], 'reasons': list(dict.fromkeys(reasons)),
                          'timePolicy': 'qualified-logical-sample-windows' if logical else 'coordinator-sample-windows' if coordinated else 'independent-clock-domains'}
    case['timeline'] = timeline
    return case


def render(case, output):
    from flight_recorder_view import render_case
    data = render_case(assess_case(case)).encode()
    require(len(data) <= MAX_BYTES, 'view exceeds byte budget')
    atomic_write(output, lambda stream: stream.write(data))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    pack = commands.add_parser('pack', help='package native recorder JSONL, sanitized by default')
    pack.add_argument('recordings', nargs='+', type=Path)
    pack.add_argument('--output', required=True, type=Path)
    pack.add_argument('--evidence-class', required=True, choices=EVIDENCE_CLASSES)
    pack.add_argument('--paired-logical-windows', action='store_true')
    pack.add_argument('--source-manifest', type=Path)
    pack.add_argument('--capture-metadata', type=Path)
    pack.add_argument('--trigger-index', type=int)
    pack.add_argument('--private', action='store_true', help='retain original stream identities locally')
    for name in ('inspect', 'view', 'redact'):
        cmd = commands.add_parser(name)
        cmd.add_argument('case', type=Path)
        if name != 'inspect':
            cmd.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    try:
        if args.command == 'pack':
            case = make_case(args.recordings, args.evidence_class, args.paired_logical_windows, args.trigger_index,
                             args.source_manifest, args.capture_metadata, args.private)
            write_case(case, args.output)
            # Verify the actual published representation, including hashes and associations.
            read_case(args.output)
        elif args.command == 'inspect':
            case = assess_case(read_case(args.case))
            print(json.dumps({'caseId': case['manifest']['caseId'], 'evidenceClass': case['manifest']['evidenceClass'],
                              'assessment': case['assessment'], 'selectedHypothesis': case['analysis']['selectedHypothesis']}, ensure_ascii=False, indent=2))
        elif args.command == 'view':
            render(read_case(args.case), args.output)
        else:
            write_case(redact_case(read_case(args.case)), args.output)
        return 0
    except (CaseError, OSError, KeyError, TypeError, ValueError) as error:
        print('Flight Recorder: ' + str(error), file=sys.stderr)
        return 2


if __name__ == '__main__':
    raise SystemExit(main())
