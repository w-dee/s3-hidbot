"""Coordinator ordering and complete phase-journal regressions."""
from pathlib import Path
import inspect
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "qualification_campaign"))
import evidence_contract as c
import official_campaign as coordinator


class OfficialCoordinatorTests(unittest.TestCase):
    def test_preflight_failure_cannot_create_attempt(self):
        with tempfile.TemporaryDirectory() as temp, \
                mock.patch.object(coordinator, "sha", return_value=coordinator.EXPECTED_ARCHIVE), \
                mock.patch.object(coordinator, "normalized_preflight",
                                  side_effect=c.EvidenceError("OFFICIAL_PREFLIGHT_FAILED")), \
                mock.patch.object(coordinator, "create_package") as create:
            with self.assertRaisesRegex(c.EvidenceError, "OFFICIAL_PREFLIGHT_FAILED"):
                coordinator.run(Path(temp) / "artifact", Path(temp))
            create.assert_not_called()

    def test_package_creation_follows_normalized_preflight(self):
        source = inspect.getsource(coordinator.run)
        self.assertLess(source.index("normalized_preflight"), source.index("create_package"))
        self.assertLess(source.index("create_package"), source.index("QUALIFICATION_ATTEMPT_START"))

    def test_phase_journal_starts_with_exact_not_executed_vocabulary(self):
        source = inspect.getsource(coordinator.run)
        self.assertIn("'status': 'NOT EXECUTED'", source)
        self.assertIn("details['phases'][index]['status'] = 'RUNNING'", source)
        self.assertIn("status='PASS' if", source)
        self.assertEqual([name for name, _ in coordinator.PHASES], [
            "q1_strict", "q2_just_works", "q3_keyboard", "q4_id7", "q5_led",
            "q6_metadata", "q7_sleep", "q8_host_security", "q9_lifecycle",
            "q10_usb", "q11_cache",
        ])


if __name__ == "__main__":
    unittest.main()
