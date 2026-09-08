#!/usr/bin/env python3
import copy
import errno
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import flight_recorder as fr

ROOT = Path(__file__).resolve().parents[1]
LOSS = ROOT / 'testdata/golden-incidents/incident-006-jsonl-constant-loss-robotweax-haivision'
PRESSURE = ROOT / 'testdata/golden-incidents/incident-010-receiver-pause-robotweax-haivision'
MARGIN = ROOT / 'testdata/golden-incidents/incident-014-recovery-margin-robotweax-haivision'


def fixture(directory=PRESSURE, private=False):
    return fr.make_case([directory / 'sender.jsonl', directory / 'receiver.jsonl'],
                        'retained-capture', True, 3, directory / 'manifest.json', private=private)


class CaseTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.out = Path(self.tmp.name) / 'case.frcase'

    def roundtrip(self, case):
        fr.write_case(case, self.out)
        return fr.assess_case(fr.read_case(self.out))

    def test_retained_classes_reuse_existing_analysis(self):
        for directory, expected in [(LOSS, 'transmission-path-loss-with-recovery'),
                                     (PRESSURE, 'receiver-side-pressure'),
                                     (MARGIN, 'receiver-recovery-margin-exhausted')]:
            with self.subTest(directory=directory.name):
                self.out.unlink(missing_ok=True)
                result = self.roundtrip(fixture(directory))
                self.assertEqual(result['analysis']['selectedHypothesis']['id'], expected)

    def test_roundtrip_preserves_all_samples_availability_and_timestamps(self):
        case = fixture()
        result = self.roundtrip(case)
        self.assertEqual(case['recordings'], result['recordings'])
        self.assertEqual(len(result['timeline']), 18)
        self.assertIsNone(result['timeline'][0]['delta']['receiver_loss_packet_delta'])

    def test_independent_clocks_never_imply_pairing(self):
        case = fr.make_case([PRESSURE / 'sender.jsonl', PRESSURE / 'receiver.jsonl'], 'retained-capture')
        result = self.roundtrip(case)
        self.assertEqual(result['assessment']['status'], 'insufficient-evidence')
        self.assertEqual(result['assessment']['timePolicy'], 'independent-clock-domains')

    def test_pairing_requires_recording_digest_proof(self):
        manifest = json.loads((PRESSURE / 'manifest.json').read_text())
        manifest['recordings']['sender']['sha256'] = '0' * 64
        source = Path(self.tmp.name) / 'source.json'
        source.write_text(json.dumps(manifest))
        with self.assertRaises(fr.CaseError):
            fr.make_case([PRESSURE / 'sender.jsonl', PRESSURE / 'receiver.jsonl'], 'retained-capture', True, source_manifest=source)

    def test_missing_endpoint_yields_explicit_insufficiency(self):
        case = fr.make_case([LOSS / 'sender.jsonl'], 'synthetic-fixture', True, 3, LOSS / 'manifest.json')
        self.assertEqual(self.roundtrip(case)['assessment']['status'], 'insufficient-evidence')

    def test_missing_metric_not_zero(self):
        case = fixture(LOSS)
        for sample in list(case['recordings'].values())[1]['samples']:
            sample['delta']['receiver_loss_packet_delta'] = None
            sample['delta']['available_delta_fields'] &= ~(1 << 5)
        self.assertEqual(self.roundtrip(case)['assessment']['status'], 'insufficient-evidence')

    def test_contradictory_drop_does_not_claim_recoverable_loss(self):
        case = fixture(LOSS)
        receiver = list(case['recordings'].values())[1]
        receiver['samples'][3]['delta']['receiver_drop_packet_delta'] = 1
        for sample in receiver['samples'][3:]:
            sample['raw']['receiver_drop_packet_total'] = 1
        result = self.roundtrip(case)
        self.assertEqual(result['assessment']['status'], 'insufficient-evidence')
        loss = next(h for h in result['analysis']['hypotheses'] if h['id'] == 'transmission-path-loss-with-recovery')
        self.assertTrue(loss['contradictoryEvidence'])

    def test_reset_prevents_global_diagnosis(self):
        case = fixture()
        list(case['recordings'].values())[0]['samples'][4]['delta']['quality_flags'] |= 8
        result = self.roundtrip(case)
        self.assertEqual(result['assessment']['status'], 'insufficient-evidence')
        self.assertIn('counter-reset', result['timeline'][4]['quality'])

    def test_time_offset_cannot_be_accepted_as_logical_pairing(self):
        case = fixture()
        for sample in list(case['recordings'].values())[1]['samples']:
            sample['raw']['sample_monotonic_nano'] += 1000000000000
        self.assertEqual(self.roundtrip(case)['assessment']['status'], 'insufficient-evidence')

    def test_wall_clock_jump_visible(self):
        case = fixture()
        list(case['recordings'].values())[0]['samples'][4]['raw']['observed_at_unix_nano'] += 9000000000
        result = self.roundtrip(case)
        self.assertIn('wall-clock-jump', result['timeline'][4]['quality'])
        self.assertEqual(result['assessment']['status'], 'insufficient-evidence')

    def test_counter_provider_change_boundary(self):
        case = fixture()
        list(case['recordings'].values())[0]['samples'][4]['raw']['provider'] = 'haivision-srt'
        self.assertEqual(self.roundtrip(case)['assessment']['status'], 'insufficient-evidence')

    def test_unequal_counts_abstain(self):
        case = fixture()
        rec = list(case['recordings'].values())[1]
        rec['samples'].pop()
        rec['header']['recordCount'] -= 1
        self.assertEqual(self.roundtrip(case)['assessment']['status'], 'insufficient-evidence')

    def test_reconnect_epochs_are_not_stitched(self):
        case = fixture()
        case['manifest']['sessions'][0]['endpoints'][1]['connectionEpoch'] = 2
        self.assertEqual(self.roundtrip(case)['assessment']['status'], 'insufficient-evidence')

    def test_archive_corruption_and_unknown_version_rejected(self):
        fr.write_case(fixture(), self.out)
        with zipfile.ZipFile(self.out) as archive:
            files = {name: archive.read(name) for name in archive.namelist()}
        files['recordings/endpoint-1.jsonl'] += b'\n'
        self.write_zip(files)
        with self.assertRaises(fr.CaseError):
            fr.read_case(self.out)
        self.out.unlink()
        fr.write_case(fixture(), self.out)
        with zipfile.ZipFile(self.out) as archive:
            files = {name: archive.read(name) for name in archive.namelist()}
        m = json.loads(files['manifest.json']); m['formatVersion'] = '99'
        files['manifest.json'] = json.dumps(m).encode()
        self.write_zip(files)
        with self.assertRaises(fr.CaseError):
            fr.read_case(self.out)

    def write_zip(self, files):
        with zipfile.ZipFile(self.out, 'w') as archive:
            for name, data in files.items():
                archive.writestr(name, data)

    def test_traversal_duplicate_and_zip_bomb_rejected(self):
        for name in ('../outside', '/tmp/outside', 'report.html'):
            self.write_zip({'manifest.json': b'{}', name: b'x'})
            with self.assertRaises(fr.CaseError):
                fr.read_case(self.out)
        with zipfile.ZipFile(self.out, 'w', compression=zipfile.ZIP_DEFLATED) as archive:
            archive.writestr('manifest.json', '{}')
            archive.writestr('recordings/endpoint-1.jsonl', b'x' * (fr.MAX_FILE_BYTES + 1))
        with self.assertRaises(fr.CaseError):
            fr.read_case(self.out)

    def test_nonfinite_unknown_and_inconsistent_fields_rejected(self):
        recording = fixture()['recordings']['recordings/endpoint-1.jsonl']
        for mutation in ('mask', 'unknown', 'negative'):
            altered = copy.deepcopy(recording)
            if mutation == 'mask':
                altered['samples'][1]['raw']['available_fields'] = 0
            elif mutation == 'unknown':
                altered['samples'][1]['raw']['secret'] = 'bad'
            else:
                altered['samples'][1]['raw']['sent_packet_total'] = -1
            with self.assertRaises(fr.CaseError):
                fr.parse_recording(fr.recording_bytes(altered))
        for data in (b'{"x":NaN}', b'{"x":1,"x":2}'):
            with self.assertRaises(fr.CaseError):
                fr.strict_json(data)

    def test_support_redaction_removes_identity_and_build_secrets(self):
        case = fixture(private=True)
        rec = list(case['recordings'].values())[0]
        old_id = rec['header']['streamId']
        rec['header']['providerVersion'] = '1.2.3-password'
        rec['header']['providerRevision'] = 'password'
        rec['header']['buildProfile'] = 'srt://user:password@10.0.0.1?streamid=customer'
        result = self.roundtrip(fr.redact_case(case))
        all_bytes = self.out.read_bytes()
        for secret in (b'password', b'10.0.0.1', old_id.encode(), b'incident-010'):
            self.assertNotIn(secret, all_bytes)
        self.assertEqual(result['analysis']['selectedHypothesis']['id'], 'receiver-side-pressure')
        self.assertEqual(result['manifest']['privacy']['mode'], 'support')

    def test_existing_file_never_overwritten(self):
        self.out.write_bytes(b'preserve')
        with self.assertRaises(fr.CaseError):
            fr.write_case(fixture(), self.out)
        self.assertEqual(self.out.read_bytes(), b'preserve')

    def test_disk_full_and_write_error_leave_no_final_or_temp_file(self):
        for code in (errno.ENOSPC, errno.EIO):
            def broken(stream):
                stream.write(b'partial')
                raise OSError(code, 'injected storage failure')
            with self.assertRaises(OSError):
                fr.atomic_write(self.out, broken)
            self.assertFalse(self.out.exists())
            self.assertEqual(list(Path(self.tmp.name).iterdir()), [])

    def test_sample_and_event_resource_caps(self):
        data = (LOSS / 'sender.jsonl').read_bytes()
        with mock.patch.object(fr, 'MAX_SAMPLES', 2):
            with self.assertRaises(fr.CaseError):
                fr.parse_recording(data)
        case = fixture()
        case['manifest']['events'] *= 129
        with self.assertRaises(fr.CaseError):
            fr.write_case(case, self.out)

    def test_manifest_freeform_credentials_rejected(self):
        case = fixture()
        case['manifest']['events'][0]['credentials'] = 'password'
        with self.assertRaises(fr.CaseError):
            fr.write_case(case, self.out)


if __name__ == '__main__':
    unittest.main()
