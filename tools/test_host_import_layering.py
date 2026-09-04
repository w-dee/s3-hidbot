#!/usr/bin/env python3
"""Keep hardware-independent host imports independent of pyserial."""

from __future__ import annotations

import os
from pathlib import Path
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]


def main() -> int:
    program = r'''\
import sys

import hidbot
assert "hidbot.serial_transport" not in sys.modules
from hidbot import protocol
import qualification_harness

assert protocol.PROTOCOL_VERSION == 1
assert qualification_harness.QualificationError is not None
assert "hidbot.serial_transport" not in sys.modules
assert "PySerialTransport" in hidbot.__all__

try:
    hidbot.PySerialTransport
except ModuleNotFoundError as exc:
    assert exc.name == "serial", exc
else:
    raise AssertionError("PySerialTransport unexpectedly loaded without pyserial")
'''
    environment = os.environ.copy()
    environment["PYTHONPATH"] = os.pathsep.join(
        (str(ROOT / "host" / "src"), str(ROOT / "tools"))
    )
    result = subprocess.run(
        [sys.executable, "-S", "-c", program],
        cwd=ROOT,
        env=environment,
        check=False,
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        raise AssertionError(
            "hardware-independent imports require pyserial:\n"
            + result.stdout
            + result.stderr
        )
    print("PASS: host protocol and qualification imports are pyserial-independent")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
