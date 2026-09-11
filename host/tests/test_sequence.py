from __future__ import annotations

import json
import unittest

from hidbot.client import Client
from hidbot.errors import ProtocolError
from hidbot.protocol import (
    HID_SEQUENCE_CAPABILITY,
    build_sequence_start_frame,
    build_sequence_status_frame,
    validate_sequence_handle,
    validate_sequence_status,
)
from hidbot.sequence import MouseButton, Sequence, SequenceBuilder


TOKEN = "0123456789abcdef0123456789abcdef"


class RetryTransport:
    def __init__(self) -> None:
        self.writes: list[bytes] = []
        self.response_unmatched = False
        self.response = b""

    def write(self, data: bytes) -> None:
        self.writes.append(bytes(data))
        if len(self.writes) == 2:
            request = json.loads(data[len(b"@HIDBOT ") : -1])
            payload = {
                "type": "response",
                "v": 1,
                "id": request["id"],
                "session": request["session"],
                "ok": True,
                "result": {"sequence_id": request["id"], "state": "accepted"},
            }
            self.response = b"@HIDBOT " + json.dumps(
                payload, separators=(",", ":")
            ).encode("ascii") + b"\n"

    def read(self, max_bytes: int, timeout: float) -> bytes:
        del max_bytes, timeout
        result, self.response = self.response, b""
        return result

    def close(self) -> None:
        pass


class AdvancingClock:
    def __init__(self) -> None:
        self.value = 0.0

    def now(self) -> float:
        self.value += 0.01
        return self.value

    def sleep(self, duration: float) -> None:
        self.value += duration


class StatusTransport:
    def __init__(self) -> None:
        self.response = b""

    def write(self, data: bytes) -> None:
        request = json.loads(data[len(b"@HIDBOT ") : -1])
        payload = {
            "type": "response",
            "v": 1,
            "id": request["id"],
            "session": request["session"],
            "ok": True,
            "result": {
                "sequence_id": request["params"]["sequence_id"],
                "state": "completed",
                "started": True,
                "executed": 1,
                "failed_token": None,
                "code": None,
            },
        }
        self.response = b"@HIDBOT " + json.dumps(
            payload, separators=(",", ":")
        ).encode("ascii") + b"\n"

    def read(self, max_bytes: int, timeout: float) -> bytes:
        del max_bytes, timeout
        result, self.response = self.response, b""
        return result

    def close(self) -> None:
        pass


class SequenceTests(unittest.TestCase):
    def test_builder_encodes_canonical_complete_grammar(self) -> None:
        sequence = (
            SequenceBuilder()
            .set_default_delay_ms(10)
            .key_press(4)
            .key_release(4)
            .wait_ms(200)
            .mouse_press(MouseButton.LEFT)
            .mouse_release(MouseButton.LEFT)
            .build()
        )
        self.assertEqual(sequence.encode(), "d10;kp4;kr4;w200;mpL;mrL")
        self.assertIn(b'"code":"d10;kp4;kr4;w200;mpL;mrL"',
                      build_sequence_start_frame(3, TOKEN, sequence))
        self.assertIn(b'"sequence_id":3', build_sequence_status_frame(4, TOKEN, 3))

    def test_builder_bounds_and_modifier_mouse_vocabulary(self) -> None:
        builder = SequenceBuilder()
        for _ in range(43):
            builder.key_press(231)
        for _ in range(21):
            builder.set_default_delay_ms(0)
        self.assertEqual(len(builder.build().encode()), 320)
        with self.assertRaises(ProtocolError):
            builder.wait_ms(0)
        for invalid in (-1, 3, 165, 222, 232, True):
            with self.assertRaises(ProtocolError):
                SequenceBuilder().key_press(invalid)
        with self.assertRaises(ProtocolError):
            SequenceBuilder().wait_ms(2001)
        with self.assertRaises(ProtocolError):
            SequenceBuilder().wait_ms(2000).wait_ms(501).build()
        with self.assertRaises(ProtocolError):
            SequenceBuilder().mouse_press("L")  # type: ignore[arg-type]
        for usage in range(224, 232):
            SequenceBuilder().key_press(usage).key_release(usage).build()
        for button in MouseButton:
            SequenceBuilder().mouse_press(button).mouse_release(button).build()
        with self.assertRaises(ProtocolError):
            Sequence(())

    def test_result_validation_is_exact(self) -> None:
        self.assertEqual(
            validate_sequence_handle({"sequence_id": 8, "state": "accepted"}).sequence_id,
            8,
        )
        status = validate_sequence_status(
            {
                "sequence_id": 8,
                "state": "aborted",
                "started": True,
                "executed": 2,
                "failed_token": 2,
                "code": "SEQUENCE_ABORTED",
            }
        )
        self.assertEqual(status.executed, 2)
        for invalid in (
            {},
            {"sequence_id": 8, "state": "completed", "started": True,
             "executed": 2, "failed_token": 2, "code": None},
            {"sequence_id": 8, "state": "failed", "started": True,
             "executed": 2, "failed_token": None, "code": "SEQUENCE_TIMEOUT"},
        ):
            with self.assertRaises(ProtocolError):
                validate_sequence_status(invalid)

    def test_status_validation_rejects_wrong_types_and_lifecycle_conflicts(self) -> None:
        valid = {
            "sequence_id": 8,
            "state": "completed",
            "started": True,
            "executed": 2,
            "failed_token": None,
            "code": None,
        }
        invalid_overrides = (
            {"state": []},
            {"started": 1},
            {"executed": -1},
            {"executed": 1.0},
            {"executed": True},
            {"failed_token": []},
            {"failed_token": -1},
            {"failed_token": 64},
            {"code": []},
            {"code": "UNKNOWN"},
            {"state": "accepted", "started": True, "executed": 0},
            {"state": "accepted", "started": False, "executed": 1},
            {"state": "running", "started": False, "executed": 0},
            {"state": "running", "started": True, "failed_token": 0,
             "code": "SEQUENCE_TIMEOUT"},
            {"state": "completed", "started": False, "executed": 2},
            {"state": "completed", "executed": 0},
            {"state": "completed", "failed_token": 2,
             "code": "SEQUENCE_TIMEOUT"},
            {"state": "failed", "failed_token": None,
             "code": "SEQUENCE_TIMEOUT"},
            {"state": "failed", "failed_token": 1,
             "code": "SEQUENCE_TIMEOUT"},
            {"state": "failed", "failed_token": 2,
             "code": "SEQUENCE_ABORTED"},
            {"state": "aborted", "failed_token": 2,
             "code": "SEQUENCE_TIMEOUT"},
            {"state": "aborted", "started": False, "failed_token": 2,
             "code": "SEQUENCE_ABORTED"},
        )
        for override in invalid_overrides:
            with self.subTest(override=override):
                malformed = dict(valid)
                malformed.update(override)
                with self.assertRaises(ProtocolError):
                    validate_sequence_status(malformed)

    def test_client_exact_transport_retry_reuses_one_start_id_and_frame(self) -> None:
        transport = RetryTransport()
        clock = AdvancingClock()
        client = Client(transport, timeout=0.05, max_attempts=2,
                        clock=clock.now, sleeper=clock.sleep)
        client._session = TOKEN
        client._capabilities = (HID_SEQUENCE_CAPABILITY,)
        sequence = SequenceBuilder().key_press(4).build()
        handle = client.sequence_start(sequence)
        self.assertEqual(handle.sequence_id, 0)
        self.assertEqual(len(transport.writes), 2)
        self.assertEqual(transport.writes[0], transport.writes[1])

    def test_client_sequence_status_is_typed(self) -> None:
        client = Client(StatusTransport())
        client._session = TOKEN
        client._capabilities = (HID_SEQUENCE_CAPABILITY,)
        status = client.sequence_status(9)
        self.assertEqual(status.sequence_id, 9)
        self.assertEqual(status.state, "completed")
        self.assertEqual(status.executed, 1)


if __name__ == "__main__":
    unittest.main()
