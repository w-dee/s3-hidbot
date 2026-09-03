from __future__ import annotations

import json
import unittest
from collections import deque
from unittest.mock import patch

from hidbot.client import Client
from hidbot.errors import CompatibilityError, ProtocolError, RemoteError, RequestTimeoutError
from hidbot.framing import FRAME_PREFIX
from hidbot.protocol import (
    BLE_BOND_ADMINISTRATION_CAPABILITY,
    BLE_PAIRING_TRANSACTION_CAPABILITY,
    BleBondList,
    BleBondRemoveResult,
    BlePairingAction,
    BlePairingLastResult,
    BlePairingState,
    build_ble_bond_remove_frame,
    build_ble_pairing_respond_frame,
    validate_ble_bond_list,
    validate_ble_bond_remove_result,
    validate_ble_pairing_respond_result,
    validate_ble_pairing_status,
)


TOKEN = "0123456789abcdef0123456789abcdef"
SECRET = "000123"


def response(
    request_id: int,
    *,
    result: object | None = None,
    error: dict[str, str] | None = None,
) -> bytes:
    value: dict[str, object] = {
        "type": "response",
        "v": 1,
        "id": request_id,
        "session": TOKEN,
        "ok": error is None,
    }
    if error is None:
        value["result"] = result
    else:
        value["error"] = error
    return FRAME_PREFIX + json.dumps(value, separators=(",", ":")).encode("ascii") + b"\n"


def status_value(**updates: object) -> dict[str, object]:
    value: dict[str, object] = {
        "state": "idle",
        "generation": 0,
        "connected": False,
        "pairing_id": None,
        "action": None,
        "remaining_ms": None,
        "encrypted": False,
        "authenticated": False,
        "bonded": False,
        "secure_connections": False,
        "key_size": 0,
        "last_result": "none",
    }
    value.update(updates)
    return value


def bond_value(*ids: str, healthy: bool = True) -> dict[str, object]:
    return {
        "capacity": 3,
        "count": len(ids),
        "available": 3 - len(ids),
        "healthy": healthy,
        "bonds": [
            {
                "bond_id": bond_id,
                "our_sec": True,
                "peer_sec": True,
                "verified": True,
                "schema_revision": 2,
                "schema_current": True,
                "connected": False,
            }
            for bond_id in ids
        ],
    }


class Clock:
    def __init__(self) -> None:
        self.value = 0.0

    def now(self) -> float:
        return self.value

    def sleep(self, duration: float) -> None:
        self.value += duration


class Transport:
    def __init__(self, on_write=None) -> None:
        self.writes: list[bytes] = []
        self.chunks: deque[bytes] = deque()
        self.on_write = on_write

    def write(self, data: bytes) -> None:
        self.writes.append(data)
        if self.on_write is not None:
            self.on_write(self, data)

    def read(self, max_bytes: int, timeout: float) -> bytes:
        del max_bytes, timeout
        return self.chunks.popleft() if self.chunks else b""

    def close(self) -> None:
        return None


def client_for(transport: Transport, *, attempts: int = 2, logs=None) -> Client:
    clock = Clock()
    client = Client(
        transport,
        timeout=0.02,
        max_attempts=attempts,
        clock=clock.now,
        sleeper=clock.sleep,
        log_sink=logs.append if logs is not None else None,
    )
    client._session = TOKEN
    client._capabilities = (BLE_PAIRING_TRANSACTION_CAPABILITY,)
    return client


class PairingProtocolTests(unittest.TestCase):
    def test_status_states_last_results_and_types(self) -> None:
        idle = validate_ble_pairing_status(status_value())
        self.assertEqual(idle.state, BlePairingState.IDLE)
        securing = validate_ble_pairing_status(
            status_value(state="securing", generation=0xFFFF_FFFF, connected=True)
        )
        self.assertEqual(securing.state, BlePairingState.SECURING)
        waiting = validate_ble_pairing_status(
            status_value(
                state="waiting_input",
                generation=7,
                connected=True,
                pairing_id=0xFFFF_FFFF,
                action="passkey_input",
                remaining_ms=0,
            )
        )
        self.assertEqual(waiting.action, BlePairingAction.PASSKEY_INPUT)
        self.assertEqual(waiting.pairing_id, 0xFFFF_FFFF)
        for member in BlePairingLastResult:
            parsed = validate_ble_pairing_status(status_value(last_result=member.value))
            self.assertEqual(parsed.last_result, member)

    def test_status_rejects_schema_enum_integer_boolean_and_nullability_attacks(self) -> None:
        base = status_value()
        invalid = [
            {},
            {**base, "extra": 1},
            {key: value for key, value in base.items() if key != "key_size"},
            {**base, "state": "pairing"},
            {**base, "last_result": "failure"},
            {**base, "generation": True},
            {**base, "generation": -1},
            {**base, "generation": 0x1_0000_0000},
            {**base, "connected": 1},
            {**base, "encrypted": 0},
            {**base, "authenticated": 1},
            {**base, "bonded": 0},
            {**base, "secure_connections": 1},
            {**base, "key_size": True},
            {**base, "key_size": -1},
            {**base, "key_size": 17},
            {**base, "key_size": 1.0},
            {**base, "pairing_id": 1},
            {**base, "action": "passkey_input"},
            {**base, "remaining_ms": 0},
            status_value(
                state="waiting_input",
                pairing_id=None,
                action="passkey_input",
                remaining_ms=1,
            ),
            status_value(
                state="waiting_input", pairing_id=1, action=None, remaining_ms=1
            ),
            status_value(
                state="waiting_input",
                pairing_id=1,
                action="passkey_input",
                remaining_ms=None,
            ),
            status_value(
                state="waiting_input",
                pairing_id=True,
                action="passkey_input",
                remaining_ms=1,
            ),
            status_value(
                state="waiting_input",
                pairing_id=0,
                action="passkey_input",
                remaining_ms=1,
            ),
            status_value(
                state="waiting_input",
                pairing_id=0x1_0000_0000,
                action="passkey_input",
                remaining_ms=1,
            ),
            status_value(
                state="waiting_input",
                pairing_id=1,
                action="passkey_input",
                remaining_ms=True,
            ),
            status_value(
                state="waiting_input",
                pairing_id=1,
                action="passkey_input",
                remaining_ms=-1,
            ),
            status_value(
                state="waiting_input",
                pairing_id=1,
                action="passkey_input",
                remaining_ms=0x1_0000_0000,
            ),
            status_value(
                state="waiting_input",
                pairing_id=1,
                action="numeric",
                remaining_ms=1,
            ),
        ]
        for value in invalid:
            with self.subTest(value=value), self.assertRaises(ProtocolError):
                validate_ble_pairing_status(value)

    def test_respond_builder_and_result_are_exact(self) -> None:
        self.assertEqual(
            build_ble_pairing_respond_frame(4, TOKEN, 12, SECRET),
            b'@HIDBOT {"v":1,"id":4,"session":"0123456789abcdef0123456789abcdef",'
            b'"cmd":"ble.pairing.respond","params":{"pairing_id":12,"passkey":"000123"}}\n',
        )
        result = validate_ble_pairing_respond_result({"accepted": True, "pairing_id": 12})
        self.assertTrue(result.accepted)
        for value in (
            {},
            {"accepted": False, "pairing_id": 12},
            {"accepted": 1, "pairing_id": 12},
            {"accepted": True, "pairing_id": True},
            {"accepted": True, "pairing_id": 0},
            {"accepted": True, "pairing_id": 0x1_0000_0000},
            {"accepted": True, "pairing_id": 12, "extra": None},
        ):
            with self.assertRaises(ProtocolError):
                validate_ble_pairing_respond_result(value)


class PairingClientTests(unittest.TestCase):
    def test_status_and_respond_are_typed_and_canonical(self) -> None:
        def on_write(transport: Transport, data: bytes) -> None:
            request = json.loads(data[len(FRAME_PREFIX) : -1])
            if request["cmd"] == "ble.pairing.status":
                self.assertEqual(request["params"], {})
                transport.chunks.append(response(request["id"], result=status_value()))
            else:
                self.assertEqual(request["params"], {"pairing_id": 12, "passkey": SECRET})
                transport.chunks.append(
                    response(request["id"], result={"accepted": True, "pairing_id": 12})
                )

        client = client_for(Transport(on_write), attempts=1)
        self.assertEqual(client.ble_pairing_status().state, BlePairingState.IDLE)
        self.assertTrue(client.ble_pairing_respond(12, SECRET).accepted)

    def test_argument_rejection_and_missing_capability_are_zero_wire(self) -> None:
        transport = Transport()
        client = client_for(transport)
        invalid_ids = (True, 0, -1, 0x1_0000_0000, 1.5, "1")
        for pairing_id in invalid_ids:
            with self.assertRaises(ProtocolError):
                client.ble_pairing_respond(pairing_id, SECRET)  # type: ignore[arg-type]
        invalid_secrets = (
            123456,
            b"123456",
            True,
            "１２３４５６",
            " 12345",
            "123456 ",
            "+12345",
            "12345\n",
            "12345",
            "1234567",
            "12\x00345",
        )
        for passkey in invalid_secrets:
            with self.assertRaises(ProtocolError):
                client.ble_pairing_respond(1, passkey)  # type: ignore[arg-type]
        self.assertEqual(transport.writes, [])

        client._capabilities = ()
        with self.assertRaises(CompatibilityError):
            client.ble_pairing_status()
        with self.assertRaises(CompatibilityError):
            client.ble_pairing_respond(1, "000000")
        self.assertEqual(transport.writes, [])
        self.assertEqual(client._next_request_id, 0)

    def test_all_documented_passkeys_preserve_leading_zeroes(self) -> None:
        for passkey in ("000000", "000123", "999999"):
            with self.subTest(passkey=passkey):
                transport = Transport()

                def on_write(current: Transport, data: bytes) -> None:
                    request = json.loads(data[len(FRAME_PREFIX) : -1])
                    self.assertEqual(request["params"]["passkey"], passkey)
                    current.chunks.append(
                        response(
                            request["id"],
                            result={"accepted": True, "pairing_id": 8},
                        )
                    )

                transport.on_write = on_write
                result = client_for(transport, attempts=1).ble_pairing_respond(
                    8, passkey
                )
                self.assertTrue(result.accepted)

    def test_sensitive_retry_is_byte_identical_and_serialized_once(self) -> None:
        attempts = 0

        def on_write(transport: Transport, data: bytes) -> None:
            nonlocal attempts
            attempts += 1
            request = json.loads(data[len(FRAME_PREFIX) : -1])
            if attempts == 2:
                transport.chunks.append(
                    response(request["id"], result={"accepted": True, "pairing_id": 27})
                )

        transport = Transport(on_write)
        client = client_for(transport, attempts=2)
        from hidbot import protocol

        original = protocol._serialize_request
        with patch("hidbot.protocol._serialize_request", wraps=original) as serialize:
            self.assertTrue(client.ble_pairing_respond(27, SECRET).accepted)
        self.assertEqual(serialize.call_count, 1)
        self.assertEqual(transport.writes, [transport.writes[0], transport.writes[0]])
        self.assertFalse(hasattr(client, "_retry_frame"))

    def test_pairing_errors_and_timeout_are_redacted(self) -> None:
        for code in (
            "BLE_PAIRING_NOT_PENDING",
            "BLE_PAIRING_FAILED",
            "REQUEST_ID_CONFLICT",
        ):
            logs: list[bytes] = []

            def on_write(transport: Transport, data: bytes, code=code) -> None:
                request = json.loads(data[len(FRAME_PREFIX) : -1])
                transport.chunks.append(
                    response(
                        request["id"],
                        error={"code": code, "message": f"rejected {SECRET}"},
                    )
                )

            client = client_for(Transport(on_write), attempts=1, logs=logs)
            with self.assertRaises(RemoteError) as caught:
                client.ble_pairing_respond(1, SECRET)
            self.assertNotIn(SECRET, str(caught.exception))
            self.assertNotIn(SECRET, repr(caught.exception))
            self.assertNotIn(SECRET.encode(), b"".join(logs))

        transport = Transport()
        client = client_for(transport, attempts=1)
        with self.assertRaises(RequestTimeoutError) as caught:
            client.ble_pairing_respond(1, SECRET)
        self.assertNotIn(SECRET, str(caught.exception))
        self.assertNotIn(SECRET, repr(caught.exception))


class BondAdministrationTests(unittest.TestCase):
    def test_list_empty_one_three_and_strict_abnormal_states(self) -> None:
        empty = validate_ble_bond_list(bond_value())
        self.assertIsInstance(empty, BleBondList)
        self.assertEqual((empty.count, empty.available, empty.bonds), (0, 3, ()))
        one = validate_ble_bond_list(bond_value("1" * 32))
        self.assertEqual(one.bonds[0].bond_id, "1" * 32)
        three = validate_ble_bond_list(
            bond_value("0" * 32, "8" * 32, "f" * 32)
        )
        self.assertEqual([item.bond_id for item in three.bonds],
                         ["0" * 32, "8" * 32, "f" * 32])

        half = bond_value("2" * 32, healthy=False)
        half["bonds"][0]["peer_sec"] = False  # type: ignore[index]
        half["bonds"][0]["verified"] = False  # type: ignore[index]
        self.assertFalse(validate_ble_bond_list(half).healthy)

        invalid = [
            {**bond_value(), "capacity": 4},
            {**bond_value(), "available": 2},
            {**bond_value("1" * 32), "healthy": False},
            bond_value("f" * 32, "0" * 32),
            bond_value("0" * 32, "0" * 32),
            bond_value("A" * 32),
            {**bond_value(), "ltk": "secret"},
        ]
        incoherent = bond_value("3" * 32)
        incoherent["bonds"][0]["schema_revision"] = None  # type: ignore[index]
        invalid.append(incoherent)
        for value in invalid:
            with self.subTest(value=value), self.assertRaises(ProtocolError):
                validate_ble_bond_list(value)

    def test_remove_builder_result_and_client_are_exact_and_typed(self) -> None:
        selected = "0123456789abcdef0123456789abcdef"
        self.assertEqual(
            build_ble_bond_remove_frame(4, TOKEN, selected),
            b'@HIDBOT {"v":1,"id":4,"session":"0123456789abcdef0123456789abcdef",'
            b'"cmd":"ble.bond.remove","params":{"bond_id":"0123456789abcdef0123456789abcdef"}}\n',
        )
        parsed = validate_ble_bond_remove_result(
            {"bond_id": selected, "removed": True, "remaining": 2}
        )
        self.assertIsInstance(parsed, BleBondRemoveResult)

        def on_write(transport: Transport, data: bytes) -> None:
            request = json.loads(data[len(FRAME_PREFIX) : -1])
            if request["cmd"] == "ble.bond.list":
                transport.chunks.append(
                    response(request["id"], result=bond_value(selected))
                )
            else:
                self.assertEqual(request["params"], {"bond_id": selected})
                transport.chunks.append(
                    response(
                        request["id"],
                        result={"bond_id": selected, "removed": True, "remaining": 0},
                    )
                )

        transport = Transport(on_write)
        client = client_for(transport, attempts=1)
        client._capabilities = (BLE_BOND_ADMINISTRATION_CAPABILITY,)
        self.assertEqual(client.ble_bond_list().bonds[0].bond_id, selected)
        self.assertEqual(client.ble_bond_remove(selected).bond_id, selected)

    def test_remove_retry_is_byte_identical_and_invalid_or_missing_cap_is_zero_wire(self) -> None:
        selected = "a" * 32
        attempts = 0

        def on_write(transport: Transport, data: bytes) -> None:
            nonlocal attempts
            attempts += 1
            request = json.loads(data[len(FRAME_PREFIX) : -1])
            if attempts == 2:
                transport.chunks.append(
                    response(
                        request["id"],
                        result={"bond_id": selected, "removed": True, "remaining": 1},
                    )
                )

        transport = Transport(on_write)
        client = client_for(transport, attempts=2)
        client._capabilities = (BLE_BOND_ADMINISTRATION_CAPABILITY,)
        self.assertTrue(client.ble_bond_remove(selected).removed)
        self.assertEqual(transport.writes, [transport.writes[0], transport.writes[0]])

        invalid_transport = Transport()
        invalid = client_for(invalid_transport)
        invalid._capabilities = (BLE_BOND_ADMINISTRATION_CAPABILITY,)
        for value in ("", "0" * 31, "0" * 33, "A" * 32, 1, None):
            with self.assertRaises(ProtocolError):
                invalid.ble_bond_remove(value)  # type: ignore[arg-type]
        self.assertEqual(invalid_transport.writes, [])

        missing_transport = Transport()
        missing = client_for(missing_transport)
        missing._capabilities = ()
        with self.assertRaises(CompatibilityError):
            missing.ble_bond_list()
        with self.assertRaises(CompatibilityError):
            missing.ble_bond_remove(selected)
        self.assertEqual(missing_transport.writes, [])


if __name__ == "__main__":
    unittest.main()
