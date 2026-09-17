from __future__ import annotations

import io
import json
import unittest

from hidbot.client import Client
from hidbot.cli import main
from hidbot.errors import CompatibilityError, ProtocolError
from hidbot.framing import TRANSPORT_SYNC
from hidbot.protocol import BASELINE_REQUIRED_CAPABILITIES, OPTIONAL_CAPABILITIES, validate_ble_led_status
from test_client import FakeTransport, NONCE, TOKEN, hello_response, response, request_object


class LedObservationTests(unittest.TestCase):
    def test_exact_bounded_observation(self):
        for mask in range(32):
            observed = validate_ble_led_status({"supported": True, "valid": True, "leds": mask})
            self.assertEqual(observed.leds, mask)
        for supported in (False, True):
            self.assertFalse(validate_ble_led_status({"supported": supported, "valid": False, "leds": 0}).valid)
        base = {"supported": True, "valid": True, "leds": 3}
        for invalid in (None, [], {}, {**base, "extra": 1}, {**base, "supported": 1},
                        {**base, "valid": 0}, {**base, "leds": True}, {**base, "leds": -1},
                        {**base, "leds": 32}, {**base, "supported": False}, {**base, "valid": False}):
            with self.subTest(value=invalid), self.assertRaises(ProtocolError):
                validate_ble_led_status(invalid)

    def test_typed_client_and_cli_do_only_observation(self):
        for cli in (False, True):
            commands = []
            def on_write(transport, data):
                if data == TRANSPORT_SYNC:
                    return
                req = request_object(data)
                commands.append(req["cmd"])
                if req["cmd"] == "protocol.hello":
                    transport.chunks.append(hello_response(req["id"], req["params"]["client_nonce"],
                        capabilities=sorted(BASELINE_REQUIRED_CAPABILITIES | OPTIONAL_CAPABILITIES)))
                else:
                    self.assertEqual(req["cmd"], "ble.led.status")
                    self.assertEqual(req["params"], {})
                    transport.chunks.append(response(req["id"], TOKEN, result={"supported": True, "valid": True, "leds": 21}))
            transport = FakeTransport(on_write)
            if cli:
                transport.open = lambda: None
                output = io.StringIO()
                self.assertEqual(main(["--json", "--port", "fake", "ble-led-status"], environ={},
                    transport_factory=lambda *a, **k: transport, output=output), 0)
                self.assertEqual(json.loads(output.getvalue()), {"supported": True, "valid": True, "leds": 21})
            else:
                client = Client(transport, nonce_factory=lambda: NONCE)
                client.connect()
                self.assertEqual(client.ble_led_status().leds, 21)
            self.assertEqual(commands, ["protocol.hello", "ble.led.status"])

    def test_capability_missing_sends_no_command(self):
        def on_write(transport, data):
            if data != TRANSPORT_SYNC:
                req = request_object(data)
                transport.chunks.append(hello_response(req["id"], NONCE))
        transport = FakeTransport(on_write)
        client = Client(transport, nonce_factory=lambda: NONCE)
        client.connect()
        before = len(transport.writes)
        with self.assertRaises(CompatibilityError):
            client.ble_led_status()
        self.assertEqual(len(transport.writes), before)
