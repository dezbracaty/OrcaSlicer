"""Regression tests for false-positive entry audits, independent of slicer code."""
import contextlib
import io
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import compare_legacy_landing as checker


def entry(x=10, current=True, extra='', dwell='G4 P0', powered=True):
    return ('G1 X{0} Y10 Z2.2 F21000\n'.format(x)
            + (';FIBER_BEGIN v=3\n' if current else '')
            + 'G1 E23.5 F600\n'+extra
            + 'G1 X{} Y10 Z.2 F180\n'.format(x+1)+dwell+'\n'
            + ('G1 X{} Y10 E1 F600\n'.format(x+2) if powered else '')
            + (';FIBER_END\n' if current else ''))


PREFIX = 'G90\nM83\n;LAYER_CHANGE\n;TYPE:Fiber wall\n'


class EntryComparisonTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)

    def write(self, text, name='entry.gcode'):
        path = self.root/name
        path.write_text(text)
        return path

    def failures(self, text, legacy=False, contract=None):
        rows = checker.parse(self.write(PREFIX+text), legacy, contract)
        return rows, checker.audit(rows, contract)

    def test_valid_without_source_metadata(self):
        rows, failures = self.failures(entry())
        self.assertEqual(len(rows), 1)
        self.assertFalse(failures)
        self.assertEqual(checker.metadata_audit(rows)['checked'], 0)

    def test_small_extra_feed_is_never_ignored(self):
        rows, failures = self.failures(entry(extra='G1 E.1 F600\n'))
        self.assertEqual(rows[0]['extra_e'][0]['E'], .1)
        self.assertIn('additional_E_only_command', failures[0]['issues'])

    def test_duplicate_prefeed_does_not_reset_legacy_entry(self):
        rows, failures = self.failures(entry(current=False, extra='G1 E23.5 F600\n'), True)
        self.assertEqual(len(rows), 1)
        self.assertIn('additional_E_only_command', failures[0]['issues'])

    def test_ordinary_unretract_after_completed_legacy_entry_is_not_a_new_entry(self):
        rows, failures = self.failures(entry(current=False)+'G1 E.8 F1800\nG1 X40 E1\n', True)
        self.assertEqual(len(rows), 1)
        self.assertFalse(failures)

    def test_cut_between_prefeed_and_powered_move_fails(self):
        _, failures = self.failures(entry(extra='M400\nS0\nM400\n'))
        self.assertIn('cut_inside_entry', failures[0]['issues'])

    def test_retract_or_zero_e_word_is_an_extra_e_command(self):
        for value in ('-.1', '0'):
            with self.subTest(value=value):
                _, failures = self.failures(entry(extra='G1 E'+value+'\n'))
                self.assertIn('additional_E_only_command', failures[0]['issues'])

    def test_absolute_e_and_reset_are_resolved(self):
        text = (PREFIX+'M82\nG92 E100\n'+entry()
                .replace('E23.5', 'E123.5').replace('E1 F600', 'E124.5 F600'))
        rows = checker.parse(self.write(text), False)
        self.assertFalse(checker.audit(rows))
        self.assertEqual(rows[0]['first_powered']['E'], 1)

    def test_unfinished_entries_fail_at_eof_and_end_marker(self):
        for ending in ('', ';FIBER_END\n'):
            rows, failures = self.failures(entry(powered=False).replace(';FIBER_END\n', ending))
            self.assertEqual(len(rows), 1)
            self.assertIn('incomplete_entry', failures[0]['issues'])

    def test_a_new_entry_does_not_hide_an_unfinished_one(self):
        rows, failures = self.failures(entry(powered=False).replace(';FIBER_END\n', '')+entry(20))
        self.assertEqual(len(rows), 2)
        self.assertEqual(len(failures), 1)

    def test_zero_e_on_descent_is_rejected_even_with_zero_delta(self):
        _, failures = self.failures(entry().replace('Z.2 F180', 'Z.2 E0 F180'))
        self.assertIn('landing_must_be_G1_without_E_word', failures[0]['issues'])

    def test_dwell_before_descent_is_rejected(self):
        text = entry(dwell='').replace('G1 X11', 'G4 P0\nG1 X11')
        _, failures = self.failures(text)
        self.assertIn('prefeed_landing_dwell_powered_order', failures[0]['issues'])

    def test_dwell_seconds_are_converted(self):
        _, failures = self.failures(entry(dwell='G4 S0'))
        self.assertFalse(failures)

    def test_relative_xyz_descent_is_resolved(self):
        text = entry().replace('G1 X11 Y10 Z.2 F180', 'G91\nG1 X1 Z-2 F180\nG90')
        _, failures = self.failures(text)
        self.assertFalse(failures)

    def test_no_hop_means_powered_first_edge_then_dwell(self):
        contract = dict(checker.DEFAULT_CONTRACT, hop_mm=0.)
        text = (';FIBER_BEGIN\nG1 E23.5 F600\nG1 X1 E1 F600\nG4 P0\nG1 X2 E1 F600\n;FIBER_END\n')
        rows, failures = self.failures(text, contract=contract)
        self.assertFalse(failures)
        self.assertEqual(rows[0]['landing'], [])
        self.assertIn('first_post_dwell_powered', rows[0])

    def test_different_coordinates_pass_contract_but_fail_strict(self):
        standard = self.write(PREFIX+entry(current=False), 'old.gcode')
        current = self.write(PREFIX+entry(20), 'new.gcode')
        report = checker.compare(standard, current)
        self.assertTrue(report['entry_contract_passed'])
        self.assertFalse(report['strict_match_passed'])
        self.assertEqual(report['matching']['unmatched_standard'], [0])
        with contextlib.redirect_stdout(io.StringIO()):
            with patch('sys.argv', ['compare', str(standard), str(current), str(self.root/'strict.json')]):
                self.assertEqual(checker.main(), 1)
            with patch('sys.argv', ['compare', str(standard), str(current), str(self.root/'contract.json'), '--mode', 'contract']):
                self.assertEqual(checker.main(), 0)

    def test_unique_correspondence_does_not_use_file_order(self):
        old = self.write(PREFIX+entry(current=False)+entry(20, current=False), 'old.gcode')
        new = self.write(PREFIX+entry(20)+entry(), 'new.gcode')
        report = checker.compare(old, new)
        self.assertTrue(report['strict_match_passed'])
        self.assertEqual(report['matching']['pairs'][0]['current_index'], 1)

    def test_two_legacy_exports_can_be_compared_explicitly(self):
        old = self.write(PREFIX+entry(current=False), 'old.gcode')
        new = self.write(PREFIX+entry(current=False), 'new.gcode')
        self.assertTrue(checker.compare(old, new, current_legacy=True)['strict_match_passed'])

    def test_repeated_seam_is_ambiguous_instead_of_greedily_matched(self):
        old = self.write(PREFIX+entry(current=False)+entry(current=False), 'old.gcode')
        new = self.write(PREFIX+entry()+entry(), 'new.gcode')
        report = checker.compare(old, new)
        self.assertFalse(report['strict_match_passed'])
        self.assertEqual(report['matching']['ambiguous_standard'], [0, 1])

    def test_metadata_cannot_make_a_different_command_pass(self):
        old = self.write(PREFIX+entry(current=False), 'old.gcode')
        new = self.write(PREFIX+entry().replace('G1 E23.5', ';CF_ENTRY start_x=10 start_y=10 first_x=11 first_y=10 source_station=1\nG1 E23.5')
                         .replace('X11 Y10 Z.2', 'X11.5 Y10 Z.2'), 'new.gcode')
        report = checker.compare(old, new)
        self.assertTrue(report['entry_contract_passed'])
        self.assertFalse(report['strict_match_passed'])
        self.assertTrue(report['metadata_consistency']['failures'])


if __name__ == '__main__':
    unittest.main()
