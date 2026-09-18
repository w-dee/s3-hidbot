"""Deterministic official preflight reset and start-gate tests."""
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "qualification_campaign"))
import official_preflight as preflight


def state(profile="strict_composite", **changes):
    value = {
        "identity_match": True,
        "boot_id": "post",
        "profile": profile,
        "profile_stable": True,
        "route_none": True,
        "all_up": True,
        "sequence_active": False,
        "pairing_idle": True,
        "ble_safe": True,
        "usb_safe": True,
        "host_safe": True,
        "bond_store_healthy": True,
        "bond_inventory": [{"bond_id": "opaque", "schema_revision": 8}],
    }
    value.update(changes)
    return value


class OfficialPreflightTests(unittest.TestCase):
    def run_case(self, initial_profile, final=None, reset=None):
        events = []
        initial = state(initial_profile, boot_id="initial")
        final = state() if final is None else final

        def read_initial():
            events.append("read_initial")
            return initial

        def reset_once():
            events.append("reset")
            if reset is not None:
                reset()

        def read_final():
            events.append("read_final")
            return final

        result = preflight.normalize_fixture(read_initial, reset_once, read_final)
        self.assertEqual(events, ["read_initial", "reset", "read_final"])
        self.assertEqual(result["reset_count"], 1)
        return result

    def test_p1_stale_host_security_profile_resets_to_strict(self):
        self.assertEqual(self.run_case("mouse_host_initiated_security")["result"], "PASS")

    def test_p2_every_other_finite_profile_resets_to_strict(self):
        profiles = (
            "standalone_mouse_just_works", "standalone_keyboard",
            "standalone_mouse_just_works_id7", "standalone_keyboard_leds",
            "mouse_metadata", "mouse_simulated_sleep_v1",
        )
        for profile in profiles:
            with self.subTest(profile=profile):
                self.assertEqual(self.run_case(profile)["result"], "PASS")

    def test_p3_already_strict_still_resets_once(self):
        self.assertEqual(self.run_case("strict_composite")["reset_count"], 1)

    def test_p4_reset_failure_prohibits_start(self):
        with self.assertRaisesRegex(OSError, "reset failed"):
            self.run_case("strict_composite", reset=lambda: (_ for _ in ()).throw(OSError("reset failed")))

    def test_p5_wrong_post_reset_profile_prohibits_start(self):
        with self.assertRaisesRegex(preflight.PreflightFailure, "POST_RESET_PROFILE_NOT_STRICT"):
            self.run_case("strict_composite", state("mouse_metadata"))

    def test_p6_post_reset_identity_mismatch_prohibits_start(self):
        with self.assertRaisesRegex(preflight.PreflightFailure, "POST_RESET_IDENTITY_MISMATCH"):
            self.run_case("strict_composite", state(identity_match=False))

    def test_p7_every_safety_debt_prohibits_start(self):
        cases = {
            "route_none": "POST_RESET_ROUTE_NOT_NONE",
            "profile_stable": "POST_RESET_PROFILE_NOT_STABLE",
            "all_up": "POST_RESET_NOT_ALL_UP",
            "sequence_active": "POST_RESET_SEQUENCE_ACTIVE",
            "pairing_idle": "POST_RESET_PAIRING_ACTIVE",
            "ble_safe": "POST_RESET_BLE_NOT_HIDDEN",
            "usb_safe": "POST_RESET_USB_NOT_HIDDEN",
            "host_safe": "POST_RESET_HOST_NOT_SAFE",
        }
        for field, code in cases.items():
            value = True if field == "sequence_active" else False
            with self.subTest(field=field), self.assertRaisesRegex(preflight.PreflightFailure, code):
                self.run_case("strict_composite", state(**{field: value}))

    def test_p8_incompatible_bond_inventory_is_preserved_and_allowed(self):
        result = self.run_case("mouse_host_initiated_security")
        self.assertTrue(result["bond_inventory_preserved"])
        self.assertEqual(result["post_reset"]["bond_inventory"][0]["schema_revision"], 8)

    def test_bond_inventory_change_during_reset_fails_closed(self):
        with self.assertRaisesRegex(preflight.PreflightFailure, "POST_RESET_BOND_INVENTORY_CHANGED"):
            self.run_case("strict_composite", state(bond_inventory=[]))

    def test_reset_must_produce_a_new_boot_id(self):
        with self.assertRaisesRegex(preflight.PreflightFailure, "APPLICATION_RESET_NOT_OBSERVED"):
            self.run_case("strict_composite", state(boot_id="initial"))

    def test_initial_identity_is_checked_before_reset(self):
        events = []
        initial = state(boot_id="initial", identity_match=False)
        with self.assertRaisesRegex(preflight.PreflightFailure, "INITIAL_IDENTITY_MISMATCH"):
            preflight.normalize_fixture(lambda: initial, lambda: events.append("reset"), state)
        self.assertEqual(events, [])


if __name__ == "__main__":
    unittest.main()
