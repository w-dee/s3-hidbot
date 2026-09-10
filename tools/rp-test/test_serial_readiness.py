"""Host-only serial identity/node/open/protocol tests, including real PTYs."""
import errno
import json
import os
from pathlib import Path
import pty
import tempfile
import unittest

import serial

import serial_readiness as subject
from startup_probe import MalformedRead, OperationBudget, RetryableRead


class Clock:
    def __init__(self):
        self.value = 0.0
    def __call__(self):
        return self.value
    def sleep(self, seconds):
        self.value += seconds


class Peer:
    def __init__(self, *, sync=None, hello=None, info='exact', handle=None):
        self.sync_result = sync
        self.hello_result = hello
        self.info_result = info
        self.handle = handle
        self.closed = False
    def sync(self, remaining):
        if isinstance(self.sync_result, BaseException):
            raise self.sync_result
    def hello(self, remaining):
        if isinstance(self.hello_result, BaseException):
            raise self.hello_result
        return 'hello' if self.hello_result is None else self.hello_result
    def info(self, remaining):
        if isinstance(self.info_result, BaseException):
            raise self.info_result
        return self.info_result
    def close(self):
        self.closed = True
        if self.handle is not None:
            self.handle.close()


class SerialReadinessTests(unittest.TestCase):
    def setUp(self):
        self.clock = Clock()

    def run_ready(self, resolver, opener, matches=lambda h, i: True, timeout=1):
        return subject.serial_readiness(resolver, opener, matches, timeout=timeout,
                                        interval=.1, clock=self.clock, sleeper=self.clock.sleep)

    def test_01_real_pty_present_and_open_immediately(self):
        master, slave = pty.openpty()
        name = os.ttyname(slave)
        try:
            result = self.run_ready(
                lambda _: subject.SerialResolution(True, True, name),
                lambda endpoint, remaining: Peer(handle=serial.Serial(endpoint, timeout=.05, exclusive=True)),
            )
            self.assertEqual(result['classification'], 'IDENTITY_VERIFIED')
            self.assertEqual(result['serial']['open_attempt_count'], 1)
        finally:
            os.close(master)
            os.close(slave)

    def test_02_alias_absent_then_appears(self):
        master, slave = pty.openpty()
        with tempfile.TemporaryDirectory() as directory:
            alias = Path(directory) / 'stable'
            calls = [0]
            def resolve(_):
                calls[0] += 1
                if calls[0] == 2:
                    alias.symlink_to(os.ttyname(slave))
                return subject.SerialResolution(alias.exists(), alias.exists(), alias if alias.exists() else None)
            result = self.run_ready(resolve, lambda endpoint, _: Peer(handle=serial.Serial(str(endpoint), exclusive=True)))
            self.assertEqual(result['classification'], 'IDENTITY_VERIFIED')
        os.close(master); os.close(slave)

    def test_03_alias_target_absent_then_target_appears(self):
        states = iter((subject.SerialResolution(True, False),
                       subject.SerialResolution(True, True, 'synthetic')))
        result = self.run_ready(lambda _: next(states), lambda endpoint, _: Peer())
        self.assertEqual(result['serial']['node_seen_count'], 1)

    def test_04_alias_retargets_after_reenumeration(self):
        endpoints = iter(('old', 'new'))
        opened = []
        def resolve(_):
            return subject.SerialResolution(True, True, next(endpoints))
        def open_peer(endpoint, _):
            opened.append(endpoint)
            if endpoint == 'old':
                raise FileNotFoundError(errno.ENOENT, 'private')
            return Peer()
        result = self.run_ready(resolve, open_peer)
        self.assertEqual(result['classification'], 'IDENTITY_VERIFIED')
        self.assertEqual(opened, ['old', 'new'])

    def test_05_first_open_transient_then_success(self):
        calls = [0]
        def opener(endpoint, remaining):
            calls[0] += 1
            if calls[0] == 1:
                raise OSError(errno.ENODEV, 'private')
            return Peer()
        result = self.run_ready(lambda _: subject.SerialResolution(True, True, object()), opener)
        self.assertEqual(result['classification'], 'IDENTITY_VERIFIED')
        self.assertEqual(result['serial']['open_attempt_count'], 2)

    def test_06_device_disappears_between_resolution_and_open(self):
        calls = [0]
        def opener(endpoint, remaining):
            calls[0] += 1
            if calls[0] == 1:
                raise FileNotFoundError(errno.ENOENT, 'private')
            return Peer()
        result = self.run_ready(lambda _: subject.SerialResolution(True, True, 'private'), opener)
        self.assertEqual(result['serial']['last_open_failure'], 'PATH_NOT_PRESENT')
        self.assertEqual(result['classification'], 'IDENTITY_VERIFIED')

    def test_07_port_disappears_during_sync_then_returns(self):
        peers = iter((Peer(sync=RetryableRead('gone')), Peer()))
        result = self.run_ready(lambda _: subject.SerialResolution(True, True, object()),
                                lambda endpoint, _: next(peers))
        self.assertEqual(result['classification'], 'IDENTITY_VERIFIED')
        self.assertEqual(result['serial']['open_success_count'], 2)

    def test_08_access_denied_is_terminal(self):
        result = self.run_ready(lambda _: subject.SerialResolution(True, True, object()),
                                lambda endpoint, _: (_ for _ in ()).throw(PermissionError(errno.EACCES, 'private')))
        self.assertEqual(result['classification'], 'SERIAL_OPEN_ACCESS_DENIED')
        self.assertFalse(result['serial']['open_failure_transient'])
        self.assertEqual(result['attempts'], 1)

    def test_09_identity_ambiguous_is_terminal(self):
        result = self.run_ready(
            lambda _: (_ for _ in ()).throw(subject.SerialIdentityFailure('IDENTITY_AMBIGUOUS')),
            lambda endpoint, _: self.fail('open'))
        self.assertEqual(result['classification'], 'IDENTITY_AMBIGUOUS')
        self.assertEqual(result['attempts'], 1)

    def test_10_busy_is_bounded_transient_then_success(self):
        calls = [0]
        def opener(endpoint, remaining):
            calls[0] += 1
            if calls[0] == 1:
                raise OSError(errno.EBUSY, 'private')
            return Peer()
        result = self.run_ready(lambda _: subject.SerialResolution(True, True, object()), opener)
        self.assertEqual(result['classification'], 'IDENTITY_VERIFIED')
        self.assertEqual(result['serial']['last_open_failure'], 'DEVICE_BUSY_TRANSIENT')
        self.assertTrue(result['serial']['open_failure_transient'])

    def test_11_first_hello_timeout_then_success(self):
        peers = iter((Peer(hello=RetryableRead('timeout')), Peer()))
        result = self.run_ready(lambda _: subject.SerialResolution(True, True, object()),
                                lambda endpoint, _: next(peers))
        self.assertEqual(result['classification'], 'IDENTITY_VERIFIED')
        self.assertEqual(result['serial']['protocol_attempt_count'], 2)

    def test_12_partial_frame_resynchronizes(self):
        peers = iter((Peer(hello=MalformedRead('partial')), Peer()))
        result = self.run_ready(lambda _: subject.SerialResolution(True, True, object()),
                                lambda endpoint, _: next(peers))
        self.assertEqual(result['classification'], 'IDENTITY_VERIFIED')

    def test_13_stale_frame_resynchronizes(self):
        peers = iter((Peer(hello=RetryableRead('stale')), Peer()))
        result = self.run_ready(lambda _: subject.SerialResolution(True, True, object()),
                                lambda endpoint, _: next(peers))
        self.assertIn('HELLO_READY', result['states'])

    def test_14_deadline_before_any_open(self):
        result = self.run_ready(lambda _: subject.SerialResolution(False, False),
                                lambda endpoint, _: self.fail('open'))
        self.assertEqual(result['classification'], 'SERIAL_IDENTITY_NOT_READY')
        self.assertEqual(result['serial']['open_attempt_count'], 0)

    def test_15_open_but_protocol_never_synchronizes(self):
        result = self.run_ready(lambda _: subject.SerialResolution(True, True, object()),
                                lambda endpoint, _: Peer(hello=RetryableRead('timeout')))
        self.assertEqual(result['classification'], 'PROTOCOL_READINESS_TIMEOUT')
        self.assertGreater(result['serial']['open_success_count'], 0)

    def test_16_identity_mismatch(self):
        result = self.run_ready(lambda _: subject.SerialResolution(True, True, object()),
                                lambda endpoint, _: Peer(), lambda h, i: False)
        self.assertEqual(result['classification'], 'IDENTITY_MISMATCH')
        self.assertEqual(result['attempts'], 1)

    def test_17_termio_and_serial_exception_are_terminal(self):
        termio = subject.classify_open_failure(OSError(errno.ENOTTY, 'private'))
        self.assertEqual((termio.category, termio.transient), ('INVALID_DEVICE_TYPE', False))
        error = serial.SerialException('private')
        serial_failure = subject.classify_open_failure(error)
        self.assertEqual((serial_failure.category, serial_failure.transient),
                         ('PYTHON_SERIAL_INTERNAL_ERROR', False))

    def test_18_diagnostics_do_not_serialize_endpoint_or_message(self):
        secret = '/' + 'private' + '/endpoint'
        result = self.run_ready(lambda _: subject.SerialResolution(True, True, secret),
                                lambda endpoint, _: (_ for _ in ()).throw(OSError(errno.ENODEV, secret)))
        encoded = json.dumps(result)
        self.assertNotIn(secret, encoded)
        self.assertEqual(result['serial']['open_exception_class'], 'OSError')

    def test_19_opportunistic_deadline_separate_and_reset_budget_two(self):
        clock = Clock()
        budget = OperationBudget(startup_limit=2)
        resets = []
        results = subject.startup_campaign(
            budget, lambda: resets.append(True),
            lambda _: subject.SerialResolution(False, False),
            lambda endpoint, _: self.fail('open'), lambda h, i: True,
            policy=subject.StartupPolicy(), interval=.3, clock=clock, sleeper=clock.sleep)
        self.assertEqual([item['deadline_ms'] for item in results], [2000, 12000, 12000])
        self.assertEqual(len(resets), 2)
        self.assertEqual(budget.startup_attempts, 2)

    def test_20_known_not_running_skips_opportunistic(self):
        clock = Clock()
        budget = OperationBudget(startup_limit=1)
        results = subject.startup_campaign(
            budget, lambda: None, lambda _: subject.SerialResolution(False, False),
            lambda endpoint, _: self.fail('open'), lambda h, i: True,
            policy=subject.StartupPolicy(start_state='application_not_running'),
            interval=.3, clock=clock, sleeper=clock.sleep)
        self.assertEqual(len(results), 1)
        self.assertEqual(results[0]['probe'], 'reset')

    def test_21_no_reset_after_side_effect(self):
        budget = OperationBudget(startup_limit=2)
        budget.invoke_once('ble.enable', lambda: None)
        with self.assertRaisesRegex(Exception, 'STARTUP_BUDGET_EXHAUSTED'):
            budget.startup(lambda: self.fail('reset'))

    @staticmethod
    def wrapped(outer_type, inner):
        outer = outer_type('outer-private-message')
        outer.__cause__ = inner
        return outer

    def test_22_nested_file_not_found_is_transient(self):
        failure = subject.classify_open_failure(
            self.wrapped(Exception, FileNotFoundError(errno.ENOENT, 'private')))
        self.assertEqual(failure.category, 'PATH_NOT_PRESENT')
        self.assertEqual((failure.outer_class, failure.root_class),
                         ('Exception', 'FileNotFoundError'))
        self.assertEqual(failure.wrapper_depth, 1)
        self.assertTrue(failure.transient)

    def test_23_nested_device_disconnected_is_transient(self):
        failure = subject.classify_open_failure(
            self.wrapped(RuntimeError, OSError(errno.ENODEV, 'private')))
        self.assertEqual(failure.category, 'DEVICE_DISCONNECTED')
        self.assertTrue(failure.transient)

    def test_24_nested_permission_is_terminal(self):
        failure = subject.classify_open_failure(
            self.wrapped(Exception, PermissionError(errno.EACCES, 'private')))
        self.assertEqual(failure.category, 'ACCESS_DENIED')
        self.assertTrue(failure.terminal)

    def test_25_nested_busy_is_transient(self):
        failure = subject.classify_open_failure(
            self.wrapped(Exception, OSError(errno.EBUSY, 'private')))
        self.assertEqual(failure.category, 'DEVICE_BUSY_TRANSIENT')
        self.assertTrue(failure.transient)

    def test_26_serial_wrapper_uses_deepest_file_not_found(self):
        serial_error = serial.SerialException('private wrapper')
        serial_error.__cause__ = FileNotFoundError(errno.ENOENT, 'private root')
        failure = subject.classify_open_failure(self.wrapped(Exception, serial_error))
        self.assertEqual(failure.category, 'PATH_NOT_PRESENT')
        self.assertEqual(failure.root_class, 'FileNotFoundError')
        self.assertEqual(failure.wrapper_depth, 2)

    def test_27_serial_exception_structured_errno(self):
        failure = subject.classify_open_failure(
            serial.SerialException(errno.ENODEV, 'private'))
        self.assertEqual((failure.category, failure.errno_category),
                         ('DEVICE_DISCONNECTED', 'ENODEV'))

    def test_28_generic_without_chain_is_explicit_unknown(self):
        failure = subject.classify_open_failure(Exception('private'))
        self.assertEqual(failure.category, 'SERIAL_WRAPPER_ROOT_UNKNOWN')
        self.assertTrue(failure.root_same_as_outer)
        self.assertTrue(failure.root_unknown)
        self.assertTrue(failure.terminal)

    def test_29_chain_too_deep_fails_closed(self):
        current = FileNotFoundError(errno.ENOENT, 'private')
        for _ in range(subject.MAX_EXCEPTION_CHAIN):
            current = self.wrapped(RuntimeError, current)
        failure = subject.classify_open_failure(current)
        self.assertEqual(failure.category, 'SERIAL_EXCEPTION_CHAIN_TOO_DEEP')
        self.assertEqual(failure.chain_status, 'TOO_DEEP')
        self.assertTrue(failure.terminal)

    def test_30_chain_cycle_fails_closed(self):
        first = RuntimeError('private one')
        second = RuntimeError('private two')
        first.__cause__ = second
        second.__cause__ = first
        failure = subject.classify_open_failure(first)
        self.assertEqual(failure.category, 'SERIAL_EXCEPTION_CHAIN_CYCLE')
        self.assertEqual(failure.chain_status, 'CYCLE')
        self.assertTrue(failure.root_unknown)

    def test_31_messages_and_args_never_become_diagnostics(self):
        secret_path = '/' + 'private' + '/device'
        secret_serial = 'USB_' + 'SECRET1234'
        inner = FileNotFoundError(errno.ENOENT, secret_path, secret_serial)
        result = self.run_ready(
            lambda _: subject.SerialResolution(True, True, object()),
            lambda endpoint, _: (_ for _ in ()).throw(self.wrapped(Exception, inner)),
        )
        encoded = json.dumps(result)
        self.assertNotIn(secret_path, encoded)
        self.assertNotIn(secret_serial, encoded)
        self.assertEqual(result['serial']['root_exception_class'], 'FileNotFoundError')

    def test_32_context_used_when_explicit_cause_absent(self):
        try:
            raise OSError(errno.ENODEV, 'private root')
        except OSError:
            try:
                raise RuntimeError('private outer')
            except RuntimeError as outer:
                failure = subject.classify_open_failure(outer)
        self.assertEqual(failure.category, 'DEVICE_DISCONNECTED')
        self.assertEqual(failure.wrapper_depth, 1)

    def test_33_p2r2_outer_classification_reproduced_and_repaired(self):
        error = ModuleNotFoundError('private module')
        old_name = type(error).__name__
        old_exception_class = old_name if old_name in {
            'FileNotFoundError', 'PermissionError', 'BlockingIOError', 'OSError',
            'SerialException', 'Exception',
        } else 'Exception'
        old_category = 'OTHER'
        self.assertEqual((old_category, old_exception_class), ('OTHER', 'Exception'))
        repaired = subject.classify_open_failure(error)
        self.assertEqual(repaired.category, 'PYTHON_SERIAL_INTERNAL_ERROR')
        self.assertEqual(repaired.outer_class, 'ModuleNotFoundError')

    def test_34_nested_transient_consumes_full_probe_then_reset_budget(self):
        clock = Clock()
        budget = OperationBudget(startup_limit=1)
        results = subject.startup_campaign(
            budget, lambda: None,
            lambda _: subject.SerialResolution(True, True, object()),
            lambda endpoint, _: (_ for _ in ()).throw(
                self.wrapped(Exception, FileNotFoundError(errno.ENOENT, 'private'))),
            lambda h, i: True, policy=subject.StartupPolicy(), interval=.25,
            clock=clock, sleeper=clock.sleep,
        )
        self.assertEqual([item['deadline_ms'] for item in results], [2000, 12000])
        self.assertEqual(budget.startup_attempts, 1)
        self.assertGreater(results[0]['attempts'], 1)

    def test_35_terminal_root_stops_before_reset(self):
        clock = Clock()
        budget = OperationBudget(startup_limit=2)
        results = subject.startup_campaign(
            budget, lambda: self.fail('reset'),
            lambda _: subject.SerialResolution(True, True, object()),
            lambda endpoint, _: (_ for _ in ()).throw(
                self.wrapped(Exception, PermissionError(errno.EACCES, 'private'))),
            lambda h, i: True, policy=subject.StartupPolicy(), interval=.25,
            clock=clock, sleeper=clock.sleep,
        )
        self.assertEqual(len(results), 1)
        self.assertEqual(budget.startup_attempts, 0)

    def test_36_startup_serial_and_protocol_results_are_distinct(self):
        serial_timeout = self.run_ready(
            lambda _: subject.SerialResolution(True, False),
            lambda endpoint, _: self.fail('open'))
        self.assertEqual(subject.startup_result_classification(serial_timeout),
                         'STARTUP_SERIAL_READINESS_TIMEOUT')
        self.clock = Clock()
        protocol_timeout = self.run_ready(
            lambda _: subject.SerialResolution(True, True, object()),
            lambda endpoint, _: Peer(hello=RetryableRead('private')))
        self.assertEqual(subject.startup_result_classification(protocol_timeout),
                         'STARTUP_PROTOCOL_READINESS_TIMEOUT')

    def test_37_actual_pyserial_missing_path_chain(self):
        missing = str(Path(tempfile.gettempdir()) / 's3hidbot-r2-definitely-missing')
        try:
            serial.Serial(missing, timeout=.01, exclusive=True)
        except serial.SerialException as exc:
            failure = subject.classify_open_failure(exc)
        else:
            self.fail('missing fixture unexpectedly opened')
        self.assertEqual((failure.category, failure.errno_category),
                         ('PATH_NOT_PRESENT', 'ENOENT'))

    def test_38_actual_pyserial_rejects_regular_file(self):
        with tempfile.NamedTemporaryFile() as fixture:
            try:
                serial.Serial(fixture.name, timeout=.01, exclusive=True)
            except serial.SerialException as exc:
                failure = subject.classify_open_failure(exc)
            else:
                self.fail('regular file unexpectedly opened')
        self.assertEqual(failure.category, 'INVALID_SERIAL_CONFIGURATION')

    def test_39_actual_pyserial_exclusive_busy_when_supported(self):
        master, slave = pty.openpty()
        name = os.ttyname(slave)
        first = serial.Serial(name, timeout=.01, exclusive=True)
        try:
            with self.assertRaises(serial.SerialException) as raised:
                serial.Serial(name, timeout=.01, exclusive=True)
            failure = subject.classify_open_failure(raised.exception)
            self.assertEqual(failure.category, 'DEVICE_BUSY_TRANSIENT')
        finally:
            first.close()
            os.close(master)
            os.close(slave)

    def test_40_actual_pyserial_permission_denied_fixture(self):
        if os.geteuid() == 0:
            self.skipTest('permission fixture requires an unprivileged process')
        descriptor, name = tempfile.mkstemp(prefix='s3hidbot-r2-permission-')
        os.close(descriptor)
        os.chmod(name, 0)
        try:
            with self.assertRaises(serial.SerialException) as raised:
                serial.Serial(name, timeout=.01, exclusive=True)
            failure = subject.classify_open_failure(raised.exception)
            self.assertEqual(failure.category, 'ACCESS_DENIED')
            self.assertTrue(failure.terminal)
        finally:
            os.chmod(name, 0o600)
            os.unlink(name)


if __name__ == '__main__':
    unittest.main()
