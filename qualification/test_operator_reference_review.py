"""Technical rubric checks; all response examples are synthetic test data."""
import copy
import json
from pathlib import Path
import shutil
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
sys.path.insert(0, str(ROOT / 'qualification'))
import operator_evaluation as evaluation
import operator_reference_review as review
import generate_abstention_fixtures as fixtures

PACKET = ROOT / 'qualification/operator-evaluation/measurement-review-v2'


class ReferenceReviewTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        shutil.copytree(PACKET / 'study', self.root / 'study')
        self.path = self.root / 'study/study.json'
        self.study, self.base = evaluation.validate_study(self.path)

    def case(self, number):
        scenario = next(s for s in self.study['scenarios'] if s['scenarioId'].startswith(f'incident-{number:03}'))
        return scenario, evaluation.load_study_artifact(self.base, scenario['views']['raw-statistics']), evaluation.load_study_artifact(self.base, scenario['reference'])

    def test_ten_cases_are_balanced_with_two_explicit_synthetic_abstentions(self):
        self.assertEqual(len(self.study['scenarios']), 10)
        for arm in self.study['arms'].values():
            self.assertEqual(sum(a['condition'] == 'raw-statistics' for a in arm), 5)
        for n in (900, 901):
            _, _, ref = self.case(n)
            self.assertEqual(ref['failureDomain'], 'insufficient-evidence')
            self.assertEqual(ref['supportedHypotheses'], [])

    def test_derivatives_reproduce_byte_for_byte(self):
        output = fixtures.generate(self.root / 'fixtures')
        originals = sorted((PACKET / 'fixtures').rglob('*'))
        for path in originals:
            if path.is_file():
                self.assertEqual(path.read_bytes(), (output / path.relative_to(PACKET / 'fixtures')).read_bytes())
        with self.assertRaises(RuntimeError): fixtures.generate(output)

    def test_missing_means_null_with_zero_availability_and_unknown_onset(self):
        _, raw, ref = self.case(900)
        for row in raw['rows']:
            self.assertEqual(row['raw']['available_fields'], 0)
            self.assertEqual(row['delta']['available_delta_fields'], 0)
            self.assertEqual(row['delta']['available_rates'], 0)
            self.assertIsNone(row['delta']['receiver_loss_packet_delta'])
            self.assertIsNone(row['raw']['rtt_seconds'])
        self.assertIsNone(ref['firstDeviationSample'])

    def test_contradiction_is_numerically_present_not_just_labeled(self):
        _, raw, ref = self.case(901)
        for index in range(1, 9):
            rows = {r['role']:r for r in raw['rows'] if r['sampleIndex'] == index}
            self.assertEqual(rows['sender']['delta']['unique_sent_packet_delta'],
                             rows['receiver']['delta']['unique_received_packet_delta'])
            self.assertEqual(rows['receiver']['delta']['receiver_drop_packet_delta'], 7 if index == 3 else 0)
        self.assertEqual(ref['firstDeviationSample'], 3)
        self.assertIn('receiver-drop-delivery-consistency', ref['missingEvidence'])

    def test_zero_occupancy_is_available_not_missing(self):
        for number in (12, 13, 14, 15):
            _, raw, ref = self.case(number)
            self.assertTrue(any(r['raw']['receive_buffer_bytes'] == 0 for r in raw['rows']))
            self.assertNotIn('receiver-buffer-occupancy', ref['missingEvidence'])

    def test_rubric_does_not_use_analyzer_reference_answers(self):
        _, raw, reference = self.case(6)
        rubric = evaluation.load_study_artifact(self.base, self.study['referenceReview'])
        damaged = dict(reference, firstDeviationSample=900, failureDomain='exact-network-hop', missingEvidence=[])
        result = review.reviewed_reference(rubric, raw, damaged)
        self.assertEqual(result, reference)

    def test_rehashed_wrong_reference_is_rejected(self):
        scenario, _, ref = self.case(6)
        ref['failureDomain'] = 'sender-endpoint'
        path = self.base / scenario['reference']['file']
        path.write_text(evaluation.encode_json(ref))
        scenario['reference']['sha256'] = evaluation.sha256_file(path)
        self.path.write_text(evaluation.encode_json(self.study))
        with self.assertRaisesRegex(RuntimeError, 'differs from measurement rubric'):
            evaluation.validate_study(self.path)

    def test_review_cannot_be_removed_from_reviewed_study(self):
        del self.study['referenceReview']
        self.path.write_text(evaluation.encode_json(self.study))
        with self.assertRaisesRegex(RuntimeError, 'requires its measurement rubric'):
            evaluation.validate_study(self.path)

    def test_changed_measurements_and_factual_assertions_are_rejected(self):
        _, raw, ref = self.case(6)
        rubric = evaluation.load_study_artifact(self.base, self.study['referenceReview'])
        changed = copy.deepcopy(raw)
        changed['rows'][4]['delta']['receiver_loss_packet_delta'] = 100
        with self.assertRaisesRegex(RuntimeError, 'measurements differ'):
            review.reviewed_reference(rubric, changed, ref)
        rubric['scenarios'][0]['assertions'][0]['values'][3] = 100
        with self.assertRaisesRegex(RuntimeError, 'assertion failed'):
            review.reviewed_reference(rubric, raw, ref)

    def test_unsupported_attribution_and_false_onset_are_penalized(self):
        for number in (900, 901):
            _, _, ref = self.case(number)
            correct = dict(ref, confidence=3, notes='synthetic unit test')
            score = evaluation.score_task(correct, ref)
            self.assertTrue(score['failureDomainCorrect'])
            wrong = dict(correct, failureDomain='exact-network-hop', supportedHypotheses=['sender-side-pressure'])
            score = evaluation.score_task(wrong, ref)
            self.assertFalse(score['failureDomainCorrect'])
            self.assertEqual(score['unsupportedHypotheses'], ['sender-side-pressure'])
            wrong['firstDeviationSample'] = 99
            self.assertFalse(evaluation.score_task(wrong, ref)['firstDeviationCorrect'])

    def test_rubric_has_no_human_signoff_claim_and_source_hashes_hold(self):
        rubric = evaluation.read_json(PACKET / 'rubric.json')
        self.assertEqual(rubric['humanReview'], 'pending')
        for case in rubric['scenarios']:
            source = case['sourceManifest']
            evaluation.validate_digest(ROOT / source['file'], source['sha256'], 'rubric source')

    def test_historical_readiness_remains_valid(self):
        old = ROOT / 'qualification/operator-evaluation/five-class-internal-rehearsal-v1'
        evaluation.verify_rehearsal_readiness(old / 'study.json', old / 'rehearsal-readiness.json')
        evaluation.verify_rehearsal_readiness(PACKET / 'study/study.json', PACKET / 'study/rehearsal-readiness.json')


if __name__ == '__main__':
    unittest.main()
