from __future__ import annotations

import json
import unittest

from hidbot.errors import ProtocolError
from hidbot.protocol import (
    REQUIRED_CAPABILITIES,
    build_command_frame,
    build_hello_frame,
    parse_response,
    validate_release_all_result,
    validate_hello_response,
)


TOKEN = "0123456789abcdef0123456789abcdef"
OTHER_TOKEN = "fedcba9876543210fedcba9876543210"


def frame_payload(value: object) -> bytes:
    return json.dumps(value, separators=(",", ":")).encode("ascii")


class ProtocolTests(unittest.TestCase):
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


if __name__ == "__main__":
    unittest.main()
