#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>

#include "hid_control_executor/hid_control_executor.hpp"

namespace {

using hid_control_executor::ControlOperation;

struct FakeBackend final : hid_control_executor::Backend {
    hid_control_executor::BackendResult install() override {
        ++install_calls;
        return install_result;
    }

    hid_control_executor::BackendResult uninstall() override {
        ++uninstall_calls;
        return uninstall_result;
    }

    int install_calls = 0;
    int uninstall_calls = 0;
    hid_control_executor::BackendResult install_result{
        .kind = hid_control_executor::BackendResultKind::kSuccess,
        .error_code = 0,
    };
    hid_control_executor::BackendResult uninstall_result{
        .kind = hid_control_executor::BackendResultKind::kSuccess,
        .error_code = 0,
    };
};

struct FakeBleBackend final : hid_control_executor::BleBackend {
    std::int32_t initialize(hid_control_executor::BleEventSink *event_sink,
                            hid_control_executor::BleDatabase *,
                            ble_lifecycle::Generation generation) override {
        ++initialize_calls;
        sink = event_sink;
        active_generation = generation;
        return initialize_result;
    }
    void set_generation(ble_lifecycle::Generation generation) override {
        active_generation = generation;
    }
    std::int32_t start_advertising() override {
        ++advertising_calls;
        return advertising_result;
    }
    std::int32_t stop_advertising() override {
        ++stop_calls;
        return stop_result;
    }
    std::int32_t disconnect(std::uint16_t connection_handle) override {
        ++disconnect_calls;
        last_connection = connection_handle;
        return disconnect_result;
    }
    std::int32_t configure_connection(std::uint16_t connection_handle) override {
        ++configure_connection_calls;
        last_configured_connection = connection_handle;
        return configure_connection_result;
    }
    void record_heap_checkpoint(HeapCheckpoint checkpoint) override {
        ++heap_checkpoint_calls;
        last_heap_checkpoint = checkpoint;
    }
    bool event(hid_control_executor::BleEventKind kind,
               std::uint16_t connection = ble_lifecycle::kNoConnection,
               std::int32_t status = 0) {
        return sink->signal_ble_event({.kind = kind,
                                      .generation = active_generation,
                                      .connection_handle = connection,
                                      .status = status});
    }
    hid_control_executor::BleEventSink *sink = nullptr;
    ble_lifecycle::Generation active_generation = 0;
    std::uint16_t last_connection = ble_lifecycle::kNoConnection;
    std::uint16_t last_configured_connection = ble_lifecycle::kNoConnection;
    int initialize_calls = 0;
    int advertising_calls = 0;
    int stop_calls = 0;
    int disconnect_calls = 0;
    int configure_connection_calls = 0;
    int heap_checkpoint_calls = 0;
    std::int32_t initialize_result = 0;
    std::int32_t advertising_result = 0;
    std::int32_t stop_result = 0;
    std::int32_t disconnect_result = 0;
    std::int32_t configure_connection_result = 0;
    HeapCheckpoint last_heap_checkpoint = HeapCheckpoint::kColdBoot;
};

struct FakeBleDatabase final : hid_control_executor::BleDatabase {
    int register_database() override { return 0; }
    void clear_peer_state() override { ++clear_calls; }
    void on_subscribe(std::uint16_t, bool) override {}
    int clear_calls = 0;
};

struct AcceptingExecutor final : usb_lifecycle::Executor {
    bool schedule(usb_lifecycle::ExecutorAction, usb_lifecycle::Snapshot) override {
        ++calls;
        return true;
    }
    int calls = 0;
};

struct ReportSink {
    int calls = 0;
    std::array<std::uint8_t, 8> report{};

    static bool submit(void *context, std::uint8_t,
                       const std::uint8_t *report, std::uint16_t length) {
        auto *sink = static_cast<ReportSink *>(context);
        ++sink->calls;
        std::memcpy(sink->report.data(), report, length);
        return true;
    }
};

usb_lifecycle::TransitionResult action(hid_control_executor::CommandOutcome outcome) {
    return outcome.action_result;
}

bool same_lifecycle(const usb_lifecycle::Snapshot &left,
                    const usb_lifecycle::Snapshot &right) {
    return left.desired == right.desired && left.observed == right.observed &&
           left.generation == right.generation &&
           left.uncertainty_generation == right.uncertainty_generation &&
           left.safety_pending == right.safety_pending &&
           left.host_release_uncertain == right.host_release_uncertain &&
           left.recovery_required == right.recovery_required &&
           left.last_error.present == right.last_error.present &&
           left.last_error.operation == right.last_error.operation &&
           left.last_error.code == right.last_error.code;
}

void complete_attach(hid_runtime::Runtime &runtime, FakeBackend &backend,
                     hid_control_executor::Controller &controller) {
    assert(controller.initialize(&runtime, &backend));
    assert(action(controller.request_attach()) == usb_lifecycle::TransitionResult::kAccepted);
    assert(controller.active_operation_for_test() == ControlOperation::kUsbAttach);
    assert(controller.process_one_for_test());
    assert(controller.active_operation_for_test() == ControlOperation::kNone);
}

void mount_ready(hid_runtime::Runtime &runtime) {
    runtime.state_machine().on_mount();
    runtime.state_machine().set_ready(hid_runtime::Interface::kKeyboard, true);
    runtime.state_machine().set_ready(hid_runtime::Interface::kMouse, true);
}

void hold_f24(hid_runtime::Runtime &runtime, ReportSink &sink) {
    if (runtime.state_machine().route_snapshot().active == hid_route::OutputRoute::kNone) {
        assert(runtime.state_machine().request_route_usb().action_result ==
               hid_runtime::RouteTransitionResult::kAccepted);
    }
    const std::array<std::uint8_t, 6> f24 = {115, 0, 0, 0, 0, 0};
    assert(runtime.state_machine().queue_keyboard_report(0, f24));
    runtime.state_machine().execute(ReportSink::submit, &sink);
    assert(sink.calls == 1 && sink.report[2] == 115);
    assert(runtime.state_machine().report_complete(0));
}

void test_install_and_uninstall_are_task_owned_and_serialized() {
    hid_runtime::Runtime runtime;
    FakeBackend backend;
    hid_control_executor::Controller controller;
    assert(controller.initialize(&runtime, &backend));

    const auto accepted_attach = controller.request_attach();
    assert(action(accepted_attach) == usb_lifecycle::TransitionResult::kAccepted);
    assert(accepted_attach.snapshot_valid);
    assert(accepted_attach.snapshot.lifecycle.observed ==
           usb_lifecycle::ObservedState::kAttaching);
    assert(controller.active_operation_for_test() == ControlOperation::kUsbAttach);
    assert(action(controller.request_attach()) == usb_lifecycle::TransitionResult::kBusy);
    assert(action(controller.request_detach()) == usb_lifecycle::TransitionResult::kBusy);
    assert(backend.install_calls == 0);
    assert(controller.process_one_for_test());
    assert(backend.install_calls == 1);
    assert(controller.active_operation_for_test() == ControlOperation::kNone);

    mount_ready(runtime);
    const auto old_generation = controller.snapshot().lifecycle.generation;
    const auto accepted_detach = controller.request_detach();
    assert(action(accepted_detach) == usb_lifecycle::TransitionResult::kAccepted);
    assert(accepted_detach.snapshot_valid);
    assert(accepted_detach.snapshot.lifecycle.observed ==
           usb_lifecycle::ObservedState::kDetaching);
    assert(controller.active_operation_for_test() == ControlOperation::kUsbDetach);
    assert(action(controller.request_detach()) == usb_lifecycle::TransitionResult::kBusy);
    assert(action(controller.request_attach()) == usb_lifecycle::TransitionResult::kBusy);
    assert(backend.uninstall_calls == 0);
    assert(controller.process_one_for_test());
    assert(backend.uninstall_calls == 1);
    assert(controller.active_operation_for_test() == ControlOperation::kNone);
    const auto hidden = controller.snapshot();
    assert(hidden.lifecycle.observed == usb_lifecycle::ObservedState::kDriverNotInstalled);
    assert(hidden.lifecycle.generation == old_generation + 1);
    assert(!hidden.lifecycle.recovery_required);
}

void test_route_owner_blocks_attach_before_lifecycle_stage_a() {
    hid_runtime::Runtime runtime;
    FakeBackend backend;
    hid_control_executor::Controller controller;
    assert(controller.initialize(&runtime, &backend));
    assert(controller.reserve_operation_for_test(ControlOperation::kRouteChange));
    const auto before = controller.snapshot().lifecycle;

    assert(action(controller.request_attach()) == usb_lifecycle::TransitionResult::kBusy);
    assert(same_lifecycle(before, controller.snapshot().lifecycle));
    assert(backend.install_calls == 0);
    assert(!controller.process_one_for_test());
    controller.release_operation_for_test(ControlOperation::kRouteChange);
    assert(controller.active_operation_for_test() == ControlOperation::kNone);
}

void test_route_owner_blocks_detach_before_lifecycle_stage_a() {
    hid_runtime::Runtime runtime;
    FakeBackend backend;
    hid_control_executor::Controller controller;
    complete_attach(runtime, backend, controller);
    mount_ready(runtime);
    assert(controller.reserve_operation_for_test(ControlOperation::kRouteChange));
    const auto before = controller.snapshot().lifecycle;

    assert(action(controller.request_detach()) == usb_lifecycle::TransitionResult::kBusy);
    assert(same_lifecycle(before, controller.snapshot().lifecycle));
    assert(backend.uninstall_calls == 0);
    assert(!controller.process_one_for_test());
    controller.release_operation_for_test(ControlOperation::kRouteChange);
    assert(controller.active_operation_for_test() == ControlOperation::kNone);
}

void test_usb_owner_blocks_route_reservation() {
    {
        hid_runtime::Runtime runtime;
        FakeBackend backend;
        hid_control_executor::Controller controller;
        assert(controller.initialize(&runtime, &backend));
        assert(action(controller.request_attach()) == usb_lifecycle::TransitionResult::kAccepted);
        assert(controller.active_operation_for_test() == ControlOperation::kUsbAttach);
        assert(!controller.reserve_operation_for_test(ControlOperation::kRouteChange));
        assert(controller.process_one_for_test());
    }
    {
        hid_runtime::Runtime runtime;
        FakeBackend backend;
        hid_control_executor::Controller controller;
        complete_attach(runtime, backend, controller);
        mount_ready(runtime);
        assert(action(controller.request_detach()) == usb_lifecycle::TransitionResult::kAccepted);
        assert(controller.active_operation_for_test() == ControlOperation::kUsbDetach);
        assert(!controller.reserve_operation_for_test(ControlOperation::kRouteChange));
        assert(controller.process_one_for_test());
    }
}

void test_no_op_and_ordinary_rejection_release_guard() {
    {
        hid_runtime::Runtime runtime;
        FakeBackend backend;
        hid_control_executor::Controller controller;
        assert(controller.initialize(&runtime, &backend));
        assert(action(controller.request_detach()) == usb_lifecycle::TransitionResult::kNoOp);
        assert(controller.active_operation_for_test() == ControlOperation::kNone);
        assert(!controller.process_one_for_test());
    }
    {
        hid_runtime::Runtime runtime;
        FakeBackend backend;
        hid_control_executor::Controller controller;
        complete_attach(runtime, backend, controller);
        assert(action(controller.request_attach()) == usb_lifecycle::TransitionResult::kNoOp);
        assert(controller.active_operation_for_test() == ControlOperation::kNone);
        assert(!controller.process_one_for_test());
    }
    {
        hid_runtime::Runtime runtime;
        FakeBackend backend;
        hid_control_executor::Controller controller;
        AcceptingExecutor external;
        assert(controller.initialize(&runtime, &backend));
        assert(runtime.state_machine().request_usb_attach(external).action_result ==
               usb_lifecycle::TransitionResult::kAccepted);
        const auto before = controller.snapshot().lifecycle;
        assert(action(controller.request_detach()) == usb_lifecycle::TransitionResult::kBusy);
        assert(same_lifecycle(before, controller.snapshot().lifecycle));
        assert(controller.active_operation_for_test() == ControlOperation::kNone);
    }
}

void test_schedule_requires_matching_preclaimed_owner() {
    hid_runtime::Runtime runtime;
    FakeBackend backend;
    hid_control_executor::Controller controller;
    assert(controller.initialize(&runtime, &backend));
    const auto snapshot = controller.snapshot().lifecycle;

    assert(!controller.schedule(usb_lifecycle::ExecutorAction::kInstall, snapshot));
    assert(controller.reserve_operation_for_test(ControlOperation::kRouteChange));
    assert(!controller.schedule(usb_lifecycle::ExecutorAction::kInstall, snapshot));
    controller.release_operation_for_test(ControlOperation::kRouteChange);
    assert(controller.reserve_operation_for_test(ControlOperation::kUsbAttach));
    assert(controller.schedule(usb_lifecycle::ExecutorAction::kInstall, snapshot));
    assert(controller.process_one_for_test());
    assert(backend.install_calls == 0);
    assert(controller.active_operation_for_test() == ControlOperation::kNone);
}

void test_backend_failures_release_guard() {
    for (const auto kind : {hid_control_executor::BackendResultKind::kCleanInstallFailure,
                            hid_control_executor::BackendResultKind::kAmbiguousInstallFailure}) {
        hid_runtime::Runtime runtime;
        FakeBackend backend;
        hid_control_executor::Controller controller;
        assert(controller.initialize(&runtime, &backend));
        backend.install_result = {.kind = kind, .error_code = -99};
        assert(action(controller.request_attach()) == usb_lifecycle::TransitionResult::kAccepted);
        assert(controller.process_one_for_test());
        assert(controller.active_operation_for_test() == ControlOperation::kNone);
        const auto snapshot = controller.snapshot().lifecycle;
        assert(snapshot.last_error.present && snapshot.last_error.code == -99);
        assert(snapshot.recovery_required ==
               (kind == hid_control_executor::BackendResultKind::kAmbiguousInstallFailure));
    }

    hid_runtime::Runtime runtime;
    FakeBackend backend;
    hid_control_executor::Controller controller;
    complete_attach(runtime, backend, controller);
    mount_ready(runtime);
    backend.uninstall_result = {
        .kind = hid_control_executor::BackendResultKind::kUninstallFailure,
        .error_code = -77,
    };
    assert(action(controller.request_detach()) == usb_lifecycle::TransitionResult::kAccepted);
    assert(controller.process_one_for_test());
    assert(controller.active_operation_for_test() == ControlOperation::kNone);
    const auto failed = controller.snapshot().lifecycle;
    assert(failed.recovery_required);
    assert(failed.last_error.present && failed.last_error.code == -77);
}

void test_real_queue_failure_is_recovery_fault_and_releases_guard() {
    hid_runtime::Runtime runtime;
    FakeBackend backend;
    hid_control_executor::Controller controller;
    assert(controller.initialize(&runtime, &backend));
    controller.fail_next_enqueue_for_test();

    assert(action(controller.request_attach()) == usb_lifecycle::TransitionResult::kBusy);
    const auto failed = controller.snapshot().lifecycle;
    assert(failed.desired == usb_lifecycle::DesiredExposure::kExposed);
    assert(failed.observed == usb_lifecycle::ObservedState::kAttaching);
    assert(failed.generation == 1);
    assert(failed.recovery_required);
    assert(failed.last_error.present && failed.last_error.code == -1);
    assert(controller.active_operation_for_test() == ControlOperation::kNone);
    assert(!controller.process_one_for_test());
    assert(backend.install_calls == 0);
    assert(action(controller.request_attach()) == usb_lifecycle::TransitionResult::kBusy);
    assert(controller.active_operation_for_test() == ControlOperation::kNone);
}

void test_callback_invalidation_obsoletes_action_and_releases_guard() {
    hid_runtime::Runtime runtime;
    FakeBackend backend;
    hid_control_executor::Controller controller;
    assert(controller.initialize(&runtime, &backend));
    const auto authority_before = runtime.state_machine().authority_epoch();
    assert(action(controller.request_attach()) == usb_lifecycle::TransitionResult::kAccepted);
    const auto accepted_generation = controller.snapshot().lifecycle.generation;
    assert(controller.active_operation_for_test() == ControlOperation::kUsbAttach);

    runtime.state_machine().on_unmount();
    assert(runtime.state_machine().authority_epoch() != authority_before);
    assert(controller.snapshot().lifecycle.generation != accepted_generation);
    assert(controller.active_operation_for_test() == ControlOperation::kUsbAttach);
    assert(controller.process_one_for_test());
    assert(backend.install_calls == 0);
    assert(controller.active_operation_for_test() == ControlOperation::kNone);
}

void test_route_zero_work_is_synchronous_and_never_uninstalls_usb() {
    hid_runtime::Runtime runtime;
    FakeBackend backend;
    hid_control_executor::Controller controller;
    complete_attach(runtime, backend, controller);
    mount_ready(runtime);
    assert(controller.request_route(hid_route::OutputRoute::kUsb).action_result ==
           hid_runtime::RouteTransitionResult::kAccepted);
    const auto usb_generation = controller.snapshot().lifecycle.generation;

    const auto release = controller.request_route(hid_route::OutputRoute::kNone);
    assert(release.action_result == hid_runtime::RouteTransitionResult::kAccepted);
    assert(release.snapshot_valid);
    assert(release.snapshot.route.desired == hid_route::OutputRoute::kNone);
    assert(release.snapshot.route.active == hid_route::OutputRoute::kUsb);
    assert(release.snapshot.route.transition == hid_route::Transition::kReleasing);
    assert(controller.active_operation_for_test() == ControlOperation::kNone);
    assert(!controller.process_one_for_test());
    assert(backend.uninstall_calls == 0);
    assert(controller.snapshot().lifecycle.generation == usb_generation);
    assert(controller.snapshot().lifecycle.observed ==
           usb_lifecycle::ObservedState::kMounted);
    assert(controller.route_snapshot().route.active == hid_route::OutputRoute::kNone);

    const auto select = controller.request_route(hid_route::OutputRoute::kUsb);
    assert(select.action_result == hid_runtime::RouteTransitionResult::kAccepted);
    assert(select.snapshot.ready);
    assert(backend.install_calls == 1);
    assert(controller.snapshot().lifecycle.generation == usb_generation);
}

void test_route_release_owns_guard_and_blocks_detach_before_stage_a() {
    hid_runtime::Runtime runtime;
    FakeBackend backend;
    hid_control_executor::Controller controller;
    ReportSink sink;
    complete_attach(runtime, backend, controller);
    mount_ready(runtime);
    hold_f24(runtime, sink);
    const auto lifecycle_before = controller.snapshot().lifecycle;

    const auto release = controller.request_route(hid_route::OutputRoute::kNone);
    assert(release.action_result == hid_runtime::RouteTransitionResult::kAccepted);
    assert(controller.active_operation_for_test() == ControlOperation::kRouteChange);
    assert(action(controller.request_detach()) == usb_lifecycle::TransitionResult::kBusy);
    assert(same_lifecycle(lifecycle_before, controller.snapshot().lifecycle));
    assert(controller.process_one_for_test());
    assert(controller.active_operation_for_test() == ControlOperation::kNone);
    assert(controller.route_snapshot().route.active == hid_route::OutputRoute::kNone);
    assert(controller.snapshot().lifecycle.host_release_uncertain);
    assert(backend.uninstall_calls == 0);
    assert(!controller.process_one_for_test());
}

void test_usb_detach_owner_blocks_route_without_duplicate_action() {
    hid_runtime::Runtime runtime;
    FakeBackend backend;
    hid_control_executor::Controller controller;
    complete_attach(runtime, backend, controller);
    mount_ready(runtime);
    assert(action(controller.request_detach()) == usb_lifecycle::TransitionResult::kAccepted);
    const auto route = controller.request_route(hid_route::OutputRoute::kNone);
    assert(route.action_result == hid_runtime::RouteTransitionResult::kBusy);
    assert(controller.process_one_for_test());
    assert(backend.uninstall_calls == 1);
    assert(!controller.process_one_for_test());
}

void test_route_schedule_failure_terminalizes_none_and_releases_guard() {
    hid_runtime::Runtime runtime;
    FakeBackend backend;
    hid_control_executor::Controller controller;
    ReportSink sink;
    complete_attach(runtime, backend, controller);
    mount_ready(runtime);
    hold_f24(runtime, sink);
    controller.fail_next_enqueue_for_test();

    const auto release = controller.request_route(hid_route::OutputRoute::kNone);
    assert(release.action_result == hid_runtime::RouteTransitionResult::kBusy);
    assert(!release.snapshot_valid);
    assert(controller.active_operation_for_test() == ControlOperation::kNone);
    assert(controller.route_snapshot().route.active == hid_route::OutputRoute::kNone);
    assert(controller.snapshot().lifecycle.host_release_uncertain);
    assert(!controller.process_one_for_test());
}

void test_unmount_preempts_route_action_and_guard_releases_once() {
    hid_runtime::Runtime runtime;
    FakeBackend backend;
    hid_control_executor::Controller controller;
    ReportSink sink;
    complete_attach(runtime, backend, controller);
    mount_ready(runtime);
    hold_f24(runtime, sink);
    assert(controller.request_route(hid_route::OutputRoute::kNone).action_result ==
           hid_runtime::RouteTransitionResult::kAccepted);
    assert(controller.active_operation_for_test() == ControlOperation::kRouteChange);

    runtime.state_machine().on_unmount();
    assert(controller.route_snapshot().route.active == hid_route::OutputRoute::kNone);
    assert(controller.process_one_for_test());
    assert(controller.active_operation_for_test() == ControlOperation::kNone);
    assert(!controller.process_one_for_test());
}

void test_ble_lifecycle_is_shared_serialized_and_transport_independent() {
    hid_runtime::Runtime runtime;
    FakeBackend usb;
    FakeBleBackend ble;
    FakeBleDatabase database;
    hid_control_executor::Controller controller;
    assert(controller.initialize(&runtime, &usb, &ble, &database));
    assert(ble.heap_checkpoint_calls == 1);
    assert(ble.last_heap_checkpoint ==
           hid_control_executor::BleBackend::HeapCheckpoint::kColdBoot);
    const auto route_before = controller.route_snapshot();
    const auto epoch_before = runtime.state_machine().authority_epoch();

    const auto enable = controller.request_ble_enable();
    assert(enable.action_result == ble_lifecycle::TransitionResult::kAccepted);
    assert(enable.snapshot.observed == ble_lifecycle::ObservedState::kEnabling);
    assert(controller.active_operation_for_test() == ControlOperation::kBleEnable);
    assert(action(controller.request_attach()) == usb_lifecycle::TransitionResult::kBusy);
    assert(controller.request_route(hid_route::OutputRoute::kUsb).action_result ==
           hid_runtime::RouteTransitionResult::kBusy);
    assert(controller.process_one_for_test());
    assert(ble.initialize_calls == 1);
    assert(ble.last_heap_checkpoint ==
           hid_control_executor::BleBackend::HeapCheckpoint::kBeforeFirstEnable);
    assert(ble.event(hid_control_executor::BleEventKind::kSync));
    assert(controller.process_one_for_test());
    assert(controller.ble_snapshot().observed ==
           ble_lifecycle::ObservedState::kAdvertising);
    assert(controller.active_operation_for_test() == ControlOperation::kNone);
    assert(controller.route_snapshot().route.generation ==
           route_before.route.generation);
    assert(runtime.state_machine().authority_epoch() == epoch_before);

    assert(ble.event(hid_control_executor::BleEventKind::kConnect, 42));
    assert(controller.process_one_for_test());
    assert(controller.ble_snapshot().connected);
    assert(ble.configure_connection_calls == 1);
    assert(ble.last_configured_connection == 42);
    assert(ble.last_heap_checkpoint ==
           hid_control_executor::BleBackend::HeapCheckpoint::kConnected);
    assert(ble.event(hid_control_executor::BleEventKind::kDisconnect, 42));
    assert(controller.process_one_for_test());
    assert(controller.ble_snapshot().advertising);
    assert(controller.ble_snapshot().generation == 2);
    assert(ble.advertising_calls == 2);
    assert(ble.last_heap_checkpoint ==
           hid_control_executor::BleBackend::HeapCheckpoint::kReadvertising);
    assert(database.clear_calls == 1);
    assert(runtime.state_machine().authority_epoch() == epoch_before);
}

void test_ble_disable_expected_disconnect_and_retained_stack() {
    hid_runtime::Runtime runtime;
    FakeBackend usb;
    FakeBleBackend ble;
    FakeBleDatabase database;
    hid_control_executor::Controller controller;
    assert(controller.initialize(&runtime, &usb, &ble, &database));
    assert(controller.request_ble_enable().action_result ==
           ble_lifecycle::TransitionResult::kAccepted);
    assert(controller.process_one_for_test());
    assert(ble.event(hid_control_executor::BleEventKind::kSync));
    assert(controller.process_one_for_test());
    assert(ble.event(hid_control_executor::BleEventKind::kConnect, 11));
    assert(controller.process_one_for_test());

    const auto disable = controller.request_ble_disable();
    assert(disable.action_result == ble_lifecycle::TransitionResult::kAccepted);
    const auto disable_generation = disable.snapshot.generation;
    assert(controller.process_one_for_test());
    assert(ble.disconnect_calls == 1 && ble.last_connection == 11);
    assert(ble.event(hid_control_executor::BleEventKind::kDisconnect, 11));
    assert(controller.process_one_for_test());
    const auto idle = controller.ble_snapshot();
    assert(idle.generation == disable_generation);
    assert(idle.observed == ble_lifecycle::ObservedState::kIdle);
    assert(idle.stack_ready && !idle.advertising && !idle.connected);
    assert(ble.initialize_calls == 1);
    assert(controller.request_ble_disable().action_result ==
           ble_lifecycle::TransitionResult::kNoOp);
    assert(controller.request_ble_enable().action_result ==
           ble_lifecycle::TransitionResult::kAccepted);
    assert(controller.process_one_for_test());
    assert(ble.initialize_calls == 1);
}

void test_ble_busy_has_no_stage_a_and_usb_detach_does_not_change_ble() {
    hid_runtime::Runtime runtime;
    FakeBackend usb;
    FakeBleBackend ble;
    hid_control_executor::Controller controller;
    assert(controller.initialize(&runtime, &usb, &ble));
    const auto cold = controller.ble_snapshot();
    assert(controller.reserve_operation_for_test(ControlOperation::kUsbDetach));
    assert(controller.request_ble_enable().action_result ==
           ble_lifecycle::TransitionResult::kBusy);
    assert(controller.ble_snapshot().generation == cold.generation);
    assert(controller.ble_snapshot().desired == cold.desired);
    controller.release_operation_for_test(ControlOperation::kUsbDetach);
    assert(controller.request_ble_enable().action_result ==
           ble_lifecycle::TransitionResult::kAccepted);
    assert(controller.process_one_for_test());
    assert(ble.event(hid_control_executor::BleEventKind::kSync));
    assert(controller.process_one_for_test());
    const auto exposed = controller.ble_snapshot();
    assert(action(controller.request_attach()) == usb_lifecycle::TransitionResult::kAccepted);
    assert(controller.process_one_for_test());
    runtime.state_machine().on_mount();
    assert(action(controller.request_detach()) == usb_lifecycle::TransitionResult::kAccepted);
    assert(controller.process_one_for_test());
    assert(controller.ble_snapshot().desired == exposed.desired);
    assert(controller.ble_snapshot().generation == exposed.generation);
    assert(controller.ble_snapshot().advertising);
}

}  // namespace

int main() {
    test_install_and_uninstall_are_task_owned_and_serialized();
    test_route_owner_blocks_attach_before_lifecycle_stage_a();
    test_route_owner_blocks_detach_before_lifecycle_stage_a();
    test_usb_owner_blocks_route_reservation();
    test_no_op_and_ordinary_rejection_release_guard();
    test_schedule_requires_matching_preclaimed_owner();
    test_backend_failures_release_guard();
    test_real_queue_failure_is_recovery_fault_and_releases_guard();
    test_callback_invalidation_obsoletes_action_and_releases_guard();
    test_route_zero_work_is_synchronous_and_never_uninstalls_usb();
    test_route_release_owns_guard_and_blocks_detach_before_stage_a();
    test_usb_detach_owner_blocks_route_without_duplicate_action();
    test_route_schedule_failure_terminalizes_none_and_releases_guard();
    test_unmount_preempts_route_action_and_guard_releases_once();
    test_ble_lifecycle_is_shared_serialized_and_transport_independent();
    test_ble_disable_expected_disconnect_and_retained_stack();
    test_ble_busy_has_no_stage_a_and_usb_detach_does_not_change_ble();
}
