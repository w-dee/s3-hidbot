"""Host-only line-order tests for the canonical application reset helper."""

import unittest
from pathlib import Path

from common import InfraError
import serial_reset


class FakePort:
    def __init__(self, events, **values):
        self.events = events
        self.values = values
        self._dtr = None
        self._rts = None
        self._port = values['port']
        self.opened = False
        self.closed = False
        events.append(('construct', dict(values)))

    @property
    def dtr(self):
        return self._dtr

    @dtr.setter
    def dtr(self, value):
        self._dtr = value
        self.events.append(('dtr', value))

    @property
    def rts(self):
        return self._rts

    @rts.setter
    def rts(self, value):
        self._rts = value
        self.events.append(('rts', value))

    @property
    def port(self):
        return self._port

    @port.setter
    def port(self, value):
        self._port = value
        self.events.append(('port', value))

    def open(self):
        self.events.append(('open', self.dtr, self.rts, self.port))
        self.opened = True

    def close(self):
        self.events.append(('close', self.dtr, self.rts))
        self.closed = True


class FakeSerialModule:
    EIGHTBITS = 8
    PARITY_NONE = 'N'
    STOPBITS_ONE = 1

    def __init__(self, events, open_failure=False):
        self.events = events
        self.open_failure = open_failure
        self.instance = None

    def Serial(self, **values):
        self.instance = FakePort(self.events, **values)
        if self.open_failure:
            def fail():
                self.events.append(('open-failed',))
                raise OSError()
            self.instance.open = fail
        return self.instance


class ResetStrategy:
    def __init__(self, port, events, fail=False, **values):
        self.port = port
        self.events = events
        self.fail = fail
        self.events.append(('strategy', values))

    def reset(self):
        self.events.append(('reset-entry', self.port.dtr, self.port.rts))
        self.port.rts = True
        self.port.rts = False
        if self.fail:
            raise OSError()


class SerialResetTests(unittest.TestCase):
    def run_reset(self, *, reset_failure=False, open_failure=False):
        events = []
        module = FakeSerialModule(events, open_failure=open_failure)
        calls = []
        def factory(port, **values):
            calls.append(1)
            return ResetStrategy(port, events, fail=reset_failure, **values)
        if reset_failure or open_failure:
            with self.assertRaises(OSError):
                serial_reset.application_hard_reset_once(object(), module, factory)
        else:
            serial_reset.application_hard_reset_once(object(), module, factory)
        return events, module.instance, calls

    def test_01_lines_released_before_endpoint_and_open(self):
        events, _port, _calls = self.run_reset()
        names = [event[0] for event in events]
        self.assertLess(names.index('dtr'), names.index('port'))
        self.assertLess(names.index('rts'), names.index('port'))
        self.assertLess(names.index('port'), names.index('open'))
        opened = next(event for event in events if event[0] == 'open')
        self.assertEqual(opened[1:3], (False, False))

    def test_02_hard_reset_inherits_released_dtr(self):
        events, _port, calls = self.run_reset()
        self.assertEqual(calls, [1])
        self.assertIn(('reset-entry', False, False), events)
        self.assertIn(('strategy', {'uses_usb': False}), events)

    def test_03_exact_uart_configuration(self):
        events, _port, _calls = self.run_reset()
        values = next(event[1] for event in events if event[0] == 'construct')
        self.assertEqual(values, {
            'port': None, 'baudrate': 115200, 'bytesize': 8, 'parity': 'N',
            'stopbits': 1, 'timeout': 0.2, 'write_timeout': 1.0,
            'xonxoff': False, 'rtscts': False, 'dsrdtr': False,
            'exclusive': True,
        })

    def test_04_final_lines_remain_application_idle(self):
        _events, port, _calls = self.run_reset()
        self.assertEqual((port.dtr, port.rts, port.closed), (False, False, True))

    def test_05_reset_failure_has_no_retry_and_closes(self):
        events, port, calls = self.run_reset(reset_failure=True)
        self.assertEqual(calls, [1])
        self.assertEqual(sum(event[0] == 'reset-entry' for event in events), 1)
        self.assertEqual((port.dtr, port.rts, port.closed), (False, False, True))

    def test_06_open_failure_never_constructs_reset_strategy(self):
        _events, port, calls = self.run_reset(open_failure=True)
        self.assertEqual(calls, [])
        self.assertEqual((port.dtr, port.rts, port.closed), (False, False, True))

    def test_07_invalid_input_fails_before_serial_construction(self):
        events = []
        module = FakeSerialModule(events)
        with self.assertRaisesRegex(InfraError, 'APPLICATION_RESET_INPUT_INVALID'):
            serial_reset.application_hard_reset_once(None, module, lambda _p: None)
        self.assertEqual(events, [])

    def test_08_source_has_no_retry_or_physical_discovery(self):
        source = Path(serial_reset.__file__).read_text(encoding='utf-8')
        for forbidden in ('glob(', '/dev/serial', 'while ', 'for attempt', 'sleep('):
            self.assertNotIn(forbidden, source)


if __name__ == '__main__':
    unittest.main()
