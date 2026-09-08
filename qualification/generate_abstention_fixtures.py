#!/usr/bin/env python3
"""Deterministic synthetic derivatives; never a new provider capture."""
import argparse
import json
from pathlib import Path
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import operator_evaluation as evaluation

SOURCE = ROOT / 'testdata/golden-incidents/incident-006-jsonl-constant-loss-robotweax-haivision/manifest.json'
SOURCE_SHA = 'd4348d163af6bd0ea2a5bcdc0ae9e41c5bf2c2e4e1a3f4bbf85bce55efbd16cf'


def generate(output):
    output = output.resolve()
    if output.exists():
        raise RuntimeError('refusing to overwrite fixtures')
    evaluation.validate_digest(SOURCE, SOURCE_SHA, 'synthetic source manifest')
    source = evaluation.read_json(SOURCE)
    analyzer = evaluation.load_analyzer(ROOT / 'tools/analyze_incident.py')
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='.abstention-', dir=output.parent) as temporary:
        staging = Path(temporary) / 'fixtures'
        staging.mkdir()
        for number, mode in ((900, 'missing'), (901, 'contradictory')):
            identity = f'incident-{number}-synthetic-{mode}'
            directory = staging / identity
            directory.mkdir()
            recordings = {'schemaVersion': '1alpha1', 'format': 'robotweax-srt-telemetry-jsonl-v1', 'flowId': identity}
            paths = []
            for role in ('sender', 'receiver'):
                path = SOURCE.parent / source['recordings'][role]['file']
                evaluation.validate_digest(path, source['recordings'][role]['sha256'], role)
                rows = [json.loads(line) for line in path.read_text().splitlines()]
                rows[0].update(streamId=identity + '-' + role, flowId=identity,
                               buildProfile='synthetic-derivative-not-provider-qualification')
                for sample in rows[1:]:
                    sample['streamId'] = identity + '-' + role
                    raw, delta = sample['raw'], sample['delta']
                    if mode == 'missing':
                        for key in raw:
                            if key not in ('provider', 'capabilities', 'available_fields',
                                           'observed_at_unix_nano', 'sample_monotonic_nano'):
                                raw[key] = None
                        raw['available_fields'] = 0
                        for key in delta:
                            if key not in ('interval_nano', 'quality_flags', 'counter_reset_fields',
                                           'available_delta_fields', 'available_rates'):
                                delta[key] = None
                        delta.update(available_delta_fields=0, available_rates=0, counter_reset_fields=0)
                    elif role == 'receiver' and sample['sampleIndex'] >= 3:
                        raw['receiver_drop_packet_total'] += 7
                        if sample['sampleIndex'] == 3:
                            delta['receiver_drop_packet_delta'] += 7
                target = directory / (role + '.jsonl')
                target.write_text(''.join(json.dumps(r, sort_keys=True, separators=(',', ':')) + '\n' for r in rows))
                recordings[role] = dict(file=target.name, sha256=evaluation.sha256_file(target))
                paths.append(target)
            analysis = analyzer.build_analysis(analyzer.load_jsonl(paths))
            evaluation.write_new_json(directory / 'analysis.json', analysis)
            recordings['analysis'] = dict(file='analysis.json', sha256=evaluation.sha256_file(directory / 'analysis.json'))
            manifest = {
                'schemaVersion': '1alpha1', 'incidentId': identity,
                'failureClass': 'synthetic-' + mode + '-evidence', 'recordings': recordings,
                'provenance': {'kind': 'synthetic-retained-data-derivative', 'capturedFromProvider': False,
                               'generator': 'qualification/generate_abstention_fixtures.py',
                               'sourceManifest': {'file': SOURCE.relative_to(ROOT).as_posix(), 'sha256': SOURCE_SHA}},
                'mutation': ('All measurements unavailable; masks cleared; clocks, roles and provider labels retained.'
                             if mode == 'missing' else
                             'Add seven receiver drops at sample 3 and to later totals, preserving all-delivered unique counters.'),
                'expectedEvidence': ['No failure-domain attribution is justified by this synthetic input.'],
                'contradictions': ['Not an observed provider run; source capture metadata does not describe these mutated values.'],
                'machineExpectations': {'selectedAssessment': 'insufficient-evidence',
                                        'firstDeviationSample': None if mode == 'missing' else 3}}
            if analysis['selectedHypothesis']['assessment'] != 'insufficient-evidence':
                raise RuntimeError('synthetic case no longer exercises abstention')
            evaluation.write_new_json(directory / 'manifest.json', manifest)
        staging.rename(output)
    return output


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    print(generate(parser.parse_args().output))
