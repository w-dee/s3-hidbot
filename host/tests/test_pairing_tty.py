from __future__ import annotations

import errno
import os
import pty
import select
import subprocess
import sys
import unittest
from pathlib import Path


SECRET = "000123"
PACKAGE_ROOT = Path(__file__).resolve().parents[1]
PYTHONPATH = os.pathsep.join((str(PACKAGE_ROOT / "src"), str(PACKAGE_ROOT)))

CHILD_SETUP = r"""
import json
import sys
from tests.test_cli import FakeTransport
from hidbot.cli import main
from hidbot.framing import FRAME_PREFIX, TRANSPORT_SYNC
from hidbot.protocol import BLE_PAIRING_TRANSACTION_CAPABILITY

calls = []
transport = FakeTransport(calls)
transport.hello_capabilities.append(BLE_PAIRING_TRANSACTION_CAPABILITY)
code = main(
    ["--port", "dummy", "--json", "ble-pairing-respond", "--pairing-id", "27"],
    environ={},
    transport_factory=lambda *args, **kwargs: transport,
)
commands = [
    json.loads(call[1][len(FRAME_PREFIX):-1])["cmd"]
    for call in calls
    if call[0] == "write" and call[1] != TRANSPORT_SYNC
]
expected = ["protocol.hello", "ble.pairing.respond"] if code == 0 else ["protocol.hello"]
raise SystemExit(0 if commands == expected else 97)
"""


def child_environment() -> dict[str, str]:
    environment = dict(os.environ)
    environment["PYTHONPATH"] = PYTHONPATH
    return environment


def read_pty(master: int, *, until: bytes | None = None) -> bytes:
    output = bytearray()
    while True:
        ready, _, _ = select.select([master], [], [], 5.0)
        if not ready:
            break
        try:
            chunk = os.read(master, 4096)
        except OSError as exc:
            if exc.errno == errno.EIO:
                break
            raise
        if not chunk:
            break
        output.extend(chunk)
        if until is not None and until in output:
            break
    return bytes(output)


@unittest.skipUnless(os.name == "posix", "controlling-TTY tests require POSIX")
class PairingTtyTests(unittest.TestCase):
    def test_json_pairing_response_uses_controlling_tty_without_echo(self) -> None:
        stdout_read, stdout_write = os.pipe()
        stderr_read, stderr_write = os.pipe()
        child, master = pty.fork()
        if child == 0:
            os.close(stdout_read)
            os.close(stderr_read)
            os.dup2(stdout_write, sys.stdout.fileno())
            os.dup2(stderr_write, sys.stderr.fileno())
            os.close(stdout_write)
            os.close(stderr_write)
            os.chdir(PACKAGE_ROOT)
            os.execve(
                sys.executable,
                [sys.executable, "-c", CHILD_SETUP],
                child_environment(),
            )
            raise AssertionError("unreachable")
        os.close(stdout_write)
        os.close(stderr_write)
        try:
            tty_output = read_pty(master, until=b"Passkey (6 digits): ")
            self.assertIn(b"Passkey (6 digits): ", tty_output)
            os.write(master, SECRET.encode("ascii") + b"\n")
            _, status = os.waitpid(child, 0)
            tty_output += read_pty(master)
        finally:
            os.close(master)
        with os.fdopen(stdout_read, "rb") as stream:
            stdout = stream.read()
        with os.fdopen(stderr_read, "rb") as stream:
            stderr = stream.read()
        self.assertTrue(os.WIFEXITED(status))
        self.assertEqual(os.WEXITSTATUS(status), 0)
        self.assertEqual(stdout, b'{"accepted":true,"pairing_id":27}\n')
        self.assertEqual(stderr, b"")
        for captured in (tty_output, stdout, stderr):
            self.assertNotIn(SECRET.encode("ascii"), captured)

    def test_no_controlling_tty_does_not_fall_back_to_stdin_or_send_request(self) -> None:
        process = subprocess.run(
            [sys.executable, "-c", CHILD_SETUP],
            cwd=PACKAGE_ROOT,
            env=child_environment(),
            input=(SECRET + "\n").encode("ascii"),
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            start_new_session=True,
            timeout=10,
            check=False,
        )
        self.assertEqual(process.returncode, 0)
        self.assertEqual(process.stdout, b"")
        self.assertIn(b"controlling TTY", process.stderr)
        self.assertNotIn(SECRET.encode("ascii"), process.stderr)


if __name__ == "__main__":
    unittest.main()
