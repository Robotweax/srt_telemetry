"""Validate a case-specific measurement rubric without importing the analyzer.

This is a regression oracle and technical review, not an independent human
adjudication or another incident classifier. Expectations are authored in the
versioned rubric; this module never derives answers from report hypotheses.
"""
import copy
import hashlib
import json

KIND = 'robotweax-operator-measurement-rubric-v2'
ANSWER_FIELDS = ('firstDeviationSample', 'lastDeviationSample', 'recoverySample',
                 'failureDomain', 'supportedHypotheses', 'missingEvidence')


def require(condition, message):
    if not condition:
        raise RuntimeError('Reference review: ' + message)


def raw_digest(raw):
    # Study IDs vary; the measured rows and endpoint metadata do not.
    data = {key: raw[key] for key in ('rows', 'streams')}
    return hashlib.sha256(json.dumps(data, sort_keys=True, separators=(',', ':'),
                                    allow_nan=False).encode()).hexdigest()


def reviewed_reference(rubric, raw, reference):
    require(rubric.get('kind') == KIND and rubric.get('humanReview') == 'pending',
            'unsupported rubric or unsupported human approval claim')
    entries = rubric.get('scenarios', [])
    require(len(entries) == len({e['scenarioId'] for e in entries}), 'duplicate rubric scenario')
    matches = [e for e in entries if e['scenarioId'] == raw['scenarioId']]
    require(len(matches) == 1, 'scenario is not covered by rubric')
    entry = matches[0]
    require(entry['observationsSha256'] == raw_digest(raw), 'measurements differ from reviewed baseline')
    require(set(entry['expected']) == set(ANSWER_FIELDS), 'incomplete reference expectation')
    require(entry.get('rationale') and entry.get('assertions'), 'missing review reasoning or measurements')
    for assertion in entry['assertions']:
        rows = [r for r in raw['rows'] if r['role'] == assertion['role']]
        require([r['sampleIndex'] for r in rows] == assertion['sampleIndices'], 'sample sequence changed')
        actual = [r[assertion['section']].get(assertion['field']) for r in rows]
        require(actual == assertion['values'], 'measurement assertion failed: ' + assertion['field'])
    result = copy.deepcopy(reference)
    result.update(copy.deepcopy(entry['expected']))
    result['referencePolicy'] = KIND
    return result


def verify_study(rubric, study, base):
    require({e['scenarioId'] for e in rubric['scenarios']} ==
            {s['scenarioId'] for s in study['scenarios']}, 'rubric coverage differs from study')
    for scenario in study['scenarios']:
        raw = json.loads((base / scenario['views']['raw-statistics']['file']).read_text())
        reference = json.loads((base / scenario['reference']['file']).read_text())
        require(reference == reviewed_reference(rubric, raw, reference),
                'reference differs from measurement rubric')
