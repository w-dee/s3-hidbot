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
MOUSE = {"id": "standalone_mouse_just_works", "rev": 1, "schema": 2,
         "map": "c2fb165ffe3f84fc4160b013e15914dffbdecbc330c3c315051dc6922262e924",
         "bond": 1, "identity": 0}
KEYBOARD = {"id": "standalone_keyboard", "rev": 1, "schema": 3, "map": "d56a8aa0efc3f4126a0aea1df4c6f6f1b5bc1d624a710159c34b1cdb02bc45ae", "bond": 2, "identity": 0}
ID7 = {"id": "standalone_mouse_just_works_id7", "rev": 1, "schema": 4, "map": "7e06b773bb36dea83e1f0f76d9b49c46256d1a21d70183154ca4186e610ef628", "bond": 3, "identity": 0}
LEDS = {"id": "standalone_keyboard_leds", "rev": 1, "schema": 5, "map": "bc08d79cc45991446b3f46b37b23e6c82a86a3ad9450f1fe680e4e1134fd504d", "bond": 4, "identity": 0}
METADATA = {**MOUSE, "id": "mouse_metadata", "schema": 6, "bond": 5}
SLEEP = {**MOUSE, "id": "mouse_simulated_sleep_v1", "schema": 7, "bond": 6}
HOST_SECURITY = {**MOUSE, "id": "mouse_host_initiated_security", "schema": 8, "bond": 7}
STATUS = {"selected": "strict_composite", "active": None, "transition": "stable"}


def compact_catalog(*profiles):
    maps = list(dict.fromkeys(profile["map"] for profile in profiles))
    return {
        "fields": ["id", "rev", "schema", "map", "bond", "identity"],
        "maps": maps,
        "profiles": [[profile["id"], profile["rev"], profile["schema"],
                      maps.index(profile["map"]), profile["bond"],
                      profile["identity"]] for profile in profiles],
    }


class ProfileTests(unittest.TestCase):
    def test_exact_catalog_and_status(self):
        catalog = validate_ble_profile_list(compact_catalog(PROFILE))
        self.assertEqual(catalog[0].profile_id, BleProfileId.STRICT_COMPOSITE)
        self.assertEqual(catalog[0].report_map_sha256, PROFILE["map"])
        self.assertIsNone(validate_ble_profile_status(STATUS).active)
        catalog = validate_ble_profile_list(compact_catalog(PROFILE, MOUSE))
        self.assertEqual(catalog[1].profile_id, BleProfileId.STANDALONE_MOUSE_JUST_WORKS)
        self.assertEqual(catalog[1].bond_class, 1)
        mouse_status = {"selected": MOUSE["id"], "active": MOUSE["id"], "transition": "stable"}
        self.assertEqual(validate_ble_profile_status(mouse_status).active, BleProfileId.STANDALONE_MOUSE_JUST_WORKS)
        catalog = validate_ble_profile_list(compact_catalog(PROFILE, MOUSE, KEYBOARD))
        self.assertEqual(catalog[2].profile_id, BleProfileId.STANDALONE_KEYBOARD)
        self.assertEqual(catalog[2].bond_class, 2)
        self.assertIn(b'standalone_keyboard', build_ble_profile_select_frame(9, TOKEN, KEYBOARD["id"]))
        catalog = validate_ble_profile_list(compact_catalog(PROFILE, MOUSE, KEYBOARD, ID7))
        self.assertEqual(catalog[3].profile_id, BleProfileId.STANDALONE_MOUSE_JUST_WORKS_ID7)
        self.assertEqual(catalog[3].bond_class, 3)
        self.assertEqual(request_object(build_ble_profile_select_frame(10, TOKEN, ID7["id"]))["params"], {"profile": ID7["id"]})
        self.assertEqual(validate_ble_profile_status({"selected": ID7["id"], "active": ID7["id"], "transition": "stable"}).active, BleProfileId.STANDALONE_MOUSE_JUST_WORKS_ID7)
        catalog = validate_ble_profile_list(compact_catalog(PROFILE, MOUSE, KEYBOARD, ID7, LEDS))
        self.assertEqual(catalog[4].profile_id, BleProfileId.STANDALONE_KEYBOARD_LEDS)
        self.assertEqual(catalog[4].bond_class, 4)
        catalog = validate_ble_profile_list(compact_catalog(PROFILE, MOUSE, KEYBOARD, ID7, LEDS, METADATA))
        self.assertEqual(catalog[5].profile_id, BleProfileId.MOUSE_METADATA)
        self.assertEqual(catalog[5].bond_class, 5)
        self.assertEqual(request_object(build_ble_profile_select_frame(11, TOKEN, METADATA["id"]))["params"], {"profile": "mouse_metadata"})
        catalog = validate_ble_profile_list(compact_catalog(PROFILE, MOUSE, KEYBOARD, ID7, LEDS, METADATA, SLEEP))
        self.assertEqual(catalog[6].profile_id, BleProfileId.MOUSE_SIMULATED_SLEEP_V1)
        self.assertEqual(catalog[6].bond_class, 6)
        catalog = validate_ble_profile_list(compact_catalog(PROFILE, MOUSE, KEYBOARD, ID7, LEDS, METADATA, SLEEP, HOST_SECURITY))
        self.assertEqual(catalog[7].profile_id, BleProfileId.MOUSE_HOST_INITIATED_SECURITY)
        self.assertEqual(catalog[7].bond_class, 7)
        self.assertEqual(request_object(build_ble_profile_select_frame(12, TOKEN, HOST_SECURITY["id"]))["params"], {"profile": HOST_SECURITY["id"]})
        for item in ({**STATUS, "active": "strict_composite"},
                     {**STATUS, "transition": "initializing"},
                     {**STATUS, "transition": "fault"}):
            validate_ble_profile_status(item)

    def test_exact_finite_validation_rejects_unreviewed_values(self):
        valid = compact_catalog(PROFILE)
        bad_rows = [
            ["custom", 1, 1, 0, 0, 0],
            [PROFILE["id"], True, 1, 0, 0, 0],
            [PROFILE["id"], 1, 0, 0, 0, 0],
            [PROFILE["id"], 1, 1, 1, 0, 0],
            [PROFILE["id"], 1, 1, 0, 8, 0],
            [PROFILE["id"], 1, 1, 0, 0, True],
            [PROFILE["id"], 1, 1, 0, 0, 0, "upload"],
        ]
        for row in bad_rows:
            with self.subTest(row=row), self.assertRaises(ProtocolError):
                validate_ble_profile_list({**valid, "profiles": [row]})
        for item in (
            {**valid, "profiles": []},
            {**valid, "profiles": valid["profiles"] * 2},
            {**valid, "maps": ["a" * 63]},
            {**valid, "maps": valid["maps"] * 2},
            {**valid, "fields": ["id"]},
            {**valid, "extra": 1},
        ):
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
        mouse = build_ble_profile_select_frame(8, TOKEN, BleProfileId.STANDALONE_MOUSE_JUST_WORKS)
        self.assertEqual(request_object(mouse)["params"], {"profile": MOUSE["id"]})
        for bad in ("unknown", "strict_composite\x00suffix", 0, None):
            with self.assertRaises(ProtocolError):
                build_ble_profile_select_frame(7, TOKEN, bad)

    def test_client_negotiation_and_exact_retry(self):
        capabilities = sorted(BASELINE_REQUIRED_CAPABILITIES | OPTIONAL_CAPABILITIES)
        self.assertEqual(len(capabilities), 18)
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
            result = compact_catalog(PROFILE) if command == "ble.profile.list" else STATUS
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
                     ["ble-profile-select", "strict_composite"],
                     ["ble-profile-select", MOUSE["id"]],
                     ["ble-profile-select", KEYBOARD["id"]]):
            commands = []
            def on_write(transport, data):
                if data == TRANSPORT_SYNC: return
                req = request_object(data)
                commands.append(req["cmd"])
                if req["cmd"] == "protocol.hello":
                    transport.chunks.append(hello_response(req["id"], req["params"]["client_nonce"],
                        capabilities=sorted(BASELINE_REQUIRED_CAPABILITIES | OPTIONAL_CAPABILITIES)))
                else:
                    value = compact_catalog(PROFILE) if req["cmd"] == "ble.profile.list" else STATUS
                    transport.chunks.append(response(req["id"], TOKEN, result=value))
            transport = FakeTransport(on_write)
            transport.open = lambda: None
            output = io.StringIO()
            result = main(["--json", "--port", "fake", *args], environ={},
                          transport_factory=lambda *a, **k: transport, output=output)
            self.assertEqual(result, 0)
            self.assertIsInstance(json.loads(output.getvalue()), dict)
            self.assertEqual(commands, ["protocol.hello", args[0].replace("-", ".")])
