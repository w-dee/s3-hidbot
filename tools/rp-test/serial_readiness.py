"""Privacy-safe serial acquisition below protocol readiness.

Concrete missions own hardware identity matching.  This module repeatedly
asks that resolver for the current endpoint, classifies only the open boundary,
and never serializes the endpoint value or an exception message.
"""
from dataclasses import dataclass, field
import errno
import math
import time

from common import InfraError, need
from startup_probe import MalformedRead, RetryableRead

MAX_EXCEPTION_CHAIN = 8
OPEN_CATEGORIES = frozenset((
    'PATH_NOT_PRESENT', 'DEVICE_NODE_NOT_READY', 'SYMLINK_TARGET_MISSING',
    'DEVICE_DISCONNECTED', 'DEVICE_REENUMERATING', 'DEVICE_BUSY_TRANSIENT',
    'ACCESS_DENIED', 'INVALID_DEVICE_TYPE', 'UNSUPPORTED_DEVICE',
    'IDENTITY_AMBIGUOUS', 'INVALID_SERIAL_CONFIGURATION',
    'PYTHON_SERIAL_INTERNAL_ERROR', 'SERIAL_WRAPPER_ROOT_UNKNOWN',
    'SERIAL_EXCEPTION_CHAIN_CYCLE', 'SERIAL_EXCEPTION_CHAIN_TOO_DEEP',
))
EXCEPTION_CLASSES = frozenset((
    'FileNotFoundError', 'PermissionError', 'BlockingIOError', 'OSError',
    'SerialException', 'TransportError', 'ModuleNotFoundError', 'ImportError',
    'ValueError', 'TypeError', 'AttributeError', 'RuntimeError', 'TermiosError',
    'Exception',
))
ERRNO_CATEGORIES = frozenset((
    'NONE', 'ENOENT', 'ENODEV', 'ENXIO', 'ESTALE', 'EACCES', 'EPERM',
    'EBUSY', 'EAGAIN', 'ENOTTY', 'EINVAL', 'EIO', 'OTHER',
))
CHAIN_STATUSES = frozenset(('COMPLETE', 'CYCLE', 'TOO_DEEP'))
STATES = frozenset((
    'STARTUP_BEGIN', 'SERIAL_IDENTITY_RESOLVED', 'SERIAL_DEVICE_WAIT',
    'SERIAL_OPEN_ATTEMPT', 'SERIAL_PORT_OPEN', 'PROTOCOL_SYNC',
    'HELLO_READY', 'IDENTITY_VERIFIED',
))
TERMINAL_IDENTITY = frozenset(('IDENTITY_AMBIGUOUS', 'UNSUPPORTED_DEVICE'))
OPPORTUNISTIC_SECONDS = 2.0
RESET_READINESS_SECONDS = 12.0


@dataclass(frozen=True)
class SerialResolution:
    """Private endpoint is deliberately excluded from repr and diagnostics."""
    identity_resolved: bool
    node_ready: bool
    endpoint: object = field(default=None, repr=False, compare=False)

    def __post_init__(self):
        need(type(self.identity_resolved) is bool and type(self.node_ready) is bool,
             'SERIAL_RESOLUTION_INVALID')
        need(not self.node_ready or (self.identity_resolved and self.endpoint is not None),
             'SERIAL_RESOLUTION_INVALID')


class SerialIdentityFailure(InfraError):
    def __init__(self, category):
        need(category in TERMINAL_IDENTITY, 'SERIAL_IDENTITY_CATEGORY_INVALID')
        super().__init__(category)
        self.category = category


@dataclass(frozen=True)
class OpenFailure:
    category: str
    outer_class: str
    root_class: str
    wrapper_depth: int
    errno_category: str
    transient: bool
    terminal: bool
    root_same_as_outer: bool
    root_unknown: bool
    chain_status: str

    def __post_init__(self):
        need(self.category in OPEN_CATEGORIES and self.outer_class in EXCEPTION_CLASSES
             and self.root_class in EXCEPTION_CLASSES and type(self.wrapper_depth) is int
             and 0 <= self.wrapper_depth < MAX_EXCEPTION_CHAIN
             and self.errno_category in ERRNO_CATEGORIES
             and type(self.transient) is bool and type(self.terminal) is bool
             and self.transient != self.terminal and type(self.root_same_as_outer) is bool
             and type(self.root_unknown) is bool and self.chain_status in CHAIN_STATUSES,
             'SERIAL_OPEN_FAILURE_INVALID')

    @property
    def exception_class(self):
        """Compatibility alias for the old outer-only diagnostic field."""
        return self.outer_class


def _safe_class(exc):
    if type(exc).__module__ == 'termios' and type(exc).__name__ == 'error':
        return 'TermiosError'
    name = type(exc).__name__
    return name if name in EXCEPTION_CLASSES else 'Exception'


def _exception_chain(exc, limit=MAX_EXCEPTION_CHAIN):
    """Return a bounded object-only chain; never inspect args, text or repr."""
    need(type(limit) is int and 1 <= limit <= MAX_EXCEPTION_CHAIN,
         'SERIAL_EXCEPTION_CHAIN_LIMIT_INVALID')
    values = []
    seen = set()
    current = exc
    status = 'COMPLETE'
    while current is not None:
        if id(current) in seen:
            status = 'CYCLE'
            break
        if len(values) >= limit:
            status = 'TOO_DEEP'
            break
        seen.add(id(current))
        values.append(current)
        current = current.__cause__ if current.__cause__ is not None else current.__context__
    return tuple(values), status


def _errno_name(value):
    if type(value) is not int:
        return 'NONE'
    mapping = {
        errno.ENOENT: 'ENOENT', errno.ENODEV: 'ENODEV', errno.ENXIO: 'ENXIO',
        errno.EACCES: 'EACCES', errno.EPERM: 'EPERM', errno.EBUSY: 'EBUSY',
        errno.EAGAIN: 'EAGAIN', errno.ENOTTY: 'ENOTTY', errno.EINVAL: 'EINVAL',
        errno.EIO: 'EIO',
    }
    if hasattr(errno, 'ESTALE'):
        mapping[errno.ESTALE] = 'ESTALE'
    return mapping.get(value, 'OTHER')


def _structured_errno(chain):
    """Prefer the deepest structured errno/winerror; never inspect args."""
    for item in reversed(chain):
        category = _errno_name(getattr(item, 'errno', None))
        if category != 'NONE':
            return category
        winerror = getattr(item, 'winerror', None)
        portable = {2: 'ENOENT', 5: 'EACCES', 32: 'EBUSY'}
        if type(winerror) is int:
            return portable.get(winerror, 'OTHER')
    return 'NONE'


def _normalized_category(chain, status, errno_category):
    if status == 'CYCLE':
        return 'SERIAL_EXCEPTION_CHAIN_CYCLE', False, True, True
    if status == 'TOO_DEEP':
        return 'SERIAL_EXCEPTION_CHAIN_TOO_DEEP', False, True, True
    mappings = {
        'ENOENT': ('PATH_NOT_PRESENT', True),
        'ENODEV': ('DEVICE_DISCONNECTED', True),
        'ENXIO': ('DEVICE_DISCONNECTED', True),
        'ESTALE': ('DEVICE_REENUMERATING', True),
        'EBUSY': ('DEVICE_BUSY_TRANSIENT', True),
        'EAGAIN': ('DEVICE_BUSY_TRANSIENT', True),
        'EACCES': ('ACCESS_DENIED', False),
        'EPERM': ('ACCESS_DENIED', False),
        'ENOTTY': ('INVALID_DEVICE_TYPE', False),
        'EINVAL': ('INVALID_SERIAL_CONFIGURATION', False),
    }
    if errno_category in mappings:
        category, transient = mappings[errno_category]
        return category, transient, not transient, False
    root = chain[-1]
    if isinstance(root, FileNotFoundError):
        return 'PATH_NOT_PRESENT', True, False, False
    if isinstance(root, PermissionError):
        return 'ACCESS_DENIED', False, True, False
    if isinstance(root, BlockingIOError):
        return 'DEVICE_BUSY_TRANSIENT', True, False, False
    root_name = _safe_class(root)
    if root_name in ('ModuleNotFoundError', 'ImportError', 'AttributeError'):
        return 'PYTHON_SERIAL_INTERNAL_ERROR', False, True, False
    if root_name in ('ValueError', 'TypeError'):
        return 'INVALID_SERIAL_CONFIGURATION', False, True, False
    if root_name == 'TermiosError':
        return 'INVALID_SERIAL_CONFIGURATION', False, True, False
    if root_name == 'SerialException':
        return 'PYTHON_SERIAL_INTERNAL_ERROR', False, True, False
    return 'SERIAL_WRAPPER_ROOT_UNKNOWN', False, True, True


def classify_open_failure(exc):
    """Normalize cause/context structurally without retaining any message/path."""
    chain, status = _exception_chain(exc)
    need(bool(chain), 'SERIAL_EXCEPTION_CHAIN_EMPTY')
    errno_category = _structured_errno(chain)
    category, transient, terminal, root_unknown = _normalized_category(
        chain, status, errno_category)
    return OpenFailure(
        category=category,
        outer_class=_safe_class(chain[0]),
        root_class=_safe_class(chain[-1]),
        wrapper_depth=len(chain) - 1,
        errno_category=errno_category,
        transient=transient,
        terminal=terminal,
        root_same_as_outer=len(chain) == 1,
        root_unknown=root_unknown,
        chain_status=status,
    )


def _diagnostics():
    return {
        'schema': 2,
        'identity_resolved': False,
        'node_seen_count': 0,
        'open_attempt_count': 0,
        'open_success_count': 0,
        'protocol_attempt_count': 0,
        'last_open_failure': None,
        'open_exception_class': None,
        'open_failure_transient': None,
        'open_failure_terminal': None,
        'outer_exception_class': None,
        'root_exception_class': None,
        'wrapper_depth': None,
        'errno_category': None,
        'normalized_serial_category': None,
        'root_same_as_outer': None,
        'root_unknown': None,
        'exception_chain_status': None,
        'startup_substage': 'identity',
    }


def validate_diagnostics(value):
    expected = {
        'schema',
        'identity_resolved', 'node_seen_count', 'open_attempt_count',
        'open_success_count', 'protocol_attempt_count', 'last_open_failure',
        'open_exception_class', 'open_failure_transient', 'open_failure_terminal',
        'outer_exception_class', 'root_exception_class', 'wrapper_depth',
        'errno_category', 'normalized_serial_category', 'root_same_as_outer',
        'root_unknown', 'exception_chain_status', 'startup_substage',
    }
    need(isinstance(value, dict) and set(value) == expected, 'STARTUP_DIAGNOSTICS_INVALID')
    for key in ('node_seen_count', 'open_attempt_count', 'open_success_count', 'protocol_attempt_count'):
        need(type(value[key]) is int and 0 <= value[key] <= 100000, 'STARTUP_DIAGNOSTICS_INVALID')
    need(value['schema'] == 2 and type(value['identity_resolved']) is bool
         and value['open_success_count'] <= value['open_attempt_count']
         and value['protocol_attempt_count'] <= value['open_success_count'],
         'STARTUP_DIAGNOSTICS_INVALID')
    need(value['last_open_failure'] is None or value['last_open_failure'] in OPEN_CATEGORIES,
         'STARTUP_DIAGNOSTICS_INVALID')
    need(value['open_exception_class'] is None or value['open_exception_class'] in EXCEPTION_CLASSES,
         'STARTUP_DIAGNOSTICS_INVALID')
    need(value['outer_exception_class'] is None or value['outer_exception_class'] in EXCEPTION_CLASSES,
         'STARTUP_DIAGNOSTICS_INVALID')
    need(value['root_exception_class'] is None or value['root_exception_class'] in EXCEPTION_CLASSES,
         'STARTUP_DIAGNOSTICS_INVALID')
    need(value['wrapper_depth'] is None or type(value['wrapper_depth']) is int
         and 0 <= value['wrapper_depth'] < MAX_EXCEPTION_CHAIN,
         'STARTUP_DIAGNOSTICS_INVALID')
    need(value['errno_category'] is None or value['errno_category'] in ERRNO_CATEGORIES,
         'STARTUP_DIAGNOSTICS_INVALID')
    need(value['normalized_serial_category'] is None
         or value['normalized_serial_category'] in OPEN_CATEGORIES,
         'STARTUP_DIAGNOSTICS_INVALID')
    need(value['exception_chain_status'] is None or value['exception_chain_status'] in CHAIN_STATUSES,
         'STARTUP_DIAGNOSTICS_INVALID')
    need(value['open_failure_transient'] in (None, True, False)
         and value['open_failure_terminal'] in (None, True, False)
         and value['root_same_as_outer'] in (None, True, False)
         and value['root_unknown'] in (None, True, False)
         and value['startup_substage'] in ('identity', 'device_wait', 'open', 'protocol', 'identity_check'),
         'STARTUP_DIAGNOSTICS_INVALID')
    return value


def _result(classification, states, attempts, diagnostics):
    validate_diagnostics(diagnostics)
    need(all(state in STATES for state in states), 'STARTUP_STATE_INVALID')
    return {'classification': classification, 'states': states, 'attempts': attempts,
            'serial': diagnostics}


def serial_readiness(resolve_endpoint, open_peer, identity_matches, *, timeout=RESET_READINESS_SECONDS,
                     interval=0.3, clock=time.monotonic, sleeper=time.sleep):
    """Re-resolve identity/node before every single-handle open attempt."""
    need(math.isfinite(timeout) and math.isfinite(interval)
         and 0 < interval <= timeout <= 60, 'DEADLINE_INVALID')
    deadline = clock() + timeout
    states = ['STARTUP_BEGIN', 'SERIAL_DEVICE_WAIT']
    diagnostics = _diagnostics()
    attempts = 0
    malformed = False

    def remaining():
        need(clock() < deadline, 'PROTOCOL_READINESS_TIMEOUT')
        return deadline - clock()

    while clock() < deadline:
        peer = None
        attempts += 1
        try:
            resolution = resolve_endpoint(remaining())
            need(isinstance(resolution, SerialResolution), 'SERIAL_RESOLUTION_INVALID')
            if resolution.identity_resolved:
                diagnostics['identity_resolved'] = True
                if 'SERIAL_IDENTITY_RESOLVED' not in states:
                    states.append('SERIAL_IDENTITY_RESOLVED')
            if not resolution.node_ready:
                diagnostics['startup_substage'] = 'device_wait'
            else:
                diagnostics['node_seen_count'] += 1
                diagnostics['open_attempt_count'] += 1
                diagnostics['startup_substage'] = 'open'
                states.append('SERIAL_OPEN_ATTEMPT')
                try:
                    peer = open_peer(resolution.endpoint, remaining())
                except Exception as exc:
                    failure = classify_open_failure(exc)
                    diagnostics.update(last_open_failure=failure.category,
                                       open_exception_class=failure.exception_class,
                                       open_failure_transient=failure.transient,
                                       open_failure_terminal=failure.terminal,
                                       outer_exception_class=failure.outer_class,
                                       root_exception_class=failure.root_class,
                                       wrapper_depth=failure.wrapper_depth,
                                       errno_category=failure.errno_category,
                                       normalized_serial_category=failure.category,
                                       root_same_as_outer=failure.root_same_as_outer,
                                       root_unknown=failure.root_unknown,
                                       exception_chain_status=failure.chain_status)
                    if not failure.transient:
                        return _result('SERIAL_OPEN_' + failure.category, states, attempts, diagnostics)
                else:
                    need(peer is not None, 'SERIAL_OPEN_ADAPTER_INVALID')
                    diagnostics['open_success_count'] += 1
                    diagnostics['protocol_attempt_count'] += 1
                    diagnostics['startup_substage'] = 'protocol'
                    states.extend(('SERIAL_PORT_OPEN', 'PROTOCOL_SYNC'))
                    peer.sync(remaining())
                    hello = peer.hello(remaining())
                    states.append('HELLO_READY')
                    info = peer.info(remaining())
                    remaining()
                    diagnostics['startup_substage'] = 'identity_check'
                    if not identity_matches(hello, info):
                        return _result('IDENTITY_MISMATCH', states, attempts, diagnostics)
                    states.append('IDENTITY_VERIFIED')
                    return _result('IDENTITY_VERIFIED', states, attempts, diagnostics)
        except SerialIdentityFailure as exc:
            return _result(exc.category, states, attempts, diagnostics)
        except MalformedRead:
            malformed = True
            diagnostics['startup_substage'] = 'protocol'
        except RetryableRead:
            diagnostics['startup_substage'] = 'protocol'
        except InfraError as exc:
            if str(exc) != 'PROTOCOL_READINESS_TIMEOUT':
                raise
        finally:
            if peer is not None:
                peer.close()
        sleeper(max(0, min(interval, deadline - clock())))

    if malformed:
        classification = 'PROTOCOL_MALFORMED'
    elif diagnostics['open_success_count']:
        classification = 'PROTOCOL_READINESS_TIMEOUT'
    elif diagnostics['open_attempt_count']:
        classification = 'SERIAL_OPEN_DEADLINE_EXPIRED'
    elif diagnostics['identity_resolved']:
        classification = 'SERIAL_DEVICE_NODE_NOT_READY'
    else:
        classification = 'SERIAL_IDENTITY_NOT_READY'
    return _result(classification, states, attempts, diagnostics)


@dataclass(frozen=True)
class StartupPolicy:
    opportunistic_seconds: float = OPPORTUNISTIC_SECONDS
    reset_seconds: float = RESET_READINESS_SECONDS
    start_state: str = 'unknown'

    def __post_init__(self):
        need(self.start_state in ('unknown', 'application_running', 'application_not_running')
             and math.isfinite(self.opportunistic_seconds) and 0 < self.opportunistic_seconds <= 2
             and self.reset_seconds == RESET_READINESS_SECONDS, 'STARTUP_POLICY_INVALID')

    @property
    def use_opportunistic_probe(self):
        return self.start_state != 'application_not_running'


def terminal_startup_result(value):
    classification = value['classification']
    if classification in ('IDENTITY_VERIFIED', 'IDENTITY_MISMATCH',
                          'IDENTITY_AMBIGUOUS', 'UNSUPPORTED_DEVICE'):
        return True
    return (classification.startswith('SERIAL_OPEN_')
            and classification != 'SERIAL_OPEN_DEADLINE_EXPIRED')


def startup_result_classification(value):
    """Separate serial acquisition from protocol readiness for mission results."""
    classification = value['classification']
    diagnostics = value['serial']
    validate_diagnostics(diagnostics)
    if classification == 'IDENTITY_VERIFIED':
        return 'STARTUP_READY'
    if classification == 'IDENTITY_MISMATCH':
        return 'INSTALLED_QUALIFICATION_IDENTITY_INVALID'
    if classification in TERMINAL_IDENTITY:
        return 'STARTUP_SERIAL_IDENTITY_INVALID'
    if classification == 'SERIAL_OPEN_ACCESS_DENIED':
        return 'STARTUP_SERIAL_ACCESS_DENIED'
    if classification == 'SERIAL_OPEN_SERIAL_WRAPPER_ROOT_UNKNOWN':
        return 'STARTUP_SERIAL_ROOT_UNKNOWN'
    if classification.startswith('SERIAL_OPEN_') and classification != 'SERIAL_OPEN_DEADLINE_EXPIRED':
        return 'STARTUP_SERIAL_' + classification.removeprefix('SERIAL_OPEN_')
    if diagnostics['open_success_count'] == 0:
        return 'STARTUP_SERIAL_READINESS_TIMEOUT'
    return 'STARTUP_PROTOCOL_READINESS_TIMEOUT'


def startup_campaign(budget, reset, resolve_endpoint, open_peer, identity_matches, *,
                     policy=StartupPolicy(), interval=0.3, clock=time.monotonic, sleeper=time.sleep):
    """Short no-reset probe plus at most two full reset-driven attempts."""
    results = []
    if policy.use_opportunistic_probe:
        value = serial_readiness(resolve_endpoint, open_peer, identity_matches,
                                 timeout=policy.opportunistic_seconds, interval=min(interval, policy.opportunistic_seconds),
                                 clock=clock, sleeper=sleeper)
        results.append(dict(value, probe='opportunistic', reset_attempt=0,
                            deadline_ms=round(policy.opportunistic_seconds * 1000)))
        if terminal_startup_result(value):
            return results
    while not budget.invoked and budget.startup_attempts < budget.startup_limit:
        budget.startup(reset)
        value = serial_readiness(resolve_endpoint, open_peer, identity_matches,
                                 timeout=policy.reset_seconds, interval=interval,
                                 clock=clock, sleeper=sleeper)
        value['states'].insert(0, 'RESET_REQUESTED')
        results.append(dict(value, probe='reset', reset_attempt=budget.startup_attempts,
                            deadline_ms=round(policy.reset_seconds * 1000)))
        if terminal_startup_result(value):
            break
    return results
