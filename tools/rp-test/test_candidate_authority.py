"""Exact v0.4.0 candidate and authority-v5 binding regressions."""
from pathlib import Path
import sys
import unittest

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(ROOT / "tools" / "qualification_campaign"))

import authority_runtime as authority
import official_campaign as campaign
import official_preflight as preflight


class CandidateAuthorityTests(unittest.TestCase):
    def test_exact_candidate_identity_is_bound_everywhere(self):
        self.assertEqual(authority.PRODUCT, {
            "version": "0.4.0",
            "commit": "137d489d3d1219b203f84633cf8b570d9fe9f19a",
            "tree": "929f22a9510e2b0e0ca36cb943796ef7532940e7",
            "firmware": "26819663473a44fa18538bc754f0f188e92b5a1c",
            "host": "9cd778f8cb96fa9a22b8c8be628d3a9f9f0f4953",
            "archive": "2e6b5a1a20a83e20cfeafa2d10835ddb605f4aff0319225b48e340c856b39a35",
            "bin": "8b0a2e4aed27d12ad3644c1e4d02c121890f3d3066075fb7e23acdfdaffd4315",
            "elf": "0a0ac1ded06b1dddce6d014fc1127f9a49f65c23dd289ae89929c4dc41c16c45",
        })
        self.assertEqual(campaign.EXPECTED_COMMIT, authority.PRODUCT["commit"])
        self.assertEqual(campaign.EXPECTED_ARCHIVE, authority.PRODUCT["archive"])
        self.assertEqual(preflight.EXPECTED_SOURCE, authority.PRODUCT["commit"])
        self.assertEqual(preflight.EXPECTED_ARCHIVE, authority.PRODUCT["archive"])
        self.assertEqual(preflight.EXPECTED_ELF, authority.PRODUCT["elf"])

    def test_v5_has_separate_install_and_state_roots(self):
        self.assertEqual(authority.INSTALL, Path("/usr/local/lib/s3-hidbot-authority-v5"))
        self.assertEqual(authority.STATE, Path("/var/lib/s3-hidbot-authority-v5"))


if __name__ == "__main__":
    unittest.main()
