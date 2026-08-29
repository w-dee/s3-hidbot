#!/usr/bin/env python3
"""Static checks for the configured-console machine writer contract."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "firmware/components/uart_control_transport/uart_control_transport.cpp"
HEADER = ROOT / "firmware/components/uart_control_transport/include/uart_control_transport/uart_control_transport.hpp"


def main() -> int:
    source = SOURCE.read_text(encoding="utf-8")
    header = HEADER.read_text(encoding="utf-8")
    assert "kMaxLogicalMachineFrameBytes = 1023" in header
    assert "kMaxWireMachineFrameBytes = 1024" in header

    writer_start = source.index("bool write_machine")
    writer_end = source.index("esp_err_t start(", writer_start)
    writer = source[writer_start:writer_end]
    assert "length > kMaxLogicalMachineFrameBytes" in writer
    assert "ESP_LOG" not in writer
    assert "printf(" not in writer
    assert "uart_write_bytes(" not in writer

    lock = writer.index("::flockfile(stdout)")
    flush = writer.index("std::fflush(stdout)")
    fileno = writer.index("::fileno(stdout)")
    vfs_write = writer.index("::write(stdout_fd, data, length)")
    unlock = writer.index("::funlockfile(stdout)")
    assert lock < flush < fileno < vfs_write < unlock

    assert "uart_write_bytes(" not in source

    assert "uart_is_driver_installed(kConsoleUart)" in source
    assert "uart_driver_install(kConsoleUart" in source
    assert "uart_vfs_dev_use_driver(kConsoleUart)" in source
    assert "uart_read_bytes(kConsoleUart" in source
    print("PASS: UART machine writer static contract")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
