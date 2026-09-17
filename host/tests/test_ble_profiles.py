from __future__ import annotations

import unittest

from hidbot.client import Client
from hidbot.errors import CompatibilityError, ProtocolError
from hidbot.framing import TRANSPORT_SYNC, FRAME_PREFIX
from hidbot.protocol import (
    BASELINE_REQUIRED_CAPABILITIES, OPTIONAL_CAPABILITIES,
    BleProfileId,
    build_ble_profile_select_frame, parse_response,
    validate_ble_profile_list, validate_ble_profile_status,
)
from test_client import FakeClock, FakeTransport, NONCE, TOKEN, hello_response, response, request_object

PROFILE = {"id": "strict_composite", "rev": 1, "schema": 1,
           "map": "ef1be45d8fe7d0637568c8954b64bab971d5b5f57bf3d44f1cc040e8fe5c3d32",
           "bond": 0, "identity": 0}
STATUS = {"selected": "strict_composite", "active": None, "transition": "stable"}


class ProfileTests(unittest.TestCase):
    def test_exact_catalog_and_status(self):
        catalog = validate_ble_profile_list({"profiles": [PROFILE]})
        self.assertEqual(catalog[0].profile_id, BleProfileId.STRICT_COMPOSITE)
        self.assertEqual(catalog[0].report_map_sha256, PROFILE["map"])
        self.assertIsNone(validate_ble_profile_status(STATUS).active)
        for item in ({**STATUS, "active": "strict_composite"},
                     {**STATUS, "transition": "initializing"},
                     {**STATUS, "transition": "fault"}):
            validate_ble_profile_status(item)

    def test_exact_finite_validation_rejects_unreviewed_values(self):
        for key, value in [("id", "custom"), ("rev", True), ("schema", 0),
                           ("map", "a" * 63), ("bond", 1), ("identity", True),
                           ("upload", "bytes")]:
            with self.subTest(key=key), self.assertRaises(ProtocolError):
                validate_ble_profile_list({"profiles": [{**PROFILE, key: value}]})
        for item in ({"profiles": []}, {"profiles": [PROFILE, PROFILE]},
                     {"profiles": [PROFILE], "extra": 1}):
            with self.assertRaises(ProtocolError):
                validate_ble_profile_list(item)
        for item in ({**STATUS, "selected": "custom"}, {**STATUS, "active": 1},
                     {**STATUS, "transition": "rebooting"}, {**STATUS, "transition": []},
                     {**STATUS, "active": "strict_composite", "transition": "fault"},
                     {**STATUS, "unknown": None}):
            with self.assertRaises(ProtocolError):
                validate_ble_profile_status(item)

    def test_canonical_selection_encoding(self):
        frame = build_ble_profile_select_frame(7, TOKEN, BleProfileId.STRICT_COMPOSITE)
        self.assertEqual(request_object(frame), {"v": 1, "id": 7, "session": TOKEN,
            "cmd": "ble.profile.select", "params": {"profile": "strict_composite"}})
        for bad in ("unknown", "strict_composite\x00suffix", 0, None):
            with self.assertRaises(ProtocolError):
                build_ble_profile_select_frame(7, TOKEN, bad)

    def test_client_negotiation_and_exact_retry(self):
        capabilities = sorted(BASELINE_REQUIRED_CAPABILITIES | OPTIONAL_CAPABILITIES)
        self.assertEqual(len(capabilities), 17)
        selected_frames = []
        def on_write(transport, data):
            if data == TRANSPORT_SYNC: return
            req = request_object(data)
            command = req["cmd"]
            if command == "protocol.hello":
                transport.chunks.append(hello_response(req["id"], NONCE, capabilities=capabilities))
                return
            if command == "ble.profile.select":
                selected_frames.append(data)
                if len(selected_frames) == 1: return
            result = {"profiles": [PROFILE]} if command == "ble.profile.list" else STATUS
            transport.chunks.append(response(req["id"], TOKEN, result=result))
        transport = FakeTransport(on_write)
        clock = FakeClock()
        client = Client(transport, timeout=0.1, max_attempts=2, clock=clock.now,
                        sleeper=clock.sleep, nonce_factory=lambda: NONCE)
        client.connect()
        self.assertEqual(client.ble_profile_list()[0].revision, 1)
        self.assertEqual(client.ble_profile_status().selected, BleProfileId.STRICT_COMPOSITE)
        self.assertIsNone(client.ble_profile_select("strict_composite").active)
        self.assertEqual(selected_frames[0], selected_frames[1])
        self.assertEqual(client.session, TOKEN)

    def test_profile_api_requires_capability_without_mutation(self):
        def on_write(transport, data):
            if data != TRANSPORT_SYNC:
                req = request_object(data)
                transport.chunks.append(hello_response(req["id"], NONCE))
        transport = FakeTransport(on_write)
        client = Client(transport, nonce_factory=lambda: NONCE)
        client.connect()
        count = len(transport.writes)
        for call in (client.ble_profile_list, client.ble_profile_status,
                     lambda: client.ble_profile_select("strict_composite")):
            with self.assertRaises(CompatibilityError): call()
        self.assertEqual(len(transport.writes), count)

    def test_capability_bound_exception_does_not_expand_other_arrays(self):
        capabilities = sorted(BASELINE_REQUIRED_CAPABILITIES | OPTIONAL_CAPABILITIES)
        parse_response(hello_response(1, NONCE, capabilities=capabilities)[len(FRAME_PREFIX):-1])
        with self.assertRaises(ProtocolError):
            parse_response(hello_response(1, NONCE, capabilities=capabilities + ["extra-v1"])[len(FRAME_PREFIX):-1])
        with self.assertRaises(ProtocolError):
            parse_response(response(2, TOKEN, result={"other": list(range(17))})[len(FRAME_PREFIX):-1])

    def test_cli_profile_commands_use_typed_client_without_extra_mutation(self):
        import io
        import json
        from hidbot.cli import main
        for args in (["ble-profile-list"], ["ble-profile-status"],
                     ["ble-profile-select", "strict_composite"]):
            commands = []
            def on_write(transport, data):
                if data == TRANSPORT_SYNC: return
                req = request_object(data)
                commands.append(req["cmd"])
                if req["cmd"] == "protocol.hello":
                    transport.chunks.append(hello_response(req["id"], req["params"]["client_nonce"],
                        capabilities=sorted(BASELINE_REQUIRED_CAPABILITIES | OPTIONAL_CAPABILITIES)))
                else:
                    value = {"profiles": [PROFILE]} if req["cmd"] == "ble.profile.list" else STATUS
                    transport.chunks.append(response(req["id"], TOKEN, result=value))
            transport = FakeTransport(on_write)
            transport.open = lambda: None
            output = io.StringIO()
            result = main(["--json", "--port", "fake", *args], environ={},
                          transport_factory=lambda *a, **k: transport, output=output)
            self.assertEqual(result, 0)
            self.assertIsInstance(json.loads(output.getvalue()), dict)
            self.assertEqual(commands, ["protocol.hello", args[0].replace("-", ".")])
