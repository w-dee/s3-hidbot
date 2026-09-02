from __future__ import annotations

import json
import unittest

from hidbot.errors import CompatibilityError, ProtocolError
from hidbot.protocol import (
    BASELINE_REQUIRED_CAPABILITIES,
    CompatibilityResult,
    BleExposureDesired,
    BleExposureObserved,
    FirmwareIdentity,
    OutputRoute,
    HelloResponse,
    OPTIONAL_CAPABILITIES,
    REQUIRED_CAPABILITIES,
    SystemInfo,
    build_command_frame,
    build_hid_route_set_frame,
    build_keyboard_report_frame,
    build_mouse_report_frame,
    build_hello_frame,
    parse_response,
    validate_release_all_result,
    validate_ble_exposure_status,
    validate_hid_route_status,
    validate_keyboard_report_result,
    validate_mouse_report_result,
    validate_usb_exposure_status,
    validate_hello_response,
    validate_system_info,
    evaluate_compatibility,
)


TOKEN = "0123456789abcdef0123456789abcdef"
OTHER_TOKEN = "fedcba9876543210fedcba9876543210"


def frame_payload(value: object) -> bytes:
    return json.dumps(value, separators=(",", ":")).encode("ascii")


class ProtocolTests(unittest.TestCase):
    def test_ble_exposure_status_is_exact_and_strict(self) -> None:
        status = validate_ble_exposure_status(
            {
                "desired": "exposed",
                "observed": "advertising",
                "generation": 0xFFFF_FFFF,
                "stack_ready": True,
                "advertising": True,
                "connected": False,
                "recovery_required": False,
                "last_error": None,
            }
        )
        self.assertEqual(status.desired, BleExposureDesired.EXPOSED)
        self.assertEqual(status.observed, BleExposureObserved.ADVERTISING)
        for invalid in (
            {},
            {**status.__dict__, "address": "private"},
            {**status.__dict__, "observed": "pairing"},
            {**status.__dict__, "generation": -1},
            {**status.__dict__, "advertising": True, "connected": True},
            {**status.__dict__, "last_error": {"operation": "pair", "code": 1}},
        ):
            with self.assertRaises(ProtocolError):
                validate_ble_exposure_status(invalid)

    def test_hid_route_builder_and_strict_status(self) -> None:
        self.assertEqual(
            build_hid_route_set_frame(3, TOKEN, OutputRoute.USB),
            b'@HIDBOT {"v":1,"id":3,"session":"0123456789abcdef0123456789abcdef",'
            b'"cmd":"hid.route.set","params":{"route":"usb"}}\n',
        )
        parsed = validate_hid_route_status(
            {
                "desired": "none",
                "active": "usb",
                "generation": 0xFFFF_FFFF,
                "transition": "releasing",
                "ready": False,
            }
        )
        self.assertEqual(parsed.desired, OutputRoute.NONE)
        self.assertEqual(parsed.active, OutputRoute.USB)
        for invalid in (
            {},
            {"desired": "ble", "active": "none", "generation": 0,
             "transition": "stable", "ready": False},
            {"desired": "none", "active": "usb", "generation": 0,
             "transition": "stable", "ready": False},
            {"desired": "none", "active": "none", "generation": -1,
             "transition": "stable", "ready": False},
            {"desired": "none", "active": "none", "generation": 0,
             "transition": "stable", "ready": True},
        ):
            with self.assertRaises(ProtocolError):
                validate_hid_route_status(invalid)
        with self.assertRaises(ProtocolError):
            build_hid_route_set_frame(1, TOKEN, "ble")  # type: ignore[arg-type]

    def test_keyboard_report_builder_and_strict_result(self) -> None:
        frame = build_keyboard_report_frame(3, TOKEN, 2, [4, 5, 0xA4, 0xB0, 0xDD])
        self.assertEqual(
            frame,
            b'@HIDBOT {"v":1,"id":3,"session":"0123456789abcdef0123456789abcdef",'
            b'"cmd":"hid.keyboard.report","params":{"modifiers":2,"keys":[4,5,164,176,221]}}\n',
        )
        self.assertEqual(validate_keyboard_report_result({"state": "submitted"}).state, "submitted")
        for modifiers in (-1, 256, True, 1.5):
            with self.assertRaises(ProtocolError):
                build_keyboard_report_frame(1, TOKEN, modifiers, [])
        for keys in ([4, 4], [5, 4], [0], [1], [3], [0xA5], [0xDE], [0xE0], list(range(4, 11))):
            with self.assertRaises(ProtocolError):
                build_keyboard_report_frame(1, TOKEN, 0, keys)
        with self.assertRaises(ProtocolError):
            validate_keyboard_report_result({"state": "queued"})

    def test_mouse_report_builder_ranges_and_strict_result(self) -> None:
        frame = build_mouse_report_frame(3, TOKEN, 3, 1, -2, 0, 4)
        self.assertEqual(
            frame,
            b'@HIDBOT {"v":1,"id":3,"session":"0123456789abcdef0123456789abcdef",'
            b'"cmd":"hid.mouse.report","params":{"buttons":3,"x":1,"y":-2,"wheel":0,"pan":4}}\n',
        )
        build_mouse_report_frame(4, TOKEN, 0, -127, 127, -127, 127)
        build_mouse_report_frame(5, TOKEN, 31, 127, -127, 127, -127)
        self.assertEqual(validate_mouse_report_result({"state": "submitted"}).state, "submitted")
        for values in (
            (True, 0, 0, 0, 0),
            (-1, 0, 0, 0, 0),
            (32, 0, 0, 0, 0),
            (0, -128, 0, 0, 0),
            (0, 128, 0, 0, 0),
            (0, 0.5, 0, 0, 0),
            (0, 0, -128, 0, 0),
            (0, 0, 128, 0, 0),
            (0, 0, 0, -128, 0),
            (0, 0, 0, 128, 0),
            (0, 0, 0, True, 0),
            (0, 0, 0, 0, False),
        ):
            with self.assertRaises(ProtocolError):
                build_mouse_report_frame(1, TOKEN, *values)
        for value in (
            {},
            {"state": "queued"},
            {"state": "submitted", "extra": False},
        ):
            with self.assertRaises(ProtocolError):
                validate_mouse_report_result(value)

    def test_release_all_result_is_strict(self) -> None:
        result = validate_release_all_result({"keyboard": "already_up", "mouse": "submitted"})
        self.assertEqual(result.keyboard, "already_up")
        self.assertEqual(result.mouse, "submitted")
        for value in (
            {},
            {"keyboard": "already_up"},
            {"keyboard": "already_up", "mouse": "submitted", "extra": False},
            {"keyboard": "held", "mouse": "submitted"},
        ):
            with self.assertRaises(ProtocolError):
                validate_release_all_result(value)

    def test_usb_exposure_status_result_is_exact_and_bounded(self) -> None:
        value = {
            "desired": "hidden",
            "observed": "detaching",
            "generation": 0xFFFF_FFFF,
            "mounted": False,
            "suspended": False,
            "keyboard_ready": False,
            "mouse_ready": False,
            "safety_pending": True,
            "host_release_uncertain": True,
            "recovery_required": True,
            "last_error": {"operation": "uninstall", "code": -7},
        }
        parsed = validate_usb_exposure_status(value)
        self.assertEqual(parsed.desired, "hidden")
        self.assertEqual(parsed.observed, "detaching")
        self.assertEqual(parsed.generation, 0xFFFF_FFFF)
        self.assertIsNotNone(parsed.last_error)
        assert parsed.last_error is not None
        self.assertEqual(parsed.last_error.operation, "uninstall")
        self.assertEqual(parsed.last_error.code, -7)
        for invalid in (
            {},
            {**value, "generation": -1},
            {**value, "generation": 0x1_0000_0000},
            {**value, "mounted": 1},
            {**value, "observed": "connected"},
            {**value, "last_error": {"operation": "remove", "code": 1}},
            {**value, "last_error": {"operation": "install", "code": True}},
            {**value, "extra": False},
        ):
            with self.assertRaises(ProtocolError):
                validate_usb_exposure_status(invalid)

    def test_strict_response_and_hello_validation(self) -> None:
        response = parse_response(
            frame_payload(
                {
                    "type": "response",
                    "v": 1,
                    "id": 0,
                    "session": TOKEN,
                    "ok": True,
                    "result": {
                        "project": "s3-hidbot",
                        "protocol_version": 1,
                        "client_nonce": OTHER_TOKEN,
                        "boot_id": TOKEN,
                        "session": TOKEN,
                        "lease_ms": 5000,
                        "capabilities": sorted(REQUIRED_CAPABILITIES),
                    },
                }
            )
        )
        hello = validate_hello_response(response, expected_id=0, expected_nonce=OTHER_TOKEN)
        self.assertEqual(hello.session, TOKEN)
        self.assertEqual(set(hello.capabilities), REQUIRED_CAPABILITIES)

    def test_missing_required_fields_and_bool_id_are_rejected(self) -> None:
        with self.assertRaises(ProtocolError):
            parse_response(frame_payload({"type": "response", "v": 1, "session": None, "ok": True, "result": {}}))
        with self.assertRaises(ProtocolError):
            parse_response(
                frame_payload(
                    {
                        "type": "response",
                        "v": 1,
                        "id": True,
                        "session": None,
                        "ok": False,
                        "error": {"code": "X", "message": "x"},
                    }
                )
            )

    def test_duplicate_keys_and_nonfinite_numbers_are_rejected(self) -> None:
        with self.assertRaises(ProtocolError):
            parse_response(
                b'{"type":"response","v":1,"id":0,"id":1,"session":null,"ok":false,"error":{"code":"X","message":"x"}}'
            )
        with self.assertRaises(ProtocolError):
            parse_response(b'{"type":"response","v":1,"id":0,"session":null,"ok":true,"result":NaN}')

    def test_result_and_error_ok_flags_are_consistent(self) -> None:
        with self.assertRaises(ProtocolError):
            parse_response(
                b'{"type":"response","v":1,"id":0,"session":null,"ok":false,"result":{}}'
            )
        with self.assertRaises(ProtocolError):
            parse_response(
                b'{"type":"response","v":1,"id":0,"session":null,"ok":true,"error":{"code":"X","message":"x"}}'
            )

    def test_builders_are_compact_and_bounded(self) -> None:
        hello = build_hello_frame(0, TOKEN)
        command = build_command_frame(1, TOKEN, "system.ping")
        self.assertEqual(
            hello,
            b'@HIDBOT {"v":1,"id":0,"cmd":"protocol.hello","params":{"client_nonce":"0123456789abcdef0123456789abcdef"}}\n',
        )
        self.assertEqual(
            command,
            b'@HIDBOT {"v":1,"id":1,"session":"0123456789abcdef0123456789abcdef","cmd":"system.ping","params":{}}\n',
        )

    def test_hello_capability_and_correlation_fail_closed(self) -> None:
        base = {
            "type": "response",
            "v": 1,
            "id": 0,
            "session": TOKEN,
            "ok": True,
            "result": {
                "project": "s3-hidbot",
                "protocol_version": 1,
                "client_nonce": OTHER_TOKEN,
                "boot_id": TOKEN,
                "session": TOKEN,
                "lease_ms": 5000,
                "capabilities": sorted(REQUIRED_CAPABILITIES),
            },
        }
        base["result"]["capabilities"] = ["protocol.hello-v1"]
        with self.assertRaises(ProtocolError):
            validate_hello_response(
                parse_response(frame_payload(base)), expected_id=0, expected_nonce=OTHER_TOKEN
            )
        base["result"]["capabilities"] = sorted(REQUIRED_CAPABILITIES)
        base["result"]["lease_ms"] = 4999
        with self.assertRaises(ProtocolError):
            validate_hello_response(
                parse_response(frame_payload(base)), expected_id=0, expected_nonce=OTHER_TOKEN
            )

    def test_hello_accepts_baseline_without_optional_hid_capabilities(self) -> None:
        value = {
            "type": "response",
            "v": 1,
            "id": 0,
            "session": TOKEN,
            "ok": True,
            "result": {
                "project": "s3-hidbot",
                "protocol_version": 1,
                "client_nonce": OTHER_TOKEN,
                "boot_id": TOKEN,
                "session": TOKEN,
                "lease_ms": 5000,
                "capabilities": sorted(BASELINE_REQUIRED_CAPABILITIES),
            },
        }
        hello = validate_hello_response(
            parse_response(frame_payload(value)), expected_id=0, expected_nonce=OTHER_TOKEN
        )
        self.assertEqual(set(hello.capabilities), BASELINE_REQUIRED_CAPABILITIES)

    def test_hello_accepts_unknown_and_identity_optional_capabilities(self) -> None:
        value = {
            "type": "response",
            "v": 1,
            "id": 0,
            "session": TOKEN,
            "ok": True,
            "result": {
                "project": "s3-hidbot",
                "protocol_version": 1,
                "client_nonce": OTHER_TOKEN,
                "boot_id": TOKEN,
                "session": TOKEN,
                "lease_ms": 5000,
                "capabilities": sorted(
                    BASELINE_REQUIRED_CAPABILITIES
                    | {"firmware.identity-v1", "future.example-v1"}
                ),
            },
        }
        hello = validate_hello_response(
            parse_response(frame_payload(value)), expected_id=0, expected_nonce=OTHER_TOKEN
        )
        self.assertIn("future.example-v1", hello.capabilities)
        self.assertIn("firmware.identity-v1", hello.capabilities)

    def test_hello_missing_baseline_is_compatibility_error(self) -> None:
        value = {
            "type": "response",
            "v": 1,
            "id": 0,
            "session": TOKEN,
            "ok": True,
            "result": {
                "project": "s3-hidbot",
                "protocol_version": 1,
                "client_nonce": OTHER_TOKEN,
                "boot_id": TOKEN,
                "session": TOKEN,
                "lease_ms": 5000,
                "capabilities": sorted(BASELINE_REQUIRED_CAPABILITIES - {"usb.status-v1"}),
            },
        }
        with self.assertRaises(CompatibilityError):
            validate_hello_response(
                parse_response(frame_payload(value)), expected_id=0, expected_nonce=OTHER_TOKEN
            )

    def test_legacy_system_info_is_typed_without_identity(self) -> None:
        info = validate_system_info(
            {
                "project": "s3-hidbot",
                "target": "esp32s3",
                "idf_version": "v5.5.4",
                "protocol_version": 1,
            },
            capabilities=tuple(REQUIRED_CAPABILITIES),
        )
        self.assertIsInstance(info, SystemInfo)
        self.assertIsNone(info.firmware)

    def test_identity_system_info_is_strictly_typed(self) -> None:
        info = validate_system_info(
            {
                "project": "s3-hidbot",
                "target": "esp32s3",
                "idf_version": "v5.5.4",
                "protocol_version": 1,
                "firmware": {
                    "version": "0.1.0-dev",
                    "source_revision": None,
                    "app_elf_sha256": "a" * 64,
                    "build_profile": "freenove-fnk0085",
                },
            },
            capabilities=tuple(REQUIRED_CAPABILITIES) + ("firmware.identity-v1",),
        )
        self.assertIsInstance(info.firmware, FirmwareIdentity)
        assert info.firmware is not None
        self.assertEqual(info.firmware.version, "0.1.0-dev")

    def test_identity_capability_and_system_info_shape_must_agree(self) -> None:
        legacy = {
            "project": "s3-hidbot",
            "target": "esp32s3",
            "idf_version": "v5.5.4",
            "protocol_version": 1,
        }
        with self.assertRaises(CompatibilityError):
            validate_system_info(
                legacy,
                capabilities=tuple(REQUIRED_CAPABILITIES) + ("firmware.identity-v1",),
            )
        with self.assertRaises(CompatibilityError):
            validate_system_info(
                {**legacy, "firmware": {}}, capabilities=tuple(REQUIRED_CAPABILITIES)
            )

    def test_invalid_identity_is_compatibility_error(self) -> None:
        identity = {
            "version": "0.1.0-dev",
            "source_revision": None,
            "app_elf_sha256": "a" * 64,
            "build_profile": "freenove-fnk0085",
        }
        base = {
            "project": "s3-hidbot",
            "target": "esp32s3",
            "idf_version": "v5.5.4",
            "protocol_version": 1,
            "firmware": identity,
        }
        for field, bad_value in (
            ("version", "01.0.0"),
            ("source_revision", "A" * 40),
            ("app_elf_sha256", "a" * 63),
            ("build_profile", "Freenove_FNK0085"),
        ):
            invalid = {**base, "firmware": {**identity, field: bad_value}}
            with self.assertRaises(CompatibilityError):
                validate_system_info(
                    invalid,
                    capabilities=tuple(REQUIRED_CAPABILITIES) + ("firmware.identity-v1",),
                )

    def test_compatibility_evaluator_is_pure_and_deterministic(self) -> None:
        hello = HelloResponse(
            session=TOKEN,
            boot_id=TOKEN,
            client_nonce=OTHER_TOKEN,
            project="s3-hidbot",
            protocol_version=1,
            capabilities=tuple(sorted(REQUIRED_CAPABILITIES | {"future.example-v1"})),
            lease_ms=5000,
        )
        info = {
            "project": "s3-hidbot",
            "target": "esp32s3",
            "idf_version": "v5.5.4",
            "protocol_version": 1,
        }
        result = evaluate_compatibility(hello, info)
        self.assertIsInstance(result, CompatibilityResult)
        self.assertTrue(result.compatible)
        self.assertEqual(result.missing_baseline_capabilities, ())
        self.assertEqual(
            result.advertised_optional_capabilities,
            tuple(sorted(OPTIONAL_CAPABILITIES & REQUIRED_CAPABILITIES)),
        )
        self.assertFalse(result.identity_available)
        self.assertTrue(result.target_supported)

        identity_result = evaluate_compatibility(
            HelloResponse(
                **{
                    **hello.__dict__,
                    "capabilities": tuple(
                        sorted(REQUIRED_CAPABILITIES | {"firmware.identity-v1"})
                    ),
                }
            ),
            {
                **info,
                "firmware": {
                    "version": "1.2.3+build.7",
                    "source_revision": "b" * 40,
                    "app_elf_sha256": "c" * 64,
                    "build_profile": "freenove-fnk0085",
                },
            },
        )
        self.assertTrue(identity_result.compatible)
        self.assertTrue(identity_result.identity_available)
        self.assertIsNotNone(identity_result.firmware_identity)

        missing_hello = HelloResponse(
            **{
                **hello.__dict__,
                "capabilities": tuple(
                    sorted(BASELINE_REQUIRED_CAPABILITIES - {"system.info-v1"})
                ),
            }
        )
        missing_result = evaluate_compatibility(missing_hello, info)
        self.assertFalse(missing_result.compatible)
        self.assertEqual(missing_result.missing_baseline_capabilities, ("system.info-v1",))

        unsupported = evaluate_compatibility(
            hello, {**info, "target": "esp32c6"}
        )
        self.assertFalse(unsupported.compatible)
        self.assertFalse(unsupported.target_supported)


if __name__ == "__main__":
    unittest.main()
