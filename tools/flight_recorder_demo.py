#!/usr/bin/env python3
"""Build reviewable offline example cases from retained captures and optional new experiments."""
import argparse
import copy
import json
from pathlib import Path
import sys

import flight_recorder as fr

EXAMPLES = (
    ('loss-recovery', 'incident-006-jsonl-constant-loss-robotweax-haivision', 'transmission-path-loss-with-recovery'),
    ('receiver-pressure', 'incident-010-receiver-pause-robotweax-haivision', 'receiver-side-pressure'),
    ('playout-margin', 'incident-014-recovery-margin-robotweax-haivision', 'receiver-recovery-margin-exhausted'),
)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True, help='new output directory; existing directories are refused')
    parser.add_argument('--capture-root', type=Path, help='new run directory containing loss-recovery and receiver-pause captures')
    args = parser.parse_args()
    fr.require(not args.output.exists(), 'output directory already exists')
    args.output.mkdir(parents=True)
    results = []

    def publish(name, case, expected):
        package = args.output / (name + '.frcase')
        view = args.output / (name + '.html')
        fr.write_case(case, package)
        loaded = fr.assess_case(fr.read_case(package))
        selected = loaded['analysis']['selectedHypothesis']
        fr.require(selected['id'] == expected, name + ': unexpected diagnostic result')
        fr.render(loaded, view)
        results.append({'name': name, 'evidenceClass': loaded['manifest']['evidenceClass'],
                        'package': package.name, 'sha256': fr.digest(package.read_bytes()), 'view': view.name,
                        'assessment': loaded['assessment'], 'selectedHypothesis': selected, 'bytes': package.stat().st_size})

    for name, directory, expected in EXAMPLES:
        source = fr.ROOT / 'testdata/golden-incidents' / directory
        case = fr.make_case([source / 'sender.jsonl', source / 'receiver.jsonl'], 'retained-capture', True, 3, source / 'manifest.json')
        publish('retained-' + name, case, expected)
    source = fr.ROOT / 'testdata/golden-incidents' / EXAMPLES[0][1]
    base = fr.make_case([source / 'sender.jsonl', source / 'receiver.jsonl'], 'synthetic-fixture', True, 3, source / 'manifest.json')
    missing = copy.deepcopy(base)
    receiver = list(missing['recordings'].values())[1]
    for sample in receiver['samples']:
        sample['delta']['receiver_loss_packet_delta'] = None
        sample['delta']['available_delta_fields'] &= ~(1 << 5)
    publish('synthetic-missing-evidence', missing, None)
    contradictory = copy.deepcopy(base)
    receiver = list(contradictory['recordings'].values())[1]
    receiver['samples'][3]['delta']['receiver_drop_packet_delta'] = 1
    for sample in receiver['samples'][3:]:
        sample['raw']['receiver_drop_packet_total'] = 1
    publish('synthetic-contradictory-evidence', contradictory, None)
    if args.capture_root:
        for name, expected in [('loss-recovery', 'transmission-path-loss-with-recovery'), ('receiver-pause', 'receiver-side-pressure')]:
            source = args.capture_root / name
            case = fr.make_case([source / 'sender.jsonl', source / 'receiver.jsonl'], 'new-experiment', capture_metadata=source / 'capture-manifest.json')
            # Fresh evidence is reported honestly even if the narrow old rules abstain.
            actual = fr.assess_case(case)['analysis']['selectedHypothesis']['id']
            publish('new-' + name, case, actual)
    (args.output / 'results.json').write_text(json.dumps(results, ensure_ascii=False, indent=2) + '\n')
    (args.output / 'README.md').write_text(
        '# Flight Recorder examples\n\n'
        'Open an HTML file locally. Every view is generated only after strict package import. '
        'Use flight_recorder.py inspect/view/redact to recheck or export.\n\n'
        'retained-* are existing real provider captures on their recorded older revisions. '
        'new-* are newly executed instrumented socket captures on the exact revision in each package. '
        'synthetic-missing-evidence removes receiver loss availability; synthetic-contradictory-evidence '
        'adds one receiver drop to the loss case while unique delivery counters retain full recovery. '
        'They are derived fixtures, not new provider measurements.\n\n'
        'Case IDs and pseudonyms intentionally change on regeneration; observations and expected results do not. '
        'SHA-256 checks detect corruption, not malicious reauthoring. No media payloads are retained.\n')
    print(json.dumps(results, ensure_ascii=False, indent=2))
    return 0


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (fr.CaseError, OSError, KeyError, ValueError) as error:
        print('Flight Recorder demo: ' + str(error), file=sys.stderr)
        raise SystemExit(2)
