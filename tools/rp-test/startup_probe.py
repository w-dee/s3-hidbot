"""Protocol readiness and conservative replay-suppression budgets.

This module has no physical backend.

Adapters must bound each I/O to the remaining deadline, use fresh Client/Framer
and correlated nonce/session/request IDs, and open/close at board-safe idle.
No resets are performed by readiness(): only the separate mission budget can
authorize a caller-supplied startup action. New physical missions use
serial_readiness.py for the explicit identity/node/open boundary; readiness()
remains the compatibility protocol-acquisition primitive.
"""
from dataclasses import dataclass, field
import math
import time

from common import InfraError, need

READ_ONLY = frozenset(('protocol.hello', 'system.info', 'system.ping', 'usb.status',
    'usb.exposure.status', 'ble.exposure.status', 'ble.pairing.status', 'ble.bond.list',
    'hid.route.status', 'hid.route.v2.status', 'temporary.ble.diag.stage'))
SIDE_EFFECTS = frozenset(('Pair', 'Connect', 'Disconnect', 'bond.delete', 'ble.bond.remove',
    'NVS.mutation', 'flash', 'route.change', 'pairing.response', 'ble.enable', 'ble.disable'))

class RetryableRead(InfraError):
    pass

class MalformedRead(InfraError):
    pass

@dataclass
class OperationBudget:
    """Consume authority before invocation; a thrown callback remains consumed.

    A budget is valid only within a mission holding the global physical lock.
    On process restart, reconstruct from the capsule's ledger, never reset it.
    The marker suppresses replay; it does not prove callback return, success,
    or exactly-once physical execution.
    """
    startup_limit: int = 1
    startup_attempts: int = 0
    invoked: set = field(default_factory=set)
    commit: object = lambda _event, _operation: None

    def __post_init__(self):
        need(type(self.startup_limit) is int and 0 <= self.startup_limit <= 2, 'STARTUP_BUDGET_INVALID')
        need(0 <= self.startup_attempts <= self.startup_limit and self.invoked <= SIDE_EFFECTS, 'BUDGET_STATE_INVALID')

    def startup(self, action):
        need(not self.invoked and self.startup_attempts < self.startup_limit, 'STARTUP_BUDGET_EXHAUSTED')
        self.commit('STARTUP_INVOKED', 'startup')
        self.startup_attempts += 1
        return action()

    def invoke_once(self, operation, action):
        need(operation in SIDE_EFFECTS, 'SIDE_EFFECT_CLASS_REQUIRED')
        # Aliases cannot supply a second budget for the same mutation.
        key = 'ble.bond.remove' if operation == 'bond.delete' else operation
        need(key not in self.invoked, 'SIDE_EFFECT_ALREADY_INVOKED')
        self.commit('SIDE_EFFECT_INVOKED', key)
        self.invoked.add(key)
        return action()

def bounded_read(operation, action, *, timeout=12.0, interval=0.3,
                 clock=time.monotonic, sleeper=time.sleep):
    need(operation in READ_ONLY, 'READ_ONLY_CLASS_REQUIRED')
    need(math.isfinite(timeout) and math.isfinite(interval) and 0 < interval <= timeout <= 60, 'DEADLINE_INVALID')
    deadline = clock() + timeout
    while clock() < deadline:
        try:
            value = action(deadline - clock())
            need(clock() <= deadline, 'PROTOCOL_READINESS_TIMEOUT')
            return value
        except RetryableRead:
            sleeper(max(0, min(interval, deadline - clock())))
    raise InfraError('PROTOCOL_READINESS_TIMEOUT')

def readiness(open_peer, identity_matches, *, timeout=12.0, interval=0.3,
              clock=time.monotonic, sleeper=time.sleep):
    """Poll serial presence, then correlated hello + identity; never mutate.

    open_peer(remaining) returns None while serial is absent; otherwise returns
    an adapter with sync(remaining), hello(remaining), info(remaining), close().
    Exceptions must already be classified, with raw diagnostics kept separately.
    A failed round closes/discards the old parser/session before the next open.
    """
    need(math.isfinite(timeout) and math.isfinite(interval) and 0 < interval <= timeout <= 60, 'DEADLINE_INVALID')
    deadline = clock() + timeout
    states, attempts, seen_serial, malformed = ['SERIAL_WAIT'], 0, False, False
    def remaining():
        need(clock() < deadline, 'PROTOCOL_READINESS_TIMEOUT')
        return deadline - clock()
    while clock() < deadline:
        peer = None
        attempts += 1
        try:
            peer = open_peer(remaining())
            if peer is not None:
                seen_serial = True
                states.append('PROTOCOL_SYNC')
                peer.sync(remaining())
                hello = peer.hello(remaining())
                states.append('HELLO_READY')
                info = peer.info(remaining())
                remaining()
                if not identity_matches(hello, info):
                    return {'classification': 'IDENTITY_MISMATCH', 'states': states, 'attempts': attempts}
                states.append('IDENTITY_VERIFIED')
                return {'classification': 'IDENTITY_VERIFIED', 'states': states, 'attempts': attempts}
        except MalformedRead:
            malformed = True
        except RetryableRead:
            pass
        except InfraError as exc:
            if str(exc) != 'PROTOCOL_READINESS_TIMEOUT':
                raise
        finally:
            if peer is not None:
                peer.close()
        sleeper(max(0, min(interval, deadline - clock())))
    code = 'PROTOCOL_MALFORMED' if malformed else 'PROTOCOL_READINESS_TIMEOUT' if seen_serial else 'SERIAL_NOT_READY'
    return {'classification': code, 'states': states, 'attempts': attempts}

def startup_sequence(budget, reset, open_peer, identity_matches, **kwargs):
    """Future mission-only integration. No concrete serial/reset implementation."""
    while True:
        budget.startup(reset)
        result = readiness(open_peer, identity_matches, **kwargs)
        result['states'].insert(0, 'RESET_REQUESTED')
        if result['classification'] in ('IDENTITY_VERIFIED', 'IDENTITY_MISMATCH', 'PROTOCOL_MALFORMED'):
            return result
        if budget.invoked or budget.startup_attempts >= budget.startup_limit:
            return result

class ClientPeer:
    """Bridge to the existing strict Client; factory is supplied by a mission.

    Factory receives a per-request timeout, must use max_attempts=1 and a fresh
    Framer, and is solely responsible for privately resolving the UART. Client
    hello emits TRANSPORT_SYNC and validates nonce/session/ID. Framer discards
    stale/partial frames. Closing never performs a reset.
    """
    def __init__(self, client):
        need(client._max_attempts == 1, 'CLIENT_RETRY_POLICY_INVALID')
        self.client = client

    def sync(self, remaining):
        self.client._timeout = min(0.75, remaining)

    def call(self, method, remaining):
        self.client._timeout = min(0.75, remaining)
        try:
            return method()
        except Exception as exc:
            name = type(exc).__name__
            if name in ('RequestTimeoutError', 'TransportError', 'SessionLostError'):
                raise RetryableRead('PROTOCOL_SYNCING') from None
            if name == 'ProtocolError':
                raise MalformedRead('PROTOCOL_MALFORMED') from None
            raise InfraError('PROTOCOL_ADAPTER_FAILED') from None

    def hello(self, remaining):
        return self.call(self.client.connect, remaining)

    def info(self, remaining):
        return self.call(self.client.info, remaining)

    def close(self):
        self.client.close()

def qualification_may_remain(*, exact_identity, understood_state, pending_recovery, contained,
                             explicit_restore=False, milestone_ended=False, recovery_requires_restore=False):
    return bool(exact_identity and understood_state and not pending_recovery and contained
                and not (explicit_restore or milestone_ended or recovery_requires_restore))
