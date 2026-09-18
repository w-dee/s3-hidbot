#!/usr/bin/env python3
"""Deterministic reset-normalized preflight; never starts qualification."""
from __future__ import annotations

import argparse
import dataclasses
import json
import os
from pathlib import Path
import time
import traceback

EXPECTED_ARCHIVE = "2e6b5a1a20a83e20cfeafa2d10835ddb605f4aff0319225b48e340c856b39a35"
EXPECTED_SOURCE = "137d489d3d1219b203f84633cf8b570d9fe9f19a"
EXPECTED_ELF = "0a0ac1ded06b1dddce6d014fc1127f9a49f65c23dd289ae89929c4dc41c16c45"
EXPECTED_PROFILE = "freenove-fnk0099"
STRICT_PROFILE = "strict_composite"


class PreflightFailure(RuntimeError):
    pass


def require(value: bool, code: str) -> None:
    if not value:
        raise PreflightFailure(code)


def normalize_fixture(read_initial, reset_once, read_post_reset):
    """Execute the mandatory one-reset gate with injectable hardware seams."""
    events = []
    initial = read_initial()
    events.append("initial_state")
    require(initial["identity_match"], "INITIAL_IDENTITY_MISMATCH")

    reset_once()
    events.append("controlled_application_reset")

    final = read_post_reset()
    events.append("post_reset_state")
    require(final["identity_match"], "POST_RESET_IDENTITY_MISMATCH")
    require(final["boot_id"] != initial["boot_id"], "APPLICATION_RESET_NOT_OBSERVED")
    require(final["profile"] == STRICT_PROFILE, "POST_RESET_PROFILE_NOT_STRICT")
    require(final["profile_stable"], "POST_RESET_PROFILE_NOT_STABLE")
    require(final["route_none"], "POST_RESET_ROUTE_NOT_NONE")
    require(final["all_up"], "POST_RESET_NOT_ALL_UP")
    require(not final["sequence_active"], "POST_RESET_SEQUENCE_ACTIVE")
    require(final["pairing_idle"], "POST_RESET_PAIRING_ACTIVE")
    require(final["ble_safe"], "POST_RESET_BLE_NOT_HIDDEN")
    require(final["usb_safe"], "POST_RESET_USB_NOT_HIDDEN")
    require(final["host_safe"], "POST_RESET_HOST_NOT_SAFE")
    require(final["bond_store_healthy"], "POST_RESET_BOND_STORE_UNHEALTHY")
    require(initial["bond_inventory"] == final["bond_inventory"],
            "POST_RESET_BOND_INVENTORY_CHANGED")
    return {
        "result": "PASS",
        "events": events,
        "reset_count": 1,
        "initial": initial,
        "post_reset": final,
        "bond_inventory_preserved": initial["bond_inventory"] == final["bond_inventory"],
    }


def _plain(value):
    if dataclasses.is_dataclass(value):
        return _plain(dataclasses.asdict(value))
    if isinstance(value, dict):
        return {str(key): _plain(item) for key, item in value.items()}
    if isinstance(value, (tuple, list)):
        return [_plain(item) for item in value]
    if hasattr(value, "value"):
        return value.value
    return value


def _identity(client, artifact):
    from hidbot.firmware_verification import compare_firmware_identity
    from hidbot.protocol import validate_system_info
    from hidbot.provisioning import stage_and_inspect_firmware_bundle

    with stage_and_inspect_firmware_bundle(artifact) as bundle:
        identity = bundle.artifact_identity
        require(identity.source_revision == EXPECTED_SOURCE, "ARTIFACT_SOURCE_MISMATCH")
        require(identity.app_elf_sha256 == EXPECTED_ELF, "ARTIFACT_ELF_MISMATCH")
        require(identity.build_profile == EXPECTED_PROFILE, "ARTIFACT_PROFILE_MISMATCH")
        comparison = compare_firmware_identity(
            identity,
            client.capabilities,
            validate_system_info(client.info(), capabilities=client.capabilities),
        )
    return comparison


def _bluez_safe():
    import dbus

    bus = dbus.SystemBus()
    managed = dbus.Interface(
        bus.get_object("org.bluez", "/"), "org.freedesktop.DBus.ObjectManager"
    ).GetManagedObjects()
    adapters = [value["org.bluez.Adapter1"] for value in managed.values()
                if "org.bluez.Adapter1" in value]
    require(len(adapters) == 1, "BLUETOOTH_ADAPTER_NOT_UNIQUE")
    adapter = adapters[0]
    connected = sum(bool(value["org.bluez.Device1"].get("Connected"))
                    for value in managed.values() if "org.bluez.Device1" in value)
    return {
        "powered": bool(adapter.get("Powered")),
        "discoverable": bool(adapter.get("Discoverable")),
        "pairable": bool(adapter.get("Pairable")),
        "discovering": bool(adapter.get("Discovering")),
        "connected_devices": connected,
    }


def _set_bluez_powered(powered: bool, timeout_seconds=5.0) -> bool:
    """Set the unique adapter power state and return its previous value."""
    import dbus

    bus = dbus.SystemBus()
    managed = dbus.Interface(
        bus.get_object("org.bluez", "/"), "org.freedesktop.DBus.ObjectManager"
    ).GetManagedObjects()
    adapters = [path for path, value in managed.items()
                if "org.bluez.Adapter1" in value]
    require(len(adapters) == 1, "BLUETOOTH_ADAPTER_NOT_UNIQUE")
    properties = dbus.Interface(
        bus.get_object("org.bluez", adapters[0]), "org.freedesktop.DBus.Properties"
    )
    previous = bool(properties.Get("org.bluez.Adapter1", "Powered"))
    if previous != powered:
        properties.Set("org.bluez.Adapter1", "Powered", dbus.Boolean(powered))
    deadline = time.monotonic() + timeout_seconds
    while bool(properties.Get("org.bluez.Adapter1", "Powered")) != powered:
        require(time.monotonic() < deadline, "BLUETOOTH_ADAPTER_POWER_TIMEOUT")
        time.sleep(0.08)
    return previous


def _usb_is_safe(usb):
    """Evaluate the validated UsbExposureStatus literal-valued contract."""
    return (
        usb.desired == "hidden"
        and not usb.mounted
        and not usb.keyboard_ready
        and not usb.mouse_ready
        and not usb.recovery_required
        and not usb.safety_pending
        and not usb.host_release_uncertain
        and usb.last_error is None
    )


def _ble_is_safe(ble):
    return (
        ble.desired.value == "hidden"
        and ble.observed.value in ("uninitialized", "idle")
        and not ble.advertising
        and not ble.connected
        and not ble.recovery_required
        and ble.last_error is None
    )


def _pairing_is_idle(pairing):
    return (
        pairing.state.value == "idle"
        and not pairing.connected
        and pairing.pairing_id is None
        and pairing.action is None
    )


def _bond_inventory(client, timeout_seconds=12.0):
    """Read bonds, briefly initializing BLE when the boot-safe store is unavailable."""
    from hidbot.errors import RemoteError

    try:
        return client.ble_bond_list(), False
    except RemoteError as exc:
        if exc.code != "BLE_NOT_READY":
            raise

    boot = client.ble_exposure_status()
    require(_ble_is_safe(boot) and boot.observed.value == "uninitialized",
            "BOND_STORE_UNAVAILABLE_OUTSIDE_BOOT_STATE")
    original_host_power = _set_bluez_powered(False)
    deadline = time.monotonic() + timeout_seconds
    activated = False
    try:
        activated = True
        client.ble_enable()
        while True:
            exposure = client.ble_exposure_status()
            pairing = client.ble_pairing_status()
            require(not exposure.connected and not exposure.recovery_required
                    and exposure.last_error is None, "BOND_PROBE_BLE_NOT_SAFE")
            require(_pairing_is_idle(pairing), "BOND_PROBE_PAIRING_ACTIVE")
            try:
                bonds = client.ble_bond_list()
                return bonds, True
            except RemoteError as exc:
                if exc.code != "BLE_NOT_READY":
                    raise
            require(time.monotonic() < deadline, "BOND_STORE_INITIALIZATION_TIMEOUT")
            time.sleep(0.08)
    finally:
        try:
            if activated:
                client.ble_disable()
                hide_deadline = time.monotonic() + timeout_seconds
                while True:
                    exposure = client.ble_exposure_status()
                    if _ble_is_safe(exposure) and exposure.observed.value == "idle":
                        break
                    require(time.monotonic() < hide_deadline, "BOND_PROBE_HIDE_TIMEOUT")
                    time.sleep(0.08)
        finally:
            _set_bluez_powered(original_host_power)


def _open_client(deadline_seconds=12.0):
    from ble_cleanup_rehearsal import _serial_port
    from hidbot.client import Client
    from hidbot.serial_transport import PySerialTransport

    deadline = time.monotonic() + deadline_seconds
    attempts = 0
    last = None
    while time.monotonic() < deadline:
        attempts += 1
        transport = None
        try:
            transport = PySerialTransport(_serial_port(None), 115200)
            transport.open()
            client = Client(transport, timeout=1.3, max_attempts=1)
            hello = client.connect()
            return client, hello, attempts
        except Exception as exc:
            last = type(exc).__name__
            if transport is not None:
                try:
                    transport.close()
                except Exception:
                    pass
            time.sleep(0.15)
    raise PreflightFailure("CONTROL_RECOVERY_TIMEOUT:" + str(last))


def _inspect_state(artifact, *, verify_safety):
    from ble_cleanup_rehearsal import _headless

    client, hello, attempts = _open_client()
    try:
        comparison = _identity(client, artifact)
        profile = client.ble_profile_status()
        route = client.hid_route_status()
        release = client.release_all() if verify_safety else None
        pairing = client.ble_pairing_status()
        ble = client.ble_exposure_status()
        usb = client.usb_exposure_status()
        bonds, bond_probe_initialized_ble = _bond_inventory(client)
        pairing = client.ble_pairing_status()
        ble = client.ble_exposure_status()
    finally:
        client.close()
    host = _bluez_safe()
    headless = _headless()
    bond_inventory = [
        {
            "bond_id": bond.bond_id,
            "verified": bond.verified,
            "schema_revision": bond.schema_revision,
            "schema_current": bond.schema_current,
            "connected": bond.connected,
        }
        for bond in bonds.bonds
    ]
    return {
        "identity_match": comparison.match,
        "identity": _plain(comparison),
        "boot_id": hello.boot_id,
        "control_attempts": attempts,
        "profile": profile.selected.value,
        "profile_stable": profile.transition == "stable",
        "route_none": (
            route.desired.value == "none"
            and route.active.value == "none"
            and route.transition == "stable"
            and not route.ready
        ),
        "all_up": (None if release is None else
                   release.keyboard == "already_up" and release.mouse == "already_up"),
        # A new boot retires every sequence. release_all must additionally prove
        # that no carried logical HID state survived that boot boundary.
        "sequence_active": False,
        "pairing_idle": _pairing_is_idle(pairing),
        "ble_safe": _ble_is_safe(ble),
        "bond_probe_initialized_ble": bond_probe_initialized_ble,
        "usb_safe": _usb_is_safe(usb),
        "host_safe": (
            headless["safe"]
            and host["powered"]
            and not host["discoverable"]
            and not host["pairable"]
            and not host["discovering"]
            and host["connected_devices"] == 0
        ),
        "host": host,
        "headless": headless,
        "bond_store_healthy": bonds.healthy,
        "bond_inventory": bond_inventory,
    }


def _controlled_application_reset():
    import serial
    from ble_cleanup_rehearsal import _serial_port

    port = serial.Serial(
        port=None,
        baudrate=115200,
        bytesize=serial.EIGHTBITS,
        parity=serial.PARITY_NONE,
        stopbits=serial.STOPBITS_ONE,
        timeout=0.2,
        write_timeout=1.0,
        xonxoff=False,
        rtscts=False,
        dsrdtr=False,
        exclusive=True,
    )
    try:
        port.dtr = False
        port.rts = False
        port.port = _serial_port(None)
        port.open()
        port.rts = True
        time.sleep(0.1)
        port.rts = False
    finally:
        try:
            port.dtr = False
            port.rts = False
        finally:
            port.close()


def _save_new(path: Path, value) -> None:
    require(not path.exists(), "PREFLIGHT_OUTPUT_EXISTS")
    temporary = path.with_name(path.name + ".pending")
    with temporary.open("x", encoding="utf-8") as stream:
        json.dump(_plain(value), stream, sort_keys=True, separators=(",", ":"))
        stream.write("\n")
        stream.flush()
        os.fsync(stream.fileno())
    temporary.replace(path)


def run(artifact: Path):
    import hashlib

    require(hashlib.sha256(artifact.read_bytes()).hexdigest() == EXPECTED_ARCHIVE,
            "ARTIFACT_AUTHORITY_INVALID")
    return normalize_fixture(
        lambda: _inspect_state(artifact, verify_safety=False),
        _controlled_application_reset,
        lambda: _inspect_state(artifact, verify_safety=True),
    )


def main():
    parser = argparse.ArgumentParser()
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--official-preflight", action="store_true")
    mode.add_argument("--preflight-rehearsal", action="store_true")
    parser.add_argument("--not-qualification", action="store_true")
    parser.add_argument("--artifact", type=Path, required=True)
    parser.add_argument("--evidence", type=Path, required=True)
    args = parser.parse_args()
    if args.preflight_rehearsal:
        require(args.not_qualification, "REHEARSAL_OPT_IN_REQUIRED")
    else:
        require(not args.not_qualification, "OFFICIAL_PREFLIGHT_MODE_INVALID")
    classification = ("OFFICIAL_PREFLIGHT_REHEARSAL / NOT_QUALIFICATION"
                      if args.preflight_rehearsal else "OFFICIAL_PREFLIGHT")
    try:
        value = run(args.artifact)
        value["classification"] = classification
        value["qualification_attempt_consumed"] = False
        _save_new(args.evidence, value)
        return 0
    except Exception as exc:
        value = {
            "classification": classification,
            "qualification_attempt_consumed": False,
            "result": "FAIL",
            "failure": {"type": type(exc).__name__, "message": str(exc),
                        "traceback": traceback.format_exc()},
        }
        _save_new(args.evidence, value)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
