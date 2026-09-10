"""Use the actual strict host Client over synthetic partial/stale byte streams."""
from collections import deque
import json
import os
from pathlib import Path
import sys
import unittest

source = Path(os.environ.get('S3_HIDBOT_HOST_SOURCE', Path(__file__).resolve().parents[2] / 'host/src'))
sys.path.insert(0, str(source))
from hidbot.client import Client
from hidbot.framing import FRAME_PREFIX, TRANSPORT_SYNC
from hidbot.protocol import BASELINE_REQUIRED_CAPABILITIES
from startup_probe import ClientPeer
from serial_readiness import SerialResolution, serial_readiness

class Clock:
    value = 0.0
    def now(self):
        return self.value
    def sleep(self, seconds):
        self.value += seconds

class Transport:
    def __init__(self, missing=False):
        self.chunks = deque([b'boot log\n@HIDBOT {partial', b'\n'])
        self.commands = []
        self.missing = missing
        self.closed = False

    def write(self, data):
        if data == TRANSPORT_SYNC:
            return
        request = json.loads(data[len(FRAME_PREFIX):])
        self.commands.append(request['cmd'])
        if self.missing:
            return
        session = 'a' * 32
        if request['cmd'] == 'protocol.hello':
            value = {'project': 's3-hidbot', 'protocol_version': 1, 'client_nonce': request['params']['client_nonce'],
                     'boot_id': 'b' * 32, 'session': session, 'lease_ms': 5000, 'capabilities': sorted(BASELINE_REQUIRED_CAPABILITIES)}
        else:
            value = {'synthetic_identity': True}
        response = {'type': 'response', 'v': 1, 'id': request['id'], 'session': session, 'ok': True, 'result': value}
        if request['cmd'] == 'protocol.hello':
            stale = dict(response, result=dict(value, client_nonce='c' * 32))
            self.chunks.append(FRAME_PREFIX + json.dumps(stale).encode() + b'\n')
        raw = FRAME_PREFIX + json.dumps(response).encode() + b'\n'
        self.chunks.extend((raw[:3], raw[3:17], raw[17:]))

    def read(self, max_bytes, timeout):
        return self.chunks.popleft() if self.chunks else b''

    def close(self):
        self.closed = True

class ActualClientTests(unittest.TestCase):
    def test_timeout_stale_partial_and_correlated_success(self):
        clock = Clock()
        transports = []
        def factory(_endpoint, remaining):
            transport = Transport(missing=not transports)
            transports.append(transport)
            return ClientPeer(Client(transport, timeout=min(.75, remaining), max_attempts=1,
                                     clock=clock.now, sleeper=clock.sleep))
        result = serial_readiness(lambda _: SerialResolution(True, True, object()), factory,
                                  lambda h, i: i == {'synthetic_identity': True}, timeout=3,
                                  interval=.3, clock=clock.now, sleeper=clock.sleep)
        self.assertEqual(result['classification'], 'IDENTITY_VERIFIED')
        self.assertEqual(len(transports), 2)
        self.assertTrue(all(t.closed for t in transports))
        self.assertEqual([c for t in transports for c in t.commands], ['protocol.hello', 'protocol.hello', 'system.info'])

if __name__ == '__main__':
    unittest.main()
