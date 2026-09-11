#!/usr/bin/env python3
"""Static boundaries for the U4.1 HID runtime foundation."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
RUNTIME = ROOT / "firmware/components/hid_runtime/hid_runtime.cpp"
RUNTIME_HEADER = ROOT / "firmware/components/hid_runtime/include/hid_runtime/hid_runtime.hpp"
ROUTE = ROOT / "firmware/components/hid_route/hid_route.cpp"
ROUTE_HEADER = ROOT / "firmware/components/hid_route/include/hid_route/hid_route.hpp"
MAIN = ROOT / "firmware/main/main.cpp"
PROTOCOL = ROOT / "firmware/components/control_protocol/control_protocol.cpp"
TRANSPORT = ROOT / "firmware/components/uart_control_transport/uart_control_transport.cpp"
SDKCONFIG = ROOT / "firmware/sdkconfig.defaults"


def main() -> int:
    runtime = RUNTIME.read_text(encoding="utf-8")
    header = RUNTIME_HEADER.read_text(encoding="utf-8")
    route = ROUTE.read_text(encoding="utf-8")
    route_header = ROUTE_HEADER.read_text(encoding="utf-8")
    main_source = MAIN.read_text(encoding="utf-8")
    protocol = PROTOCOL.read_text(encoding="utf-8")
    transport = TRANSPORT.read_text(encoding="utf-8")
    sdkconfig = SDKCONFIG.read_text(encoding="utf-8")

    mount_start = runtime.index("void Runtime::on_mount()")
    mount_open = runtime.index("{", mount_start)
    depth = 0
    mount_close = None
    for index in range(mount_open, len(runtime)):
        if runtime[index] == "{":
            depth += 1
        elif runtime[index] == "}":
            depth -= 1
            if depth == 0:
                mount_close = index
                break
    assert mount_close is not None
    mount_body = runtime[mount_open:mount_close]

    assert "tud_hid_n_report(instance, 0, report, length)" in runtime
    assert runtime.count("tud_hid_n_report(") == 1
    assert "tud_hid_n_keyboard_report" not in runtime
    assert "tud_hid_n_mouse_report" not in runtime
    assert "tud_hid_n_report" not in main_source
    assert "tud_hid_n_keyboard_report" not in (main_source + protocol + transport)
    assert "tud_hid_n_mouse_report" not in (main_source + protocol + transport)
    assert "extern \"C\" void tud_sof_cb" in main_source
    assert "service_sof()" in main_source
    sof_start = runtime.index("void Runtime::enable_sof_after_mount()")
    sof_end = runtime.index("void Runtime::on_unmount()", sof_start)
    sof_body = runtime[sof_start:sof_end]
    assert "tud_sof_cb_enable(true)" not in mount_body
    assert sof_body.count("tud_sof_cb_enable(true)") == 1
    attached_start = main_source.index("case TINYUSB_EVENT_ATTACHED:")
    attached_end = main_source.index("break;", attached_start)
    attached_body = main_source[attached_start:attached_end]
    assert attached_body.rstrip().endswith("s_hid_runtime.enable_sof_after_mount();")
    assert attached_body.index("s_hid_runtime.on_mount()") < attached_body.index(
        "s_hid_runtime.enable_sof_after_mount()"
    )
    assert "on_report_complete" in main_source and "on_report_failed" in main_source
    assert "report_type == HID_REPORT_TYPE_INPUT" in main_source
    assert "ReportOriginOwnerId originating_local_owner_id = 0" in main_source
    assert "&originating_local_owner_id" in main_source
    assert "uart_control_transport::on_hid_safety_failure(" in main_source
    assert "s_published_local_owner" not in main_source
    assert "on_hid_lifecycle_invalidation()" in main_source
    assert "authority_epoch_" in header
    assert "slot_authority_epoch" in header
    assert "in_flight_authority_epoch" in header
    assert "std::atomic<AuthorityEpoch>::is_always_lock_free" in header
    assert "authority_epoch_.fetch_add(1, std::memory_order_acq_rel);" in runtime
    assert runtime.count("authority_epoch_.fetch_add(1, std::memory_order_acq_rel);") >= 4
    assert "slot_authority_epoch != current_authority_epoch" in runtime
    assert "in_flight_authority_epoch != authority_epoch()" in runtime
    assert "preserve_suspend_safety" in runtime
    assert "any_safety_required" in runtime
    assert "HidTransport" in header and "RouteGeneration" in header
    assert "slot_transport_generation" in header
    assert "slot_route_generation" in header
    assert "in_flight_route_generation" in header
    assert "route_generation" in header and "transport_generation" in header
    assert "hid_route/hid_route.hpp" in header
    assert "unsafe_route_active" in runtime
    assert "active_route.active == hid_route::OutputRoute::kUsb" in runtime
    assert "route_.invalidate_if_matches(active_route)" in runtime
    assert "request_route_ble" in runtime
    assert "ble_work_token_current" in runtime
    assert "process_ble_report" in runtime
    for field in (
        "connection_handle",
        "characteristic_handle",
        "report_kind",
        "ticket_id",
    ):
        assert field in header
    assert "commit_usb_if_none" in runtime
    assert "OutputRoute" in route_header
    assert "kBle" in route_header
    assert "std::uint32_t" in route_header
    assert "commit_none_locked" in route
    assert "invalidation_pending_" in route
    assert "request_release_all" in header
    assert "ReleaseAllTicket" in header
    assert "begin_release_all" in runtime
    assert "release_all_snapshot" in runtime
    assert "finalize_release_all" in runtime
    assert "kReleaseAllWaitTicks" in runtime
    assert "kReleaseAllPollTicks" in runtime
    assert "logical_state_held" in header
    assert "host_state_uncertain" in header
    assert "KeyboardReportTicket" in header
    assert "kWritingCanceled" in header
    assert "kPublished" in header and "kClaimed" in header and "kCanceled" in header
    assert "begin_keyboard_report" in header
    assert "cancel_keyboard_report" in runtime
    assert "confirmed_sequence" in header
    assert "confirmed_keyboard_equals" in runtime
    assert "MouseReportTicket" in header
    assert "begin_mouse_report" in header
    assert "process_mouse_ticket" in runtime
    assert "confirmed_mouse_buttons" in header
    assert "using HidTicketId = std::uint64_t;" in header
    assert "using ReportOriginOwnerId = std::uint64_t;" in header
    assert "originating_local_owner_id" in header
    assert "in_flight_originating_local_owner_id" in header
    assert "std::atomic<HidTicketId>" not in header
    assert "TicketMetadataLock" in header
    assert "keyboard_ticket_lock_" in header and "mouse_ticket_lock_" in header
    assert "cancel_keyboard_report(HidTicketId ticket_id);" in header
    assert "cancel_mouse_report(HidTicketId ticket_id);" in header
    keyboard_complete_start = runtime.index(
        "KeyboardReportResult Runtime::complete_keyboard_report("
    )
    keyboard_complete_end = runtime.index(
        "MouseReportResult Runtime::mouse_report(", keyboard_complete_start
    )
    keyboard_complete = runtime[keyboard_complete_start:keyboard_complete_end]
    mouse_complete_start = runtime.index(
        "MouseReportResult Runtime::complete_mouse_report("
    )
    mouse_complete_end = runtime.index(
        "void Runtime::request_release_all()", mouse_complete_start
    )
    mouse_complete = runtime[mouse_complete_start:mouse_complete_end]
    for complete, wait_name, poll_name in (
        (keyboard_complete, "kKeyboardReportWaitTicks", "kKeyboardReportPollTicks"),
        (mouse_complete, "kMouseReportWaitTicks", "kMouseReportPollTicks"),
    ):
        terminal_start = complete.index("kNotReady ||")
        terminal_end = complete.index("kPublished)", terminal_start)
        terminal_path = complete[terminal_start:terminal_end]
        assert f"xTaskGetTickCount() - wait_start >= {wait_name}" in terminal_path
        assert f"vTaskDelay({poll_name})" in terminal_path
    assert "static_cast<std::uint8_t>(Interface::kMouse)" in runtime
    assert "std::memcpy(report.data(), keyboard_ticket_.report, report.size())" in runtime
    assert "std::memcpy(report.data(), mouse_ticket_.report, report.size())" in runtime
    assert "tud_hid_n_report(instance, 0, report, length)" in runtime
    assert "GPIO_NUM_19" not in (runtime + header + main_source)
    assert "GPIO_NUM_20" not in (runtime + header + main_source)
    assert "CONFIG_TINYUSB_HID_COUNT=2" in sdkconfig
    assert "hid.lease-v1" in protocol
    assert "hid.mouse-report-v1" in protocol
    assert '\\"lease_ms\\":%lu' in protocol
    print("PASS: HID runtime task-affinity/lifecycle/safety static contract")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
