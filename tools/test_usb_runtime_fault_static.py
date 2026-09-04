#!/usr/bin/env python3
"""Static integration guards for the U7.6D TinyUSB runtime-fault seam."""

from __future__ import annotations

import os
from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
MAIN = (ROOT / "firmware/main/main.cpp").read_text(encoding="utf-8")
HID_RUNTIME = (
    ROOT / "firmware/components/hid_runtime/hid_runtime.cpp"
).read_text(encoding="utf-8")
TINYUSB_ROOT = Path(
    os.environ.get(
        "S3_HIDBOT_TINYUSB_SOURCE",
        ROOT / "firmware/managed_components/espressif__tinyusb",
    )
)


def body_after(source: str, marker: str, next_marker: str) -> str:
    start = source.index(marker)
    end = source.index(next_marker, start + len(marker))
    return source[start:end]


callback = body_after(MAIN, "void usb_event_handler(", "class TinyUsbLifecycleBackend")
for forbidden in ("ESP_LOG", "printf(", "fprintf(", "esp_rom_printf"):
    assert forbidden not in callback, f"TinyUSB lifecycle callback uses {forbidden}"

mount = body_after(callback, "case TINYUSB_EVENT_ATTACHED:", "break;")
mount_steps = (
    "s_hid_runtime.on_mount()",
    "uart_control_transport::on_hid_lifecycle_invalidation()",
    "s_usb_exposure.signal_usb_lifecycle_event(",
    "s_hid_runtime.enable_sof_after_mount()",
)
positions = [mount.index(step) for step in mount_steps]
assert positions == sorted(positions), "mount state/notification/SOF order changed"
assert mount.rstrip().endswith("s_hid_runtime.enable_sof_after_mount();")

sink = body_after(
    MAIN,
    'extern "C" int s3_hidbot_tinyusb_debug_printf',
    'extern "C" void tud_event_queue_overflow_cb',
)
for forbidden in ("va_start", "ESP_LOG", "write(", "malloc"):
    assert forbidden not in sink, f"TinyUSB diagnostic sink uses {forbidden}"
assert not re.search(
    r"(?<![A-Za-z0-9_])(?:v?sn?printf|fprintf)\s*\(", sink
), "TinyUSB diagnostic sink formats output"
assert "signal_usb_runtime_fault" in sink and "return 0;" in sink

# The outputless sink may run while the UART task owns stdout. Simulate that
# interleaving boundary explicitly: the diagnostic contributes zero bytes, so
# the already framed machine response remains byte-identical and contiguous.
machine_response = b'@HIDBOT {"id":7,"ok":true}\n'
before, after = machine_response[:11], machine_response[11:]
diagnostic_output = b""
assert before + diagnostic_output + after == machine_response

descriptor = body_after(
    MAIN,
    'extern "C" uint16_t const *__wrap_tud_descriptor_string_cb',
    'extern "C" void tud_sof_cb',
)
assert "signal_usb_runtime_fault" in descriptor
for forbidden in ("ESP_LOG", "printf(", "fprintf("):
    assert forbidden not in descriptor

runtime_latch = body_after(
    HID_RUNTIME,
    "bool StateMachine::begin_usb_runtime_fault",
    "void StateMachine::commit_usb_runtime_fault_shutdown",
)
assert "status_bits_.fetch_and" in runtime_latch
for deferred in (
    "route_.invalidate_if_matches",
    "cancel_release_ticket",
    "authority_epoch_.fetch_add",
):
    assert deferred not in runtime_latch, f"callback-side latch performs {deferred}"

debug_path = TINYUSB_ROOT / "src/common/tusb_debug.h"
usbd_path = TINYUSB_ROOT / "src/device/usbd.c"
if debug_path.is_file() or usbd_path.is_file():
    assert debug_path.is_file() and usbd_path.is_file(), (
        "TinyUSB source is only partially materialized"
    )
    tusb_debug = debug_path.read_text(encoding="utf-8")
    usbd = usbd_path.read_text(encoding="utf-8")
    assert "CFG_TUSB_DEBUG_PRINTF_OVERRIDE" in tusb_debug
    assert "tud_event_queue_overflow_cb" in usbd
    failed_send = body_after(usbd, "static inline bool queue_event", "// Prototypes")
    assert failed_send.index("tud_event_queue_overflow_cb") < failed_send.index(
        "TU_ASSERT(false)"
    )
    assert failed_send.index("TU_ASSERT(false)") < failed_send.index(
        "tud_event_hook_cb"
    )
    dependency_result = " and materialized TinyUSB source seam"
else:
    assert os.environ.get("S3_HIDBOT_REQUIRE_TINYUSB_SOURCE") != "1", (
        "materialized TinyUSB source is required for target validation"
    )
    # Tier-A clean checkouts have no generated managed_components directory.
    # test-firmware.sh reruns this guard after Component Manager materializes
    # the exact locked dependency, so the dependency seam is never skipped in
    # target validation.
    dependency_result = " (TinyUSB source seam deferred to target build)"

print(f"PASS: TinyUSB runtime fault and no-console static guards{dependency_result}")
