#include "hid_control_executor/hid_control_executor.hpp"

#ifndef HID_CONTROL_EXECUTOR_NATIVE_TEST
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_log.h"
#else
#include <mutex>
#endif

#include <cstring>
#include <limits>
#include <utility>

#include "ble_fixture_profile/ble_fixture_profile.hpp"
#include "secure_memory/secure_memory.hpp"

namespace hid_control_executor {
namespace {

inline constexpr const auto &kStrictProfile =
    ble_fixture_profile::strict_composite();

ble_fixture_profile::ReportMask subscription_mask(
    const BleHidPeerSnapshot &peer) {
    ble_fixture_profile::ReportMask result = 0;
    if (peer.keyboard_notify_enabled) {
        result |= ble_fixture_profile::report_bit(
            ble_fixture_profile::ReportRole::kKeyboardInput);
    }
    if (peer.mouse_notify_enabled) {
        result |= ble_fixture_profile::report_bit(
            ble_fixture_profile::ReportRole::kMouseInput);
    }
    return result;
}

#ifdef HID_CONTROL_EXECUTOR_NATIVE_TEST
std::mutex s_dle_mux;
struct DleLock {
    DleLock() { s_dle_mux.lock(); }
    ~DleLock() { s_dle_mux.unlock(); }
};
#else
portMUX_TYPE s_dle_mux = portMUX_INITIALIZER_UNLOCKED;
struct DleLock {
    DleLock() { portENTER_CRITICAL(&s_dle_mux); }
    ~DleLock() { portEXIT_CRITICAL(&s_dle_mux); }
};
#endif

#ifndef HID_CONTROL_EXECUTOR_NATIVE_TEST
constexpr std::uint32_t kLifecycleTaskStackBytes = 4096;
constexpr std::size_t kLifecycleTaskStackDepth =
    kLifecycleTaskStackBytes / sizeof(StackType_t);
constexpr UBaseType_t kLifecycleTaskPriority = tskIDLE_PRIORITY + 3;
constexpr char kLogTag[] = "hid_control";

StaticQueue_t s_queue_storage;
std::uint8_t s_queue_bytes[Controller::kActionQueueDepth *
                           sizeof(Controller::Action)]{};
StaticTask_t s_task_storage;
StackType_t s_task_stack[kLifecycleTaskStackDepth]{};
QueueHandle_t s_action_queue = nullptr;
TaskHandle_t s_executor_task = nullptr;
StaticSemaphore_t s_pairing_rpc_completion_storage;
SemaphoreHandle_t s_pairing_rpc_completion = nullptr;
static_assert(portTICK_PERIOD_MS == 10);
static_assert(pdMS_TO_TICKS(kUsbSofWatchdogSampleMs) > 0);
#endif

bool same_usb_link_watchdog_identity(
    const hid_runtime::UsbLinkWatchdogSnapshot &left,
    const hid_runtime::UsbLinkWatchdogSnapshot &right) {
    return left.attach_generation == right.attach_generation &&
           left.authority_epoch == right.authority_epoch &&
           left.route_generation == right.route_generation;
}

}  // namespace

bool Controller::BleRouteGraceAuthority::identities_equal(
    BleRouteReleaseIdentity left, BleRouteReleaseIdentity right) {
    return left.authority_epoch == right.authority_epoch &&
           left.route_generation == right.route_generation &&
           left.profile_activation_epoch == right.profile_activation_epoch &&
           left.ble_generation == right.ble_generation &&
           left.connection_handle == right.connection_handle &&
           left.present_roles == right.present_roles &&
           left.required_input_subscriptions ==
               right.required_input_subscriptions &&
           left.report_handles.values == right.report_handles.values &&
           left.release_epoch == right.release_epoch;
}

void Controller::BleRouteGraceAuthority::AtomicIdentity::store(
    BleRouteReleaseIdentity identity) {
    authority_epoch.store(identity.authority_epoch, std::memory_order_relaxed);
    route_generation.store(identity.route_generation,
                           std::memory_order_relaxed);
    profile_activation_epoch.store(identity.profile_activation_epoch,
                                   std::memory_order_relaxed);
    ble_generation.store(identity.ble_generation, std::memory_order_relaxed);
    connection_handle.store(identity.connection_handle,
                            std::memory_order_relaxed);
    present_roles.store(identity.present_roles, std::memory_order_relaxed);
    required_subscriptions.store(identity.required_input_subscriptions,
                                 std::memory_order_relaxed);
    for (std::size_t index = 0; index < report_handles.size(); ++index) {
        report_handles[index].store(identity.report_handles.values[index],
                                    std::memory_order_relaxed);
    }
    release_epoch.store(identity.release_epoch, std::memory_order_relaxed);
}

BleRouteReleaseIdentity
Controller::BleRouteGraceAuthority::AtomicIdentity::load() const {
    BleRouteReleaseIdentity identity{
        .authority_epoch = authority_epoch.load(std::memory_order_relaxed),
        .route_generation = route_generation.load(std::memory_order_relaxed),
        .profile_activation_epoch =
            profile_activation_epoch.load(std::memory_order_relaxed),
        .ble_generation = ble_generation.load(std::memory_order_relaxed),
        .connection_handle = connection_handle.load(std::memory_order_relaxed),
        .present_roles = present_roles.load(std::memory_order_relaxed),
        .required_input_subscriptions =
            required_subscriptions.load(std::memory_order_relaxed),
        .release_epoch = release_epoch.load(std::memory_order_relaxed),
    };
    for (std::size_t index = 0; index < report_handles.size(); ++index) {
        identity.report_handles.values[index] =
            report_handles[index].load(std::memory_order_relaxed);
    }
    return identity;
}

std::uint64_t Controller::BleRouteGraceAuthority::allocate_incarnation() {
    std::uint64_t candidate =
        next_incarnation_.load(std::memory_order_acquire);
    while (candidate != 0 && candidate <= kMaxIncarnation) {
        const std::uint64_t successor =
            candidate == kMaxIncarnation ? 0 : candidate + 1;
        if (next_incarnation_.compare_exchange_weak(
                candidate, successor, std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            return candidate;
        }
    }
    return 0;
}

Controller::BleRouteGraceAuthority::Claim
Controller::BleRouteGraceAuthority::arm(BleRouteReleaseIdentity identity) {
    const std::size_t first =
        next_slot_.fetch_add(1, std::memory_order_relaxed) % kSlotCount;
    for (std::size_t offset = 0; offset < kSlotCount; ++offset) {
        const std::size_t index = (first + offset) % kSlotCount;
        Slot &slot = slots_[index];
        std::uint64_t observed =
            slot.ownership.load(std::memory_order_acquire);
        if (state_of(observed) != State::kIdle) {
            continue;
        }
        const std::uint64_t incarnation = allocate_incarnation();
        if (incarnation == 0) {
            return {};
        }
        const std::uint64_t arming = pack(incarnation, State::kArming);
        if (!slot.ownership.compare_exchange_strong(
                observed, arming, std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            continue;
        }
        slot.identity.store(identity);
        const std::uint64_t armed = pack(incarnation, State::kArmed);
        slot.ownership.store(armed, std::memory_order_release);
        return {.slot = index, .ownership = armed, .identity = identity};
    }
    return {};
}

Controller::BleRouteGraceAuthority::Claim
Controller::BleRouteGraceAuthority::find_armed(
    BleRouteReleaseIdentity identity) const {
    for (std::size_t index = 0; index < kSlotCount; ++index) {
        const Slot &slot = slots_[index];
        const std::uint64_t observed =
            slot.ownership.load(std::memory_order_acquire);
        if (state_of(observed) == State::kArmed &&
            identities_equal(slot.identity.load(), identity)) {
            return {.slot = index,
                    .ownership = observed,
                    .identity = identity};
        }
    }
    return {};
}

Controller::BleRouteGraceAuthority::Claim
Controller::BleRouteGraceAuthority::claim(Claim candidate) {
    if (!candidate.valid() || state_of(candidate.ownership) != State::kArmed) {
        return {};
    }
    Slot &slot = slots_[candidate.slot];
    std::uint64_t expected = candidate.ownership;
    const std::uint64_t claimed =
        with_state(candidate.ownership, State::kClaimed);
    if (!slot.ownership.compare_exchange_strong(
            expected, claimed, std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        return {};
    }
    candidate.ownership = claimed;
    return candidate;
}

bool Controller::BleRouteGraceAuthority::publish_due(Claim claim) {
    if (!claim.valid() || state_of(claim.ownership) != State::kClaimed) {
        return false;
    }
    Slot &slot = slots_[claim.slot];
    std::uint64_t expected = claim.ownership;
    if (slot.ownership.compare_exchange_strong(
            expected, with_state(claim.ownership, State::kDue),
            std::memory_order_acq_rel, std::memory_order_acquire)) {
        return true;
    }
    const std::uint64_t canceled =
        with_state(claim.ownership, State::kCanceledClaimed);
    if (expected == canceled) {
        (void)slot.ownership.compare_exchange_strong(
            expected, with_state(claim.ownership, State::kIdle),
            std::memory_order_acq_rel, std::memory_order_acquire);
    }
    return false;
}

void Controller::BleRouteGraceAuthority::cancel(Claim claim) {
    if (!claim.valid()) {
        return;
    }
    Slot &slot = slots_[claim.slot];
    for (;;) {
        std::uint64_t observed =
            slot.ownership.load(std::memory_order_acquire);
        if ((observed & ~kStateMask) != (claim.ownership & ~kStateMask)) {
            return;
        }
        State destination = State::kIdle;
        switch (state_of(observed)) {
            case State::kArmed:
            case State::kDue:
                break;
            case State::kClaimed:
                destination = State::kCanceledClaimed;
                break;
            default:
                return;
        }
        if (slot.ownership.compare_exchange_weak(
                observed, with_state(observed, destination),
                std::memory_order_acq_rel, std::memory_order_acquire)) {
            return;
        }
    }
}

bool Controller::BleRouteGraceAuthority::consume_due(
    Claim claim, BleRouteReleaseIdentity identity) {
    if (!claim.valid() || !identities_equal(claim.identity, identity)) {
        return false;
    }
    Slot &slot = slots_[claim.slot];
    std::uint64_t expected = with_state(claim.ownership, State::kDue);
    if (!identities_equal(slot.identity.load(), identity)) {
        return false;
    }
    return slot.ownership.compare_exchange_strong(
        expected, with_state(claim.ownership, State::kIdle),
        std::memory_order_acq_rel, std::memory_order_acquire);
}

std::size_t
Controller::BleRouteGraceAuthority::available_slots_for_test() const {
    std::size_t available = 0;
    for (const Slot &slot : slots_) {
        if (state_of(slot.ownership.load(std::memory_order_acquire)) ==
            State::kIdle) {
            ++available;
        }
    }
    return available;
}

bool Controller::initialize(hid_runtime::Runtime *runtime, Backend *backend,
                            BleBackend *ble_backend, BleDatabase *ble_database,
                            UsbLinkStallSink usb_link_stall_sink) {
    if (initialized_) {
        return true;
    }
    if (runtime == nullptr || backend == nullptr) {
        return false;
    }
    runtime_ = runtime;
    backend_ = backend;
    ble_backend_ = ble_backend;
    ble_database_ = ble_database;
    usb_link_stall_sink_ = usb_link_stall_sink;
#ifndef HID_CONTROL_EXECUTOR_NATIVE_TEST
    s_action_queue = xQueueCreateStatic(kActionQueueDepth, sizeof(Action), s_queue_bytes,
                                        &s_queue_storage);
    s_pairing_rpc_completion =
        xSemaphoreCreateBinaryStatic(&s_pairing_rpc_completion_storage);
    if (s_action_queue == nullptr || s_pairing_rpc_completion == nullptr) {
        return false;
    }
    s_executor_task = xTaskCreateStatic(
        task_entry, "hid_control", kLifecycleTaskStackDepth, this,
        kLifecycleTaskPriority, s_task_stack, &s_task_storage);
    if (s_executor_task == nullptr) {
        return false;
    }
#endif
    runtime_->bind_authority_event_sink(this);
    initialized_ = true;
    if (ble_backend_ != nullptr) {
        ble_backend_->record_heap_checkpoint(BleBackend::HeapCheckpoint::kColdBoot);
    }
    return true;
}

BleCommandOutcome Controller::request_ble_enable() {
    constexpr ControlOperation operation = ControlOperation::kBleEnable;
    if (!initialized_ || ble_backend_ == nullptr || !claim_operation(operation)) {
        return {};
    }
    const ble_lifecycle::TransitionOutcome outcome = ble_state_.begin_enable();
    if (outcome.action_result == ble_lifecycle::TransitionResult::kAccepted) {
        const Action item =
            Action::with_operation(ActionKind::kBleEnable, operation);
        if (!enqueue(item)) {
            ble_state_.complete_fault(outcome.snapshot.generation,
                                      ble_lifecycle::Operation::kEnable, -1);
            clear_ble_hid_peer();
            release_operation(operation);
            return {};
        }
    } else {
        release_operation(operation);
    }
    return {.action_result = outcome.action_result,
            .snapshot_valid = outcome.snapshot_valid,
            .snapshot = outcome.snapshot};
}

BleCommandOutcome Controller::request_ble_disable() {
    constexpr ControlOperation operation = ControlOperation::kBleDisable;
    const auto route = runtime_ == nullptr
                           ? hid_route::Snapshot{}
                           : runtime_->state_machine().route_snapshot();
    if (!initialized_ || ble_backend_ == nullptr || !route.coherent ||
        route.active == hid_route::OutputRoute::kBle ||
        !claim_operation(operation)) {
        return {};
    }
    const ble_lifecycle::TransitionOutcome outcome = [this] {
        DleLock lock;
        const auto transition = ble_state_.begin_disable();
        if (transition.action_result == ble_lifecycle::TransitionResult::kAccepted) {
            dle_available_ = false;
        }
        return transition;
    }();
    if (outcome.action_result == ble_lifecycle::TransitionResult::kAccepted) {
        const Action item =
            Action::with_operation(ActionKind::kBleDisable, operation);
        if (!enqueue(item)) {
            ble_state_.complete_fault(outcome.snapshot.generation,
                                      ble_lifecycle::Operation::kDisable, -1);
            clear_ble_hid_peer();
            release_operation(operation);
            return {};
        }
    } else {
        release_operation(operation);
    }
    return {.action_result = outcome.action_result,
            .snapshot_valid = outcome.snapshot_valid,
            .snapshot = outcome.snapshot};
}

ble_lifecycle::Snapshot Controller::ble_snapshot() const {
    return ble_state_.snapshot();
}

BleHidPeerSnapshot Controller::ble_hid_peer_snapshot() const {
    return ble_hid_peer_;
}

bool Controller::ble_link_ready() const {
    if (ble_backend_ == nullptr || ble_database_ == nullptr ||
        ble_lifecycle_handoff_failure_.load(std::memory_order_acquire) ||
        !ble_hid_peer_.active || ble_hid_peer_.suspended ||
        !ble_fixture_profile::subscriptions_ready(
            kStrictProfile, subscription_mask(ble_hid_peer_))) {
        return false;
    }
    const auto lifecycle = ble_state_.snapshot();
    if (ble_event_overflow_pending(lifecycle.generation) ||
        ble_route_loss_pending(lifecycle.generation)) {
        return false;
    }
    const auto handles = ble_database_->hid_handles();
    return lifecycle.generation == ble_hid_peer_.generation &&
           lifecycle.desired == ble_lifecycle::DesiredExposure::kExposed &&
           lifecycle.observed == ble_lifecycle::ObservedState::kConnected &&
           lifecycle.connected && !lifecycle.recovery_required &&
           ble_state_.connection_handle() == ble_hid_peer_.connection_handle &&
           handles.keyboard_value != 0 && handles.mouse_value != 0 &&
           handles.control_point_value != 0 &&
           handles.keyboard_value == ble_hid_peer_.handles.keyboard_value &&
           handles.mouse_value == ble_hid_peer_.handles.mouse_value &&
           handles.control_point_value ==
               ble_hid_peer_.handles.control_point_value &&
           ble_backend_->security_ready_for_hid(
               ble_hid_peer_.generation, ble_hid_peer_.connection_handle) &&
           ble_backend_->gatt_schema_current_for_hid(
               ble_hid_peer_.generation, ble_hid_peer_.connection_handle);
}

ble_pairing::Snapshot Controller::pairing_snapshot() const {
    return pairing_state_.snapshot();
}

PairingStatusSnapshot Controller::request_pairing_status() {
    if (!initialized_ || ble_backend_ == nullptr ||
        pairing_rpc_pending_.load(std::memory_order_acquire) != 0) {
        return {};
    }
    std::uint32_t token = next_pairing_rpc_token_++;
    if (token == 0) {
        token = next_pairing_rpc_token_++;
    }
    pairing_rpc_pending_.store(token, std::memory_order_release);
    const Action item =
        Action::with_operation(ActionKind::kPairingStatus,
                               ControlOperation::kNone, token);
    if (!enqueue(item)) {
        pairing_rpc_pending_.store(0, std::memory_order_release);
        return {};
    }
#ifdef HID_CONTROL_EXECUTOR_NATIVE_TEST
    while (pairing_rpc_pending_.load(std::memory_order_acquire) == token &&
           process_one_for_test()) {
    }
#else
    (void)xSemaphoreTake(s_pairing_rpc_completion, portMAX_DELAY);
#endif
    return pairing_rpc_status_;
}

ble_pairing::RespondResult Controller::request_pairing_response(
    std::uint32_t pairing_id,
    const std::array<char, 6> &six_digit_secret) {
    if (!initialized_ || ble_backend_ == nullptr ||
        pairing_rpc_pending_.load(std::memory_order_acquire) != 0) {
        return ble_pairing::RespondResult::kNotPending;
    }
    const auto pending = pairing_state_.snapshot();
    std::uint32_t token = next_pairing_rpc_token_++;
    if (token == 0) {
        token = next_pairing_rpc_token_++;
    }
    pairing_mailbox_.generation = pending.generation;
    pairing_mailbox_.connection_handle = pending.connection_handle;
    pairing_mailbox_.pairing_id = pairing_id;
    pairing_mailbox_.secret = six_digit_secret;
    pairing_mailbox_.token = token;
    pairing_mailbox_.occupied = true;
    pairing_rpc_result_ = ble_pairing::RespondResult::kNotPending;
    pairing_rpc_pending_.store(token, std::memory_order_release);
    const Action item =
        Action::with_operation(ActionKind::kPairingRespond,
                               ControlOperation::kNone, token);
    if (!enqueue(item)) {
        wipe_pairing_mailbox();
        pairing_rpc_pending_.store(0, std::memory_order_release);
        return ble_pairing::RespondResult::kNotPending;
    }
#ifdef HID_CONTROL_EXECUTOR_NATIVE_TEST
    while (pairing_rpc_pending_.load(std::memory_order_acquire) == token &&
           process_one_for_test()) {
    }
#else
    (void)xSemaphoreTake(s_pairing_rpc_completion, portMAX_DELAY);
#endif
    return pairing_rpc_result_;
}

std::uint32_t Controller::begin_serialized_rpc() {
    if (pairing_rpc_pending_.load(std::memory_order_acquire) != 0) {
        return 0;
    }
    std::uint32_t token = next_pairing_rpc_token_++;
    if (token == 0) {
        token = next_pairing_rpc_token_++;
    }
    pairing_rpc_pending_.store(token, std::memory_order_release);
    return token;
}

BleBondListResult Controller::request_bond_list() {
    if (!initialized_ || ble_backend_ == nullptr) {
        return {};
    }
    const std::uint32_t token = begin_serialized_rpc();
    if (token == 0) {
        return {};
    }
    bond_list_rpc_result_ = {};
    if (!enqueue(Action::with_operation(ActionKind::kBondList,
                                        ControlOperation::kNone, token))) {
        pairing_rpc_pending_.store(0, std::memory_order_release);
        return {};
    }
#ifdef HID_CONTROL_EXECUTOR_NATIVE_TEST
    while (pairing_rpc_pending_.load(std::memory_order_acquire) == token &&
           process_one_for_test()) {
    }
#else
    (void)xSemaphoreTake(s_pairing_rpc_completion, portMAX_DELAY);
#endif
    return bond_list_rpc_result_;
}

BleBondRemoveResult Controller::request_bond_remove(const BondId &bond_id) {
    constexpr ControlOperation operation =
        ControlOperation::kBondAdministration;
    BleBondRemoveResult unavailable{};
    unavailable.bond_id = bond_id;
    if (!initialized_ || ble_backend_ == nullptr) {
        return unavailable;
    }
    if (!claim_operation(operation)) {
        unavailable.kind = BleBondRemoveResultKind::kBusy;
        return unavailable;
    }
    const std::uint32_t token = begin_serialized_rpc();
    if (token == 0) {
        release_operation(operation);
        unavailable.kind = BleBondRemoveResultKind::kBusy;
        return unavailable;
    }
    bond_remove_mailbox_ = bond_id;
    bond_remove_rpc_result_ = unavailable;
    if (!enqueue(Action::with_operation(ActionKind::kBondRemove,
                                        operation, token))) {
        bond_remove_mailbox_ = {};
        pairing_rpc_pending_.store(0, std::memory_order_release);
        release_operation(operation);
        return unavailable;
    }
#ifdef HID_CONTROL_EXECUTOR_NATIVE_TEST
    while (pairing_rpc_pending_.load(std::memory_order_acquire) == token &&
           process_one_for_test()) {
    }
#else
    (void)xSemaphoreTake(s_pairing_rpc_completion, portMAX_DELAY);
#endif
    return bond_remove_rpc_result_;
}

void Controller::retire_dle_on_reset(ble_lifecycle::Generation generation) {
    DleLock lock;
    if (generation == ble_state_.generation()) {
        dle_seen_ = true;
        dle_generation_ = generation;
        dle_available_ = false;
    }
}

void Controller::observe_dle_event(BleEvent event) {
    DleLock lock;
    const auto current = ble_state_.snapshot();
    // CONNECT can arrive before start_advertising() returns to its owner.
    // Publish provisional eligibility; executor acceptance and claim remain
    // mandatory. Do not require the owner's Advertising publication yet.
    if (event.kind == BleEventKind::kConnect && event.status == 0 &&
        current.generation == event.generation &&
        current.desired == ble_lifecycle::DesiredExposure::kExposed &&
        !current.connected && !current.recovery_required &&
        event.connection_handle != ble_lifecycle::kNoConnection &&
        (!dle_seen_ || dle_generation_ != event.generation)) {
        dle_generation_ = event.generation;
        dle_connection_ = event.connection_handle;
        dle_seen_ = true;
        dle_available_ = true;
    } else if (event.generation == dle_generation_ &&
               (event.kind == BleEventKind::kReset ||
                (event.kind == BleEventKind::kDisconnect &&
                 event.connection_handle == dle_connection_))) {
        dle_available_ = false;
    }
}

bool Controller::claim_dle(BleEvent event) {
    DleLock lock;
    const auto current = ble_state_.snapshot();
    const bool valid = dle_available_ && dle_generation_ == event.generation &&
        dle_connection_ == event.connection_handle &&
        current.generation == event.generation && current.connected &&
        current.desired == ble_lifecycle::DesiredExposure::kExposed &&
        !current.recovery_required &&
        ble_state_.connection_handle() == event.connection_handle &&
        !ble_event_overflow_pending(event.generation) &&
        !ble_lifecycle_handoff_failure_.load(std::memory_order_acquire) &&
        !ble_backend_->persistent_store_failure_observed();
    // Never rearm in executor begin_security: a terminal callback may already
    // have retired the connection while its CONNECT was waiting in the queue.
    dle_available_ = false;
    return valid;
}

bool Controller::signal_ble_event(BleEvent event) {
    observe_dle_event(event);
    const bool loses_current_route =
        event_immediately_loses_ble_hid_readiness(event) &&
        event_targets_current_ble_authority(event);
    mark_ble_route_loss(event);
    if (runtime_ != nullptr && loses_current_route) {
        runtime_->state_machine().note_ble_route_continuity_loss(
            runtime_->state_machine().ble_route_authority_snapshot());
    }
    const Action item = Action::with_ble_event(event);
    if (enqueue(item)) {
        return true;
    }
#ifdef HID_CONTROL_EXECUTOR_NATIVE_TEST
    if (ble_enqueue_failure_hook_ != nullptr &&
        ble_enqueue_failure_phase_ ==
            BleEnqueueFailurePhase::kBeforeGenericFallback) {
        const auto hook = ble_enqueue_failure_hook_;
        ble_enqueue_failure_hook_ = nullptr;
        hook(*this);
    }
#endif
    if (runtime_ != nullptr &&
        event_targets_current_ble_authority(event)) {
        runtime_->state_machine().note_ble_route_continuity_loss(
            runtime_->state_machine().ble_route_authority_snapshot());
    }
    mark_ble_event_overflow(event);
    if (event.kind == BleEventKind::kDisconnect && runtime_ != nullptr) {
        const auto release =
            runtime_->state_machine().ble_route_authority_snapshot();
        if (release.coherent && (release.active || release.releasing) &&
            release.ble_generation == event.generation &&
            release.connection_handle == event.connection_handle) {
            // The callback itself is an exact physical observation. Retain it
            // independently of the full queue so route retirement can finish
            // after the stronger generic-overflow fault is committed.
            ble_route_disconnect_observed_.store(true,
                                                 std::memory_order_release);
            request_executor_wake();
        }
    }
#ifdef HID_CONTROL_EXECUTOR_NATIVE_TEST
    if (ble_enqueue_failure_hook_ != nullptr &&
        ble_enqueue_failure_phase_ ==
            BleEnqueueFailurePhase::kAfterGenericFallback) {
        const auto hook = ble_enqueue_failure_hook_;
        ble_enqueue_failure_hook_ = nullptr;
        hook(*this);
    }
#endif
    if (event.kind == BleEventKind::kConnect && event.status == 0 &&
        event.connection_handle != ble_lifecycle::kNoConnection &&
        ble_backend_ != nullptr) {
        // This runs synchronously in the successful GAP Connect callback. The
        // physical link exists, but the executor did not adopt it because its
        // event could not enter the queue. Terminate the exact callback handle;
        // logical recovery remains governed by generic overflow handling.
        (void)ble_backend_->terminate_orphan_connection(
            event.connection_handle);
    }
    return false;
}

bool Controller::signal_ble_route_release_grace(
    BleRouteReleaseIdentity identity) {
    auto candidate = ble_route_grace_authority_.find_armed(identity);
    if (!candidate.valid()) {
        return false;
    }
    if (!ble_route_release_identity_current(identity)) {
        return false;
    }
#ifdef HID_CONTROL_EXECUTOR_NATIVE_TEST
    if (ble_grace_signal_hook_ != nullptr &&
        ble_grace_signal_phase_ ==
            BleGraceSignalPhase::kAfterIdentityValidation) {
        const auto hook = ble_grace_signal_hook_;
        ble_grace_signal_hook_ = nullptr;
        hook(*this);
    }
#endif
    const auto claim = ble_route_grace_authority_.claim(candidate);
    if (!claim.valid()) {
        return false;
    }
#ifdef HID_CONTROL_EXECUTOR_NATIVE_TEST
    if (ble_grace_signal_hook_ != nullptr &&
        ble_grace_signal_phase_ == BleGraceSignalPhase::kAfterClaim) {
        const auto hook = ble_grace_signal_hook_;
        ble_grace_signal_hook_ = nullptr;
        hook(*this);
    }
#endif
    if (!ble_route_grace_authority_.publish_due(claim)) {
        return false;
    }
    const Action action = Action::empty(ActionKind::kBleRouteReleaseGrace);
    if (!enqueue(action)) {
        // The identity-owned due slot is authoritative and the independent
        // task notification makes a full normal queue unable to strand it.
        request_executor_wake();
    }
    return true;
}

void Controller::signal_ble_lifecycle_handoff_failure() {
    ble_lifecycle_handoff_failure_.store(true, std::memory_order_release);
    if (runtime_ != nullptr) {
        runtime_->state_machine().note_ble_route_continuity_loss(
            runtime_->state_machine().ble_route_authority_snapshot());
    }
    request_executor_wake();
}

ControlOperation Controller::operation_for(usb_lifecycle::ExecutorAction action) {
    return action == usb_lifecycle::ExecutorAction::kInstall
               ? ControlOperation::kUsbAttach
               : ControlOperation::kUsbDetach;
}

bool Controller::claim_operation(ControlOperation operation) {
    ControlOperation expected = ControlOperation::kNone;
    return active_operation_.compare_exchange_strong(expected, operation,
                                                     std::memory_order_acq_rel,
                                                     std::memory_order_acquire);
}

void Controller::release_operation(ControlOperation operation) {
    ControlOperation expected = operation;
    (void)active_operation_.compare_exchange_strong(expected, ControlOperation::kNone,
                                                     std::memory_order_acq_rel,
                                                     std::memory_order_acquire);
}

CommandOutcome Controller::request_attach() {
    constexpr ControlOperation operation = ControlOperation::kUsbAttach;
    if (!initialized_ || !claim_operation(operation)) {
        return {};
    }
    const hid_runtime::UsbTransitionOutcome outcome =
        runtime_->state_machine().request_usb_attach(*this);
    if (outcome.action_result != usb_lifecycle::TransitionResult::kAccepted) {
        // No asynchronous action exists for no-op, ordinary rejection, or a
        // real post-Stage-A scheduling failure.
        release_operation(operation);
    }
    return CommandOutcome{
        .action_result = outcome.action_result,
        .snapshot_valid = outcome.snapshot_valid,
        .snapshot = ExposureSnapshot{
            .lifecycle = outcome.lifecycle,
            .runtime = outcome.runtime,
        },
    };
}

CommandOutcome Controller::request_detach() {
    constexpr ControlOperation operation = ControlOperation::kUsbDetach;
    if (!initialized_ || !claim_operation(operation)) {
        return {};
    }
    const hid_runtime::UsbTransitionOutcome outcome =
        runtime_->state_machine().request_usb_detach(*this);
    if (outcome.action_result != usb_lifecycle::TransitionResult::kAccepted) {
        // Accepted work retains ownership until process() terminalizes it.
        release_operation(operation);
    }
    return CommandOutcome{
        .action_result = outcome.action_result,
        .snapshot_valid = outcome.snapshot_valid,
        .snapshot = ExposureSnapshot{
            .lifecycle = outcome.lifecycle,
            .runtime = outcome.runtime,
        },
    };
}

ExposureSnapshot Controller::snapshot() const {
    if (runtime_ == nullptr) {
        return {};
    }
    return ExposureSnapshot{
        .lifecycle = runtime_->state_machine().usb_lifecycle_snapshot(),
        .runtime = runtime_->state_machine().status(),
    };
}

hid_runtime::RouteStatusSnapshot Controller::route_snapshot() {
    if (runtime_ == nullptr) {
        return {};
    }
    const auto status = request_pairing_status();
    auto snapshot = runtime_->state_machine().route_status_snapshot();
    snapshot.ready = snapshot.ready ||
                     (status.ble_route_ready &&
                      snapshot.route.desired == hid_route::OutputRoute::kBle);
    return snapshot;
}

RouteCommandOutcome Controller::request_route(hid_route::OutputRoute desired) {
    constexpr ControlOperation operation = ControlOperation::kRouteChange;
    if (!initialized_ || runtime_ == nullptr || !claim_operation(operation)) {
        return {};
    }
    if (desired == hid_route::OutputRoute::kBle) {
        if (ble_backend_ == nullptr || ble_database_ == nullptr ||
            pairing_rpc_pending_.load(std::memory_order_acquire) != 0) {
            release_operation(operation);
            return {};
        }
        constexpr std::uint32_t token = 0xffffffffU;
        pairing_rpc_pending_.store(token, std::memory_order_release);
        if (!enqueue(Action::with_operation(ActionKind::kRouteBleActivate,
                                            operation, token))) {
            pairing_rpc_pending_.store(0, std::memory_order_release);
            release_operation(operation);
            return {};
        }
#ifdef HID_CONTROL_EXECUTOR_NATIVE_TEST
        while (pairing_rpc_pending_.load(std::memory_order_acquire) == token &&
               process_one_for_test()) {
        }
#else
        (void)xSemaphoreTake(s_pairing_rpc_completion, portMAX_DELAY);
#endif
        return route_rpc_result_;
    }
    hid_runtime::RouteTransitionOutcome outcome =
        desired == hid_route::OutputRoute::kUsb
            ? runtime_->state_machine().request_route_usb()
            : runtime_->state_machine().request_route_none();
    if (outcome.action_result == hid_runtime::RouteTransitionResult::kAccepted &&
        outcome.async_required) {
        if (outcome.snapshot.route.active == hid_route::OutputRoute::kBle) {
            // BLE release progress is capacity-independent: the retained
            // runtime identity plus this wake replaces a queue-owned action.
            request_executor_wake();
            return RouteCommandOutcome{.action_result = outcome.action_result,
                                       .snapshot_valid = outcome.snapshot_valid,
                                       .snapshot = outcome.snapshot};
        }
        const Action item = Action::with_lifecycle(
            ActionKind::kRouteRelease,
            runtime_->state_machine().usb_lifecycle_snapshot(),
            outcome.snapshot.route, operation);
        if (!enqueue(item)) {
            runtime_->state_machine().terminalize_route_release_schedule_failure(
                outcome.snapshot.route);
            release_operation(operation);
            return {};
        }
    } else {
        release_operation(operation);
    }
    if (desired == hid_route::OutputRoute::kUsb &&
        outcome.action_result == hid_runtime::RouteTransitionResult::kAccepted &&
        !outcome.async_required) {
        // A direct task notification arms the existing executor-owned SOF
        // watchdog without adding queue traffic or a dedicated task.
        request_executor_wake();
    }
    return RouteCommandOutcome{.action_result = outcome.action_result,
                               .snapshot_valid = outcome.snapshot_valid,
                               .snapshot = outcome.snapshot};
}

RouteCommandOutcome Controller::activate_ble_route_internal() {
    if (!initialized_ || runtime_ == nullptr || ble_backend_ == nullptr ||
        ble_database_ == nullptr ||
        active_operation_.load(std::memory_order_acquire) !=
            ControlOperation::kNone) {
        return RouteCommandOutcome{
            .action_result = hid_runtime::RouteTransitionResult::kNotReady,
            .snapshot_valid = runtime_ != nullptr,
            .snapshot = runtime_ == nullptr
                            ? hid_runtime::RouteStatusSnapshot{}
                            : runtime_->state_machine().route_status_snapshot(),
        };
    }
    return activate_ble_route();
}

RouteCommandOutcome Controller::activate_ble_route() {
    hid_runtime::StateMachine &state = runtime_->state_machine();
    const auto route = state.route_snapshot();
    if (ble_route_ready()) {
        return RouteCommandOutcome{
            .action_result = hid_runtime::RouteTransitionResult::kNoOp,
            .snapshot_valid = true,
            .snapshot = hid_runtime::RouteStatusSnapshot{
                .route = route, .ready = true},
        };
    }
    if (!ble_link_ready()) {
        return RouteCommandOutcome{
            .action_result = hid_runtime::RouteTransitionResult::kNotReady,
            .snapshot_valid = runtime_ != nullptr,
            .snapshot = runtime_ == nullptr
                            ? hid_runtime::RouteStatusSnapshot{}
                            : runtime_->state_machine().route_status_snapshot(),
        };
    }
    const auto peer = ble_hid_peer_;
    const hid_runtime::BleRouteActivation activation{
        .expected_authority_epoch = state.authority_epoch(),
        .expected_route_generation = route.generation,
        .ble_generation = peer.generation,
        .connection_handle = peer.connection_handle,
        .present_roles = hid_capability::kInputRoles,
        .required_input_subscriptions = hid_capability::kInputRoles,
        .report_handles = {.values = {
            peer.handles.keyboard_value,
            peer.handles.mouse_value,
            0,
        }},
    };
    const auto outcome = state.request_route_ble(activation);
    if (outcome.action_result != hid_runtime::RouteTransitionResult::kAccepted) {
        return RouteCommandOutcome{.action_result = outcome.action_result,
                                   .snapshot_valid = outcome.snapshot_valid,
                                   .snapshot = outcome.snapshot};
    }
    const auto active = state.ble_route_authority_snapshot();
    if (!ble_route_ready()) {
        (void)state.retire_ble_route_if_matches(active);
        drive_ble_route_retirement();
        return RouteCommandOutcome{
            .action_result = hid_runtime::RouteTransitionResult::kNotReady,
            .snapshot_valid = true,
            .snapshot = state.route_status_snapshot(),
        };
    }
    return RouteCommandOutcome{.action_result = outcome.action_result,
                               .snapshot_valid = outcome.snapshot_valid,
                               .snapshot = outcome.snapshot};
}

bool Controller::ble_route_ready() const {
    if (runtime_ == nullptr) {
        return false;
    }
    const hid_runtime::StateMachine &state = runtime_->state_machine();
    const auto authority = state.ble_route_authority_snapshot();
    const auto release = state.release_all_snapshot();
    const auto peer = ble_hid_peer_;
    const auto route = state.route_snapshot();
    return !release.active && ble_link_ready() && route.coherent &&
           !route.invalidation_pending &&
           route.active == hid_route::OutputRoute::kBle &&
           route.desired == hid_route::OutputRoute::kBle &&
           route.transition == hid_route::Transition::kStable &&
           route.generation == authority.route_generation &&
           state.ble_route_normal_authority_matches(authority) &&
           peer.generation == authority.ble_generation &&
           peer.connection_handle == authority.connection_handle &&
           authority.present_roles == hid_capability::kInputRoles &&
           authority.required_input_subscriptions ==
               hid_capability::kInputRoles &&
           peer.handles.keyboard_value ==
               authority.report_handles.get(
                   hid_runtime::ReportRole::kKeyboardInput) &&
           peer.handles.mouse_value == authority.report_handles.get(
               hid_runtime::ReportRole::kMouseInput);
}

bool Controller::enqueue_ble_hid_work(hid_runtime::Interface interface,
                                      hid_runtime::HidWorkToken token) {
    if (!initialized_ || runtime_ == nullptr ||
        token.transport != hid_runtime::HidTransport::kBle ||
        token.ticket_id == 0 ||
        !runtime_->state_machine().mark_ble_report_scheduled(interface,
                                                              token)) {
        return false;
    }
    if (enqueue(Action::with_hid_report(interface, token))) {
        return true;
    }
    runtime_->state_machine().abandon_ble_report(interface, token);
    return false;
}

hid_runtime::KeyboardReportBeginResult Controller::queue_ble_keyboard_report(
    std::uint8_t modifiers,
    const std::array<std::uint8_t, 6> &keycodes,
    hid_runtime::SequenceAuthority sequence,
    hid_runtime::HidTicketId *ticket_id,
    hid_runtime::ReportOriginOwnerId originating_local_owner_id) {
    if (ticket_id != nullptr) *ticket_id = 0;
    if (!initialized_ || runtime_ == nullptr) {
        return hid_runtime::KeyboardReportBeginResult::kNotReady;
    }
    hid_runtime::StateMachine &state = runtime_->state_machine();
    hid_runtime::HidTicketId admitted_ticket = 0;
    const auto begin = state.begin_keyboard_report(
        modifiers, keycodes, sequence, &admitted_ticket,
        originating_local_owner_id);
    if (begin != hid_runtime::KeyboardReportBeginResult::kPublished) {
        return begin;
    }
    const auto token =
        state.published_report_token(hid_runtime::Interface::kKeyboard);
    if (token.ticket_id != admitted_ticket ||
        token.transport != hid_runtime::HidTransport::kBle ||
        !enqueue_ble_hid_work(hid_runtime::Interface::kKeyboard, token)) {
        (void)state.cancel_keyboard_report(admitted_ticket);
        return hid_runtime::KeyboardReportBeginResult::kBusy;
    }
    if (ticket_id != nullptr) *ticket_id = admitted_ticket;
    return begin;
}

hid_runtime::MouseReportBeginResult Controller::queue_ble_mouse_report(
    std::uint8_t buttons, std::int8_t x, std::int8_t y,
    std::int8_t vertical, std::int8_t horizontal,
    hid_runtime::SequenceAuthority sequence,
    hid_runtime::HidTicketId *ticket_id,
    hid_runtime::ReportOriginOwnerId originating_local_owner_id) {
    if (ticket_id != nullptr) *ticket_id = 0;
    if (!initialized_ || runtime_ == nullptr) {
        return hid_runtime::MouseReportBeginResult::kNotReady;
    }
    hid_runtime::StateMachine &state = runtime_->state_machine();
    hid_runtime::HidTicketId admitted_ticket = 0;
    const auto begin = state.begin_mouse_report(
        buttons, x, y, vertical, horizontal, sequence, &admitted_ticket,
        originating_local_owner_id);
    if (begin != hid_runtime::MouseReportBeginResult::kPublished) {
        return begin;
    }
    const auto token = state.published_report_token(
        hid_runtime::Interface::kMouse);
    if (token.ticket_id != admitted_ticket ||
        token.transport != hid_runtime::HidTransport::kBle ||
        !enqueue_ble_hid_work(hid_runtime::Interface::kMouse, token)) {
        (void)state.cancel_mouse_report(admitted_ticket);
        return hid_runtime::MouseReportBeginResult::kBusy;
    }
    if (ticket_id != nullptr) *ticket_id = admitted_ticket;
    return begin;
}

#ifndef HID_CONTROL_EXECUTOR_NATIVE_TEST
hid_runtime::KeyboardReportResult Controller::keyboard_report(
    std::uint8_t modifiers,
    const std::array<std::uint8_t, 6> &keycodes,
    hid_runtime::ReportOriginOwnerId originating_local_owner_id) {
    const auto route = runtime_->state_machine().route_snapshot();
    if (route.active != hid_route::OutputRoute::kBle) {
        return runtime_->keyboard_report(modifiers, keycodes, {},
                                         originating_local_owner_id);
    }
    hid_runtime::HidTicketId ticket_id = 0;
    const auto begin = queue_ble_keyboard_report(
        modifiers, keycodes, {}, &ticket_id, originating_local_owner_id);
    return runtime_->complete_keyboard_report(begin, ticket_id);
}

hid_runtime::MouseReportResult Controller::mouse_report(
    std::uint8_t buttons, std::int8_t x, std::int8_t y,
    std::int8_t vertical, std::int8_t horizontal,
    hid_runtime::ReportOriginOwnerId originating_local_owner_id) {
    const auto route = runtime_->state_machine().route_snapshot();
    if (route.active != hid_route::OutputRoute::kBle) {
        return runtime_->mouse_report(buttons, x, y, vertical, horizontal, {},
                                      originating_local_owner_id);
    }
    hid_runtime::HidTicketId ticket_id = 0;
    const auto begin = queue_ble_mouse_report(
        buttons, x, y, vertical, horizontal, {}, &ticket_id,
        originating_local_owner_id);
    return runtime_->complete_mouse_report(begin, ticket_id);
}

hid_runtime::KeyboardReportResult Controller::sequence_keyboard_report(
    hid_runtime::SequenceAuthority sequence,
    std::uint8_t modifiers,
    const std::array<std::uint8_t, 6> &keycodes,
    hid_runtime::ReportOriginOwnerId originating_local_owner_id) {
    const auto route = runtime_->state_machine().route_snapshot();
    if (route.active != hid_route::OutputRoute::kBle) {
        return runtime_->keyboard_report(modifiers, keycodes, sequence,
                                         originating_local_owner_id);
    }
    hid_runtime::HidTicketId ticket_id = 0;
    const auto begin = queue_ble_keyboard_report(
        modifiers, keycodes, sequence, &ticket_id,
        originating_local_owner_id);
    return runtime_->complete_keyboard_report(begin, ticket_id);
}

hid_runtime::MouseReportResult Controller::sequence_mouse_report(
    hid_runtime::SequenceAuthority sequence, std::uint8_t buttons,
    hid_runtime::ReportOriginOwnerId originating_local_owner_id) {
    const auto route = runtime_->state_machine().route_snapshot();
    if (route.active != hid_route::OutputRoute::kBle) {
        return runtime_->mouse_report(buttons, 0, 0, 0, 0, sequence,
                                      originating_local_owner_id);
    }
    hid_runtime::HidTicketId ticket_id = 0;
    const auto begin = queue_ble_mouse_report(
        buttons, 0, 0, 0, 0, sequence, &ticket_id,
        originating_local_owner_id);
    return runtime_->complete_mouse_report(begin, ticket_id);
}
#endif

void Controller::begin_ble_hid_peer(
    ble_lifecycle::Generation generation,
    std::uint16_t connection_handle) {
    clear_ble_hid_peer();
    if (ble_database_ == nullptr ||
        connection_handle == ble_lifecycle::kNoConnection) {
        return;
    }
    const auto handles = ble_database_->hid_handles();
    if (!ble_fixture_profile::reports_present(
            kStrictProfile, kStrictProfile.required_input_subscriptions) ||
        handles.report_map_value == 0 || handles.keyboard_value == 0 ||
        handles.mouse_value == 0 ||
        handles.control_point_value == 0 ||
        handles.report_map_value == handles.keyboard_value ||
        handles.report_map_value == handles.mouse_value ||
        handles.report_map_value == handles.control_point_value ||
        handles.keyboard_value == handles.mouse_value ||
        handles.keyboard_value == handles.control_point_value ||
        handles.mouse_value == handles.control_point_value) {
        return;
    }
    ble_hid_peer_ = {
        .generation = generation,
        .connection_handle = connection_handle,
        .handles = handles,
        .active = true,
    };
}

void Controller::clear_ble_hid_peer() { ble_hid_peer_ = {}; }

bool Controller::current_ble_hid_identity(
    BleHidWorkIdentity identity, BleHidInterface interface) const {
    if (!ble_hid_peer_.active || identity.generation != ble_hid_peer_.generation ||
        identity.connection_handle != ble_hid_peer_.connection_handle) {
        return false;
    }
    const auto role = interface == BleHidInterface::kKeyboard
                          ? hid_runtime::ReportRole::kKeyboardInput
                          : hid_runtime::ReportRole::kMouseInput;
    const std::uint16_t expected_handle =
        interface == BleHidInterface::kKeyboard
            ? ble_hid_peer_.handles.keyboard_value
            : interface == BleHidInterface::kMouse
                  ? ble_hid_peer_.handles.mouse_value
                  : 0;
    if (expected_handle == 0 ||
        identity.characteristic_handle != expected_handle) {
        return false;
    }
    if (identity.profile_activation_epoch == 0) {
#ifdef HID_CONTROL_EXECUTOR_NATIVE_TEST
        // Direct internal adapter tests do not create a runtime route
        // activation. Production deferred work must always name one.
        return true;
#else
        return false;
#endif
    }
    if (runtime_ == nullptr) {
        return false;
    }
    const auto authority =
        runtime_->state_machine().ble_route_authority_snapshot();
    return authority.coherent && authority.active &&
           authority.profile_activation_epoch ==
               identity.profile_activation_epoch &&
           authority.ble_generation == identity.generation &&
           authority.connection_handle == identity.connection_handle &&
           hid_capability::has_role(authority.present_roles, role) &&
           authority.report_handles.get(role) == expected_handle;
}

bool Controller::ble_hid_interface_ready(
    BleHidWorkIdentity identity, BleHidInterface interface) const {
    if (!current_ble_hid_identity(identity, interface) ||
        ble_backend_ == nullptr || ble_database_ == nullptr ||
        ble_lifecycle_handoff_failure_.load(std::memory_order_acquire) ||
        ble_hid_peer_.suspended) {
        return false;
    }
    const auto lifecycle = ble_state_.snapshot();
    if (ble_event_overflow_pending(lifecycle.generation) ||
        ble_route_loss_pending(lifecycle.generation)) {
        return false;
    }
    const auto handles = ble_database_->hid_handles();
    const std::uint16_t current_handle =
        interface == BleHidInterface::kKeyboard ? handles.keyboard_value
                                                : handles.mouse_value;
    const auto role = interface == BleHidInterface::kKeyboard
                          ? ble_fixture_profile::ReportRole::kKeyboardInput
                          : ble_fixture_profile::ReportRole::kMouseInput;
    const bool subscribed = interface == BleHidInterface::kKeyboard
                                ? ble_hid_peer_.keyboard_notify_enabled
                                : ble_hid_peer_.mouse_notify_enabled;
    return ble_fixture_profile::reports_present(
               kStrictProfile, ble_fixture_profile::report_bit(role)) &&
           current_handle == identity.characteristic_handle && subscribed &&
           lifecycle.generation == identity.generation &&
           lifecycle.desired == ble_lifecycle::DesiredExposure::kExposed &&
           lifecycle.observed == ble_lifecycle::ObservedState::kConnected &&
           lifecycle.connected && !lifecycle.recovery_required &&
           ble_state_.connection_handle() == identity.connection_handle &&
           ble_backend_->security_ready_for_hid(identity.generation,
                                                identity.connection_handle) &&
           ble_backend_->gatt_schema_current_for_hid(
               identity.generation, identity.connection_handle);
}

BleHidSubmitResult Controller::submit_ble_report(
    BleHidWorkIdentity identity, BleHidInterface interface,
    const std::uint8_t *payload, std::uint16_t payload_length) {
    if (!current_ble_hid_identity(identity, interface)) {
        return BleHidSubmitResult::kStale;
    }
    if (!ble_hid_interface_ready(identity, interface)) {
        return BleHidSubmitResult::kNotReady;
    }
    const auto result = ble_database_->notify_custom(
        identity.connection_handle, identity.characteristic_handle, payload,
        payload_length);
    switch (result) {
        case BleNotifyBackendResult::kStackAccepted:
            return BleHidSubmitResult::kStackAccepted;
        case BleNotifyBackendResult::kResourceFailure:
            return BleHidSubmitResult::kResourceFailure;
        case BleNotifyBackendResult::kStackRejected:
        default:
            return BleHidSubmitResult::kStackRejected;
    }
}

BleHidSubmitResult Controller::submit_ble_keyboard(
    BleHidWorkIdentity identity, const BleKeyboardReport &report) {
    return submit_ble_report(identity, BleHidInterface::kKeyboard,
                             report.data(), report.size());
}

BleHidSubmitResult Controller::submit_ble_mouse(
    BleHidWorkIdentity identity, const BleMouseReport &report) {
    BleMouseReport bounded = report;
    bounded[0] &= 0x1fU;
    return submit_ble_report(identity, BleHidInterface::kMouse,
                             bounded.data(), bounded.size());
}

bool Controller::ble_explicit_release_ready(
    hid_runtime::ReleaseAllSnapshot transaction,
    BleHidInterface interface) const {
    if (runtime_ == nullptr || !transaction.active ||
        transaction.transport != hid_runtime::HidTransport::kBle ||
        (interface != BleHidInterface::kKeyboard &&
         interface != BleHidInterface::kMouse)) {
        return false;
    }
    const auto role = interface == BleHidInterface::kKeyboard
        ? hid_runtime::ReportRole::kKeyboardInput
        : hid_runtime::ReportRole::kMouseInput;
    const BleHidWorkIdentity identity{
        .generation = transaction.transport_generation,
        .profile_activation_epoch = transaction.profile_activation_epoch,
        .connection_handle = transaction.connection_handle,
        .characteristic_handle = transaction.report_handles.get(role),
    };
    const auto authority =
        runtime_->state_machine().ble_route_authority_snapshot();
    return ble_link_ready() &&
           runtime_->state_machine().ble_release_all_route_continuity_matches(
               authority) &&
           ble_hid_interface_ready(identity, interface);
}

void Controller::drive_ble_explicit_release() {
    if (runtime_ == nullptr || ble_database_ == nullptr) {
        return;
    }
    hid_runtime::StateMachine &state = runtime_->state_machine();
    auto transaction = state.release_all_snapshot();
    if (!transaction.active ||
        transaction.transport != hid_runtime::HidTransport::kBle ||
        transaction.success_committed) {
        return;
    }
    for (const auto &item : {
             std::pair{hid_runtime::Interface::kKeyboard,
                       BleHidInterface::kKeyboard},
             std::pair{hid_runtime::Interface::kMouse,
                       BleHidInterface::kMouse},
         }) {
        transaction = state.release_all_snapshot();
        const auto action = state.prepare_ble_release_all_interface(
            transaction, item.first);
        if (action == hid_runtime::BleReleaseAllAction::kWaitForOldWork) {
            return;
        }
        if (action == hid_runtime::BleReleaseAllAction::kNotApplicable) {
            retire_ble_route_if_unready();
            return;
        }
        if (action == hid_runtime::BleReleaseAllAction::kAlreadyUp) {
            continue;
        }
        if (!ble_explicit_release_ready(transaction, item.second)) {
            state.fail_ble_release_all(transaction, item.first);
            retire_ble_route_if_unready();
            return;
        }
        const auto role = item.second == BleHidInterface::kKeyboard
            ? hid_runtime::ReportRole::kKeyboardInput
            : hid_runtime::ReportRole::kMouseInput;
        const std::uint8_t *neutral = item.second == BleHidInterface::kKeyboard
            ? kBleKeyboardAllUp.data()
            : kBleMouseAllUp.data();
        const std::uint16_t length = item.second == BleHidInterface::kKeyboard
            ? static_cast<std::uint16_t>(kBleKeyboardAllUp.size())
            : static_cast<std::uint16_t>(kBleMouseAllUp.size());
        const auto result = ble_database_->notify_custom(
            transaction.connection_handle,
            transaction.report_handles.get(role), neutral, length);
        if (result != BleNotifyBackendResult::kStackAccepted ||
            !ble_explicit_release_ready(transaction, item.second) ||
            !state.complete_ble_release_all_interface(transaction,
                                                      item.first)) {
            state.fail_ble_release_all(transaction, item.first);
            retire_ble_route_if_unready();
            return;
        }
    }
    transaction = state.release_all_snapshot();
    if (!ble_explicit_release_ready(transaction,
                                    BleHidInterface::kKeyboard) ||
        !ble_explicit_release_ready(transaction,
                                    BleHidInterface::kMouse) ||
        !state.commit_ble_release_all(transaction)) {
        retire_ble_route_if_unready();
    }
}

void Controller::retire_ble_route_if_unready() {
    if (runtime_ == nullptr) {
        return;
    }
    hid_runtime::StateMachine &state = runtime_->state_machine();
    const auto authority = state.ble_route_authority_snapshot();
    if (!authority.coherent) {
        return;
    }
    if (authority.active) {
        const auto transaction = state.release_all_snapshot();
        const bool explicit_release_owns_route =
            transaction.active &&
            transaction.transport == hid_runtime::HidTransport::kBle &&
            ble_link_ready() &&
            state.ble_release_all_route_continuity_matches(authority);
        if (!explicit_release_owns_route && !ble_route_ready()) {
            (void)state.retire_ble_route_if_matches(authority);
        }
    }
    drive_ble_route_retirement();
}

bool Controller::ble_route_release_identity_current(
    BleRouteReleaseIdentity identity) const {
    if (runtime_ == nullptr) {
        return false;
    }
    const hid_runtime::BleRouteAuthoritySnapshot expected{
        .authority_epoch = identity.authority_epoch,
        .route_generation = identity.route_generation,
        .ble_generation = identity.ble_generation,
        .connection_handle = identity.connection_handle,
        .profile_activation_epoch = identity.profile_activation_epoch,
        .present_roles = identity.present_roles,
        .required_input_subscriptions =
            identity.required_input_subscriptions,
        .report_handles = identity.report_handles,
        .active = false,
        .releasing = true,
        .release_epoch = identity.release_epoch,
    };
    return runtime_->state_machine().ble_route_release_matches(expected);
}

bool Controller::ble_safety_release_ready(
    BleRouteReleaseIdentity identity, BleHidInterface interface) const {
    if (!ble_route_release_identity_current(identity) ||
        ble_backend_ == nullptr || ble_database_ == nullptr ||
        ble_lifecycle_handoff_failure_.load(std::memory_order_acquire) ||
        ble_backend_->persistent_store_failure_observed() ||
        ble_route_loss_pending(identity.ble_generation) ||
        ble_event_overflow_pending(identity.ble_generation) ||
        !ble_hid_peer_.active || ble_hid_peer_.suspended ||
        ble_hid_peer_.generation != identity.ble_generation ||
        ble_hid_peer_.connection_handle != identity.connection_handle) {
        return false;
    }
    const auto lifecycle = ble_state_.snapshot();
    const auto handles = ble_database_->hid_handles();
    const bool keyboard = interface == BleHidInterface::kKeyboard;
    const auto role = keyboard ? hid_runtime::ReportRole::kKeyboardInput
                               : hid_runtime::ReportRole::kMouseInput;
    const std::uint16_t expected_handle =
        identity.report_handles.get(role);
    const std::uint16_t peer_handle =
        keyboard ? ble_hid_peer_.handles.keyboard_value
                 : ble_hid_peer_.handles.mouse_value;
    const std::uint16_t database_handle =
        keyboard ? handles.keyboard_value : handles.mouse_value;
    const bool subscribed = keyboard
                                ? ble_hid_peer_.keyboard_notify_enabled
                                : ble_hid_peer_.mouse_notify_enabled;
    return hid_capability::has_role(identity.present_roles, role) &&
           expected_handle != 0 && peer_handle == expected_handle &&
           database_handle == expected_handle && subscribed &&
           lifecycle.generation == identity.ble_generation &&
           lifecycle.desired == ble_lifecycle::DesiredExposure::kExposed &&
           lifecycle.observed == ble_lifecycle::ObservedState::kConnected &&
           lifecycle.connected && !lifecycle.recovery_required &&
           ble_state_.connection_handle() == identity.connection_handle &&
           ble_backend_->security_ready_for_hid(identity.ble_generation,
                                                identity.connection_handle);
}

void Controller::submit_ble_safety_release(
    BleRouteReleaseIdentity identity) {
    if (ble_safety_release_ready(identity, BleHidInterface::kKeyboard)) {
        // Stack acceptance is only a bounded delivery opportunity; it is not
        // a peer acknowledgment and never causes retry.
        (void)ble_database_->notify_custom(
            identity.connection_handle,
            identity.report_handles.get(
                hid_runtime::ReportRole::kKeyboardInput),
            kBleKeyboardAllUp.data(), kBleKeyboardAllUp.size());
    }
    if (ble_safety_release_ready(identity, BleHidInterface::kMouse)) {
        (void)ble_database_->notify_custom(
            identity.connection_handle,
            identity.report_handles.get(
                hid_runtime::ReportRole::kMouseInput),
            kBleMouseAllUp.data(), kBleMouseAllUp.size());
    }
}

void Controller::start_ble_route_disconnect(
    BleRouteReleaseIdentity identity) {
    if (ble_route_release_phase_ != BleRouteReleasePhase::kGrace ||
        !ble_route_release_identity_current(identity)) {
        return;
    }
    ble_route_release_phase_ = BleRouteReleasePhase::kDisconnecting;
    const std::int32_t result =
        ble_backend_->disconnect(identity.connection_handle);
    if (result == 0) {
        return;
    }
    // No Disconnect operation/watchdog exists. Keep the route releasing: a
    // later exact Disconnect may still prove physical retirement, while the
    // lifecycle fault truthfully records present uncertainty.
    ble_route_release_phase_ = BleRouteReleasePhase::kFault;
    ble_backend_->mark_security_unhealthy(identity.ble_generation);
    fail_ble(identity.ble_generation, ble_lifecycle::Operation::kRuntime,
             result, ControlOperation::kNone);
    release_operation(ble_route_release_owner_);
    ble_route_release_owner_ = ControlOperation::kNone;
}

void Controller::note_ble_route_disconnect_result(
    ble_lifecycle::Generation generation, std::uint16_t connection_handle,
    std::int32_t result) {
    if (runtime_ == nullptr) {
        return;
    }
    const auto release =
        runtime_->state_machine().ble_route_authority_snapshot();
    if (!release.coherent || !release.releasing || release.active ||
        release.ble_generation != generation ||
        release.connection_handle != connection_handle) {
        return;
    }
    const BleRouteReleaseIdentity identity{
        .authority_epoch = release.authority_epoch,
        .route_generation = release.route_generation,
        .profile_activation_epoch = release.profile_activation_epoch,
        .ble_generation = release.ble_generation,
        .connection_handle = release.connection_handle,
        .present_roles = release.present_roles,
        .required_input_subscriptions =
            release.required_input_subscriptions,
        .report_handles = release.report_handles,
        .release_epoch = release.release_epoch,
    };
    cancel_ble_route_release_grace(identity);
    ble_route_release_ = identity;
    ble_route_release_phase_ = result == 0
                                   ? BleRouteReleasePhase::kDisconnecting
                                   : BleRouteReleasePhase::kFault;
    if (result != 0) {
        ControlOperation owner = ble_route_release_owner_;
        if (owner == ControlOperation::kNone &&
            active_operation_.load(std::memory_order_acquire) ==
                ControlOperation::kRouteChange) {
            owner = ControlOperation::kRouteChange;
        }
        release_operation(owner);
        ble_route_release_owner_ = ControlOperation::kNone;
    }
}

void Controller::cancel_ble_route_release_grace(
    BleRouteReleaseIdentity identity) {
    if (ble_route_grace_claim_.valid() &&
        BleRouteGraceAuthority::identities_equal(
            ble_route_grace_claim_.identity, identity)) {
        ble_route_grace_authority_.cancel(ble_route_grace_claim_);
        ble_route_grace_claim_ = {};
    }
    if (ble_backend_ != nullptr) {
        ble_backend_->cancel_ble_route_release_grace(identity);
    }
}

void Controller::drive_ble_route_retirement() {
    if (runtime_ == nullptr || ble_backend_ == nullptr) {
        return;
    }
    const auto release =
        runtime_->state_machine().ble_route_authority_snapshot();
    if (!release.coherent || !release.releasing || release.active) {
        return;
    }
    const BleRouteReleaseIdentity identity{
        .authority_epoch = release.authority_epoch,
        .route_generation = release.route_generation,
        .profile_activation_epoch = release.profile_activation_epoch,
        .ble_generation = release.ble_generation,
        .connection_handle = release.connection_handle,
        .present_roles = release.present_roles,
        .required_input_subscriptions =
            release.required_input_subscriptions,
        .report_handles = release.report_handles,
        .release_epoch = release.release_epoch,
    };
    if (ble_route_disconnect_observed_.exchange(false,
                                                std::memory_order_acq_rel)) {
        complete_ble_route_release_on_disconnect({
            .kind = BleEventKind::kDisconnect,
            .generation = identity.ble_generation,
            .connection_handle = identity.connection_handle,
        });
        return;
    }
    if (ble_route_release_phase_ == BleRouteReleasePhase::kNone) {
        // A callback-side loss fences normal work immediately. Wait until its
        // queued executor event has updated CCCD/suspend/security truth before
        // deciding which exact safety notifications remain usable.
        if (ble_route_loss_pending(identity.ble_generation) ||
            ble_event_overflow_pending(identity.ble_generation)) {
            return;
        }
        ble_route_release_ = identity;
        const auto active_owner =
            active_operation_.load(std::memory_order_acquire);
        if (active_owner == ControlOperation::kRouteChange) {
            ble_route_release_owner_ = active_owner;
        }
        submit_ble_safety_release(identity);
        ble_route_release_phase_ = BleRouteReleasePhase::kGrace;
        ble_route_grace_claim_ = ble_route_grace_authority_.arm(identity);
        if (!ble_route_grace_claim_.valid() ||
            ble_backend_->arm_ble_route_release_grace(identity) != 0) {
            ble_route_grace_authority_.cancel(ble_route_grace_claim_);
            ble_route_grace_claim_ = {};
            start_ble_route_disconnect(identity);
        }
        return;
    }
    if (ble_route_release_phase_ == BleRouteReleasePhase::kGrace &&
        !ble_route_loss_pending(identity.ble_generation) &&
        ble_route_grace_authority_.consume_due(ble_route_grace_claim_,
                                               identity)) {
        ble_route_grace_claim_ = {};
        start_ble_route_disconnect(identity);
    }
}

void Controller::complete_ble_route_release_on_disconnect(BleEvent event) {
    if (runtime_ == nullptr ||
        (event.kind != BleEventKind::kDisconnect &&
         event.kind != BleEventKind::kReset)) {
        return;
    }
    const auto release =
        runtime_->state_machine().ble_route_authority_snapshot();
    const bool exact_physical_loss =
        release.coherent && release.releasing && !release.active &&
        release.ble_generation == event.generation &&
        (event.kind == BleEventKind::kReset ||
         release.connection_handle == event.connection_handle);
    if (!exact_physical_loss) {
        return;
    }
    const BleRouteReleaseIdentity identity{
        .authority_epoch = release.authority_epoch,
        .route_generation = release.route_generation,
        .profile_activation_epoch = release.profile_activation_epoch,
        .ble_generation = release.ble_generation,
        .connection_handle = release.connection_handle,
        .present_roles = release.present_roles,
        .required_input_subscriptions =
            release.required_input_subscriptions,
        .report_handles = release.report_handles,
        .release_epoch = release.release_epoch,
    };
    cancel_ble_route_release_grace(identity);
    ble_route_disconnect_observed_.store(false, std::memory_order_release);
    if (!runtime_->state_machine().complete_ble_route_release_if_matches(
            release)) {
        return;
    }
    ControlOperation owner = ble_route_release_owner_;
    if (owner == ControlOperation::kNone &&
        active_operation_.load(std::memory_order_acquire) ==
            ControlOperation::kRouteChange) {
        // An exact physical loss may beat the first executor wake after an
        // explicit route-none Stage A. Recover that still-exact owner here.
        owner = ControlOperation::kRouteChange;
    }
    ble_route_release_ = {};
    ble_route_release_phase_ = BleRouteReleasePhase::kNone;
    ble_route_release_owner_ = ControlOperation::kNone;
    release_operation(owner);
}

hid_runtime::BleSubmitResult Controller::submit_runtime_ble_report(
    void *context, hid_runtime::Interface interface,
    hid_runtime::HidWorkToken token, const std::uint8_t *payload,
    std::uint16_t payload_length) {
    auto *controller = static_cast<Controller *>(context);
    if (controller == nullptr || controller->runtime_ == nullptr ||
        !controller->runtime_->state_machine().ble_work_token_current(
            interface, token)) {
        return hid_runtime::BleSubmitResult::kStale;
    }
    const BleHidWorkIdentity identity{
        .generation = token.transport_generation,
        .profile_activation_epoch = token.profile_activation_epoch,
        .connection_handle = token.connection_handle,
        .characteristic_handle = token.characteristic_handle,
    };
    BleHidSubmitResult result = BleHidSubmitResult::kStackRejected;
    if (interface == hid_runtime::Interface::kKeyboard &&
        token.report_kind == hid_runtime::ReportKind::kUnsafeKeyboard &&
        payload != nullptr && payload_length == BleKeyboardReport{}.size()) {
        BleKeyboardReport report{};
        std::memcpy(report.data(), payload, report.size());
        result = controller->submit_ble_keyboard(identity, report);
    } else if (interface == hid_runtime::Interface::kMouse &&
               token.report_kind == hid_runtime::ReportKind::kUnsafeMouse &&
               payload != nullptr && payload_length == BleMouseReport{}.size()) {
        BleMouseReport report{};
        std::memcpy(report.data(), payload, report.size());
        result = controller->submit_ble_mouse(identity, report);
    }
    switch (result) {
        case BleHidSubmitResult::kStackAccepted:
            return hid_runtime::BleSubmitResult::kStackAccepted;
        case BleHidSubmitResult::kNotReady:
            return hid_runtime::BleSubmitResult::kNotReady;
        case BleHidSubmitResult::kStale:
            return hid_runtime::BleSubmitResult::kStale;
        case BleHidSubmitResult::kResourceFailure:
            return hid_runtime::BleSubmitResult::kResourceFailure;
        case BleHidSubmitResult::kStackRejected:
        default:
            return hid_runtime::BleSubmitResult::kStackRejected;
    }
}

bool Controller::enqueue(Action item) {
#ifdef HID_CONTROL_EXECUTOR_NATIVE_TEST
    if (fail_next_enqueue_) {
        fail_next_enqueue_ = false;
        return false;
    }
    if (native_count_ == kActionQueueDepth) {
        return false;
    }
    native_queue_[(native_head_ + native_count_) % kActionQueueDepth] = item;
    ++native_count_;
    request_executor_wake();
    return true;
#else
    if (xQueueSend(s_action_queue, &item, 0) != pdPASS) {
        return false;
    }
    request_executor_wake();
    return true;
#endif
}

void Controller::request_executor_wake() {
#ifdef HID_CONTROL_EXECUTOR_NATIVE_TEST
    native_executor_wake_pending_.store(true, std::memory_order_release);
#else
    if (s_executor_task != nullptr) {
        (void)xTaskNotifyGive(s_executor_task);
    }
#endif
}

void Controller::request_executor_wake_from_isr() {
#ifdef HID_CONTROL_EXECUTOR_NATIVE_TEST
    native_executor_wake_pending_.store(true, std::memory_order_release);
#else
    if (s_executor_task != nullptr) {
        BaseType_t higher_priority_task_woken = pdFALSE;
        vTaskNotifyGiveFromISR(s_executor_task, &higher_priority_task_woken);
        portYIELD_FROM_ISR(higher_priority_task_woken);
    }
#endif
}

void Controller::signal_usb_runtime_fault(UsbRuntimeFaultReason reason,
                                          std::uint32_t event_id,
                                          bool in_isr) {
    bool occurrence_recorded = false;
    for (std::uint8_t attempt = 0; attempt < 4U; ++attempt) {
        std::uint32_t count =
            usb_runtime_fault_occurrences_.load(std::memory_order_acquire);
        if (count == std::numeric_limits<std::uint32_t>::max() ||
            usb_runtime_fault_occurrences_.compare_exchange_weak(
                count, count + 1U, std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            occurrence_recorded = true;
            break;
        }
    }
    if (!occurrence_recorded) {
        // Contention itself means the exact count is no longer useful. Keep
        // callback work bounded and conservatively saturate the diagnostic.
        usb_runtime_fault_occurrences_.store(
            std::numeric_limits<std::uint32_t>::max(),
            std::memory_order_release);
    }
    if (in_isr) {
        usb_runtime_fault_in_isr_.store(true, std::memory_order_release);
    }

    if (reason == UsbRuntimeFaultReason::kEventQueueOverflow) {
        std::uint32_t no_event = UINT32_MAX;
        (void)usb_runtime_fault_event_.compare_exchange_strong(
            no_event, event_id, std::memory_order_acq_rel,
            std::memory_order_acquire);
        // The queue-overflow classification is more specific than the generic
        // diagnostic sink, so it may replace kNone or kDiagnostic directly.
        usb_runtime_fault_reason_.store(
            UsbRuntimeFaultReason::kEventQueueOverflow,
            std::memory_order_release);
    } else {
        UsbRuntimeFaultReason none = UsbRuntimeFaultReason::kNone;
        (void)usb_runtime_fault_reason_.compare_exchange_strong(
            none, UsbRuntimeFaultReason::kDiagnostic,
            std::memory_order_acq_rel, std::memory_order_acquire);
    }

    const UsbRuntimeFaultReason retained =
        usb_runtime_fault_reason_.load(std::memory_order_acquire);
    const std::int32_t code =
        retained == UsbRuntimeFaultReason::kEventQueueOverflow
            ? usb_lifecycle::kTinyUsbEventQueueOverflowError
            : usb_lifecycle::kTinyUsbRuntimeDiagnosticError;
    if (runtime_ != nullptr) {
        if (!runtime_->state_machine().begin_usb_runtime_fault(code)) {
            runtime_->state_machine().update_usb_runtime_fault(code);
        }
    }
    if (in_isr) {
        request_executor_wake_from_isr();
    } else {
        request_executor_wake();
    }
}

void Controller::signal_usb_lifecycle_event(UsbLifecycleEvent event) {
    usb_lifecycle_log_bits_.fetch_or(static_cast<std::uint8_t>(event),
                                     std::memory_order_acq_rel);
    request_executor_wake();
}

UsbRuntimeFaultSnapshot Controller::usb_runtime_fault_snapshot() const {
    return UsbRuntimeFaultSnapshot{
        .reason = usb_runtime_fault_reason_.load(std::memory_order_acquire),
        .first_event_id =
            usb_runtime_fault_event_.load(std::memory_order_acquire),
        .occurrences =
            usb_runtime_fault_occurrences_.load(std::memory_order_acquire),
        .observed_in_isr =
            usb_runtime_fault_in_isr_.load(std::memory_order_acquire),
    };
}

bool Controller::reconcile_usb_runtime_fault() {
    const UsbRuntimeFaultSnapshot fault = usb_runtime_fault_snapshot();
    if (fault.reason == UsbRuntimeFaultReason::kNone || runtime_ == nullptr ||
        backend_ == nullptr) {
        return false;
    }
    const std::int32_t code =
        fault.reason == UsbRuntimeFaultReason::kEventQueueOverflow
            ? usb_lifecycle::kTinyUsbEventQueueOverflowError
            : usb_lifecycle::kTinyUsbRuntimeDiagnosticError;
    runtime_->state_machine().update_usb_runtime_fault(code);
    if (usb_runtime_fault_committed_) {
        return false;
    }
    usb_runtime_fault_committed_ = true;
    active_operation_.store(ControlOperation::kNone,
                            std::memory_order_release);
    runtime_->state_machine().commit_usb_runtime_fault_shutdown();
#ifndef HID_CONTROL_EXECUTOR_NATIVE_TEST
    if (fault.reason == UsbRuntimeFaultReason::kEventQueueOverflow) {
        ESP_LOGE(kLogTag,
                 "TinyUSB event queue overflow; event=%lu count=%lu isr=%s",
                 static_cast<unsigned long>(fault.first_event_id),
                 static_cast<unsigned long>(fault.occurrences),
                 fault.observed_in_isr ? "yes" : "no");
    } else {
        ESP_LOGE(kLogTag, "TinyUSB runtime diagnostic; count=%lu isr=%s",
                 static_cast<unsigned long>(fault.occurrences),
                 fault.observed_in_isr ? "yes" : "no");
    }
#endif

#ifdef HID_CONTROL_EXECUTOR_NATIVE_TEST
    const hid_runtime::LifecycleSafetyResult safety =
        runtime_->state_machine().begin_lifecycle_detach_safety();
    if (safety != hid_runtime::LifecycleSafetyResult::kClean) {
        runtime_->state_machine().mark_lifecycle_detach_uncertain(
            runtime_->state_machine().attach_generation());
    }
#else
    (void)runtime_->run_lifecycle_detach_safety();
#endif
    runtime_->state_machine().begin_usb_uninstall();
    const BackendResult result = backend_->uninstall();
    const bool uninstalled = result.kind == BackendResultKind::kSuccess;
    if (uninstalled) {
        runtime_->state_machine().on_driver_uninstalled();
    }
    runtime_->state_machine().complete_usb_runtime_fault(uninstalled);
    return true;
}

void Controller::disarm_usb_sof_watchdog() {
    usb_sof_watchdog_ = {};
}

bool Controller::reconcile_usb_sof_watchdog(std::uint32_t now_ms) {
    if (runtime_ == nullptr) {
        disarm_usb_sof_watchdog();
        return false;
    }

    hid_runtime::UsbLinkWatchdogSnapshot current{};
    if (!runtime_->state_machine().usb_link_watchdog_snapshot(&current)) {
        if (usb_sof_watchdog_.armed && usb_sof_watchdog_.pending) {
            hid_route::ConditionalInvalidationToken conditional_token =
                usb_sof_watchdog_.last_progress_ms;
            const hid_runtime::UsbLinkStallResult result =
                runtime_->state_machine().on_usb_link_stall(
                    usb_sof_watchdog_.identity, &conditional_token);
            if (result == hid_runtime::UsbLinkStallResult::kPending) {
                usb_sof_watchdog_.last_progress_ms = conditional_token;
                return false;
            }
            if (result == hid_runtime::UsbLinkStallResult::kApplied) {
                disarm_usb_sof_watchdog();
                usb_lifecycle_log_bits_.fetch_or(
                    static_cast<std::uint8_t>(UsbLifecycleEvent::kSofStall),
                    std::memory_order_acq_rel);
                if (usb_link_stall_sink_ != nullptr) {
                    usb_link_stall_sink_();
                }
                return true;
            }
            hid_runtime::UsbLinkWatchdogSnapshot refreshed{};
            if (runtime_->state_machine().usb_link_watchdog_snapshot(
                    &refreshed)) {
                usb_sof_watchdog_.identity = refreshed;
                usb_sof_watchdog_.last_progress_ms = now_ms;
                usb_sof_watchdog_.armed = true;
                usb_sof_watchdog_.pending = false;
                return false;
            }
        }
        disarm_usb_sof_watchdog();
        return false;
    }

    if (!usb_sof_watchdog_.armed ||
        !same_usb_link_watchdog_identity(usb_sof_watchdog_.identity,
                                         current)) {
        usb_sof_watchdog_.identity = current;
        usb_sof_watchdog_.last_progress_ms = now_ms;
        usb_sof_watchdog_.armed = true;
        usb_sof_watchdog_.pending = false;
        return false;
    }

    if (usb_sof_watchdog_.identity.sof_heartbeat != current.sof_heartbeat) {
        usb_sof_watchdog_.identity = current;
        usb_sof_watchdog_.last_progress_ms = now_ms;
        usb_sof_watchdog_.pending = false;
        return false;
    }

    const std::uint32_t stalled_ms =
        now_ms - usb_sof_watchdog_.last_progress_ms;
    if (stalled_ms < kUsbSofStallTimeoutMs) {
        return false;
    }

#ifdef HID_CONTROL_EXECUTOR_NATIVE_TEST
    if (sof_watchdog_before_fence_hook_ != nullptr) {
        sof_watchdog_before_fence_hook_(*this);
    }
#endif
    hid_route::ConditionalInvalidationToken conditional_token =
        hid_route::kNoConditionalInvalidationToken;
    const hid_runtime::UsbLinkStallResult result =
        runtime_->state_machine().on_usb_link_stall(current,
                                                    &conditional_token);
    if (result == hid_runtime::UsbLinkStallResult::kPending) {
        usb_sof_watchdog_.last_progress_ms = conditional_token;
        usb_sof_watchdog_.pending = true;
        return false;
    }
    if (result != hid_runtime::UsbLinkStallResult::kApplied) {
        hid_runtime::UsbLinkWatchdogSnapshot refreshed{};
        if (runtime_->state_machine().usb_link_watchdog_snapshot(&refreshed)) {
            usb_sof_watchdog_.identity = refreshed;
            usb_sof_watchdog_.last_progress_ms = now_ms;
            usb_sof_watchdog_.armed = true;
            usb_sof_watchdog_.pending = false;
        } else {
            disarm_usb_sof_watchdog();
        }
        return false;
    }

    disarm_usb_sof_watchdog();
    usb_lifecycle_log_bits_.fetch_or(
        static_cast<std::uint8_t>(UsbLifecycleEvent::kSofStall),
        std::memory_order_acq_rel);
    if (usb_link_stall_sink_ != nullptr) {
        usb_link_stall_sink_();
    }
    return true;
}

void Controller::reconcile_usb_lifecycle_logs() {
    const std::uint8_t events =
        usb_lifecycle_log_bits_.exchange(0, std::memory_order_acq_rel);
#ifndef HID_CONTROL_EXECUTOR_NATIVE_TEST
    if ((events & static_cast<std::uint8_t>(UsbLifecycleEvent::kMounted)) != 0) {
        ESP_LOGI(kLogTag, "USB HID mounted");
    }
    if ((events & static_cast<std::uint8_t>(UsbLifecycleEvent::kUnmounted)) != 0) {
        ESP_LOGI(kLogTag, "USB HID unmounted");
    }
    if ((events & static_cast<std::uint8_t>(UsbLifecycleEvent::kSuspended)) != 0) {
        ESP_LOGI(kLogTag, "USB HID suspended");
    }
    if ((events & static_cast<std::uint8_t>(UsbLifecycleEvent::kResumed)) != 0) {
        ESP_LOGI(kLogTag, "USB HID resumed");
    }
    if ((events & static_cast<std::uint8_t>(UsbLifecycleEvent::kSofStall)) != 0) {
        ESP_LOGW(kLogTag, "USB HID link activity lost (SOF stall)");
    }
#else
    (void)events;
#endif
}

void Controller::signal_hid_authority_change() {
    request_executor_wake();
}

bool Controller::schedule(usb_lifecycle::ExecutorAction action,
                          usb_lifecycle::Snapshot snapshot) {
    const ControlOperation operation = operation_for(action);
    // The request entry point must own the cross-domain guard before USB
    // lifecycle Stage-A. Scheduling only verifies inherited ownership.
    if (!initialized_ ||
        active_operation_.load(std::memory_order_acquire) != operation) {
        return false;
    }
    const Action item = Action::with_lifecycle(
        action == usb_lifecycle::ExecutorAction::kInstall
            ? ActionKind::kUsbInstall
            : ActionKind::kUsbDetach,
        snapshot, runtime_->state_machine().route_snapshot(), operation);
    return enqueue(item);
}

void Controller::process(Action action) {
    if (runtime_ == nullptr || backend_ == nullptr) {
        if (action.kind == ActionKind::kUsbInstall ||
            action.kind == ActionKind::kUsbDetach ||
            action.kind == ActionKind::kRouteRelease) {
            release_operation(action.payload.lifecycle.operation);
        } else if (action.kind == ActionKind::kBleEnable ||
                   action.kind == ActionKind::kBleDisable ||
                   action.kind == ActionKind::kRouteBleActivate ||
                   action.kind == ActionKind::kBondRemove) {
            release_operation(action.payload.operation.operation);
        }
        return;
    }
    (void)reconcile_usb_runtime_fault();
    const bool suppress_ble_event = reconcile_ble_fallbacks(&action);
    retire_ble_route_if_unready();
#ifdef HID_CONTROL_EXECUTOR_NATIVE_TEST
    if (process_after_reconciliation_hook_ != nullptr) {
        const auto hook = process_after_reconciliation_hook_;
        process_after_reconciliation_hook_ = nullptr;
        hook(*this);
    }
#endif
    if (suppress_ble_event && action.kind == ActionKind::kBleEvent) {
        return;
    }
    hid_runtime::StateMachine &state = runtime_->state_machine();
    if (action.kind == ActionKind::kBleHidReport) {
        (void)state.process_ble_report(
            action.payload.hid_report.interface,
            action.payload.hid_report.work,
            submit_runtime_ble_report, this);
        retire_ble_route_if_unready();
        return;
    }
    if (action.kind == ActionKind::kBleRouteReleaseGrace) {
        // The action is only a wake hint. The exact callback published an
        // identity-owned due slot that this owner consumes coherently.
        drive_ble_route_retirement();
        return;
    }
    if (action.kind == ActionKind::kRouteBleActivate) {
        const auto payload = action.payload.operation;
        if (pairing_rpc_pending_.load(std::memory_order_acquire) !=
            payload.mailbox_token) {
            release_operation(payload.operation);
            return;
        }
        if (active_operation_.load(std::memory_order_acquire) ==
            payload.operation) {
            route_rpc_result_ = activate_ble_route();
        } else {
            route_rpc_result_ = {};
        }
        if (ble_route_release_owner_ != payload.operation) {
            release_operation(payload.operation);
        }
        complete_pairing_rpc(payload.mailbox_token);
        return;
    }
    if (action.kind == ActionKind::kPairingStatus) {
        const auto payload = action.payload.operation;
        if (pairing_rpc_pending_.load(std::memory_order_acquire) !=
            payload.mailbox_token) {
            return;
        }
        reconcile_pairing_deadline();
        pairing_rpc_status_ = current_pairing_status();
        complete_pairing_rpc(payload.mailbox_token);
        return;
    }
    if (action.kind == ActionKind::kPairingRespond) {
        const auto payload = action.payload.operation;
        if (pairing_rpc_pending_.load(std::memory_order_acquire) !=
            payload.mailbox_token) {
            return;
        }
        std::array<char, 6> secret{};
        const bool mailbox_current = pairing_mailbox_.occupied &&
                                     pairing_mailbox_.token == payload.mailbox_token;
        const auto generation = pairing_mailbox_.generation;
        const auto connection = pairing_mailbox_.connection_handle;
        const auto pairing_id = pairing_mailbox_.pairing_id;
        if (mailbox_current) {
            secret = pairing_mailbox_.secret;
        }
        wipe_pairing_mailbox();
        reconcile_pairing_deadline();
        pairing_rpc_result_ = mailbox_current
            ? respond_to_pairing(generation, connection, pairing_id, secret)
            : ble_pairing::RespondResult::kNotPending;
        secure_memory::zero(secret.data(), secret.size());
        complete_pairing_rpc(payload.mailbox_token);
        return;
    }
    if (action.kind == ActionKind::kBondList) {
        const auto payload = action.payload.operation;
        if (pairing_rpc_pending_.load(std::memory_order_acquire) !=
            payload.mailbox_token) {
            return;
        }
        const auto lifecycle = ble_state_.snapshot();
        if (ble_backend_->persistent_store_failure_observed()) {
            bond_list_rpc_result_ = {
                .kind = BleBondListResultKind::kStorageFailure};
        } else if (!lifecycle.stack_ready || lifecycle.recovery_required) {
            bond_list_rpc_result_ = {};
        } else {
            bond_list_rpc_result_ = ble_backend_->list_bonds();
        }
        complete_pairing_rpc(payload.mailbox_token);
        return;
    }
    if (action.kind == ActionKind::kBondRemove) {
        const auto payload = action.payload.operation;
        if (pairing_rpc_pending_.load(std::memory_order_acquire) !=
            payload.mailbox_token) {
            release_operation(payload.operation);
            return;
        }
        const BondId bond_id = bond_remove_mailbox_;
        bond_remove_mailbox_ = {};
        const auto lifecycle = ble_state_.snapshot();
        if (ble_backend_->persistent_store_failure_observed()) {
            bond_remove_rpc_result_ = {
                .kind = BleBondRemoveResultKind::kStorageFailure,
                .bond_id = bond_id};
        } else if (!lifecycle.stack_ready) {
            bond_remove_rpc_result_ = {
                .kind = BleBondRemoveResultKind::kNotReady,
                .bond_id = bond_id};
        } else if (!bond_remove_eligible()) {
            bond_remove_rpc_result_ = {
                .kind = BleBondRemoveResultKind::kBusy,
                .bond_id = bond_id};
        } else {
            bond_remove_rpc_result_ = ble_backend_->remove_bond(bond_id);
        }
        release_operation(payload.operation);
        complete_pairing_rpc(payload.mailbox_token);
        return;
    }
    if (action.kind == ActionKind::kBleEvent) {
        const BleEvent event = action.payload.ble_event.event;
        complete_ble_route_release_on_disconnect(event);
        process_ble_event(event);
        if (event_immediately_loses_ble_hid_readiness(event)) {
            clear_ble_route_loss(event.generation);
        }
        retire_ble_route_if_unready();
        return;
    }
    if (action.kind == ActionKind::kBleEnable) {
        const ControlOperation operation = action.payload.operation.operation;
        const auto current = ble_state_.snapshot();
        if (active_operation_.load(std::memory_order_acquire) != operation ||
            current.desired != ble_lifecycle::DesiredExposure::kExposed ||
            current.observed != ble_lifecycle::ObservedState::kEnabling) {
            release_operation(operation);
            return;
        }
        ble_backend_->set_generation(current.generation);
        if (!current.stack_ready) {
            ble_backend_->record_heap_checkpoint(
                BleBackend::HeapCheckpoint::kBeforeFirstEnable);
            const std::int32_t result = ble_backend_->initialize(
                this, ble_database_, current.generation);
            if (result != 0) {
                fail_ble(current.generation, ble_lifecycle::Operation::kEnable,
                         result, operation);
            }
            return;
        }
        if (ble_backend_->persistent_store_failure_observed()) {
            return;
        }
        const std::int32_t result = ble_backend_->start_advertising();
        if (result == 0) {
            ble_state_.complete_advertising(current.generation);
            ble_backend_->record_heap_checkpoint(
                BleBackend::HeapCheckpoint::kAdvertising);
            release_operation(operation);
        } else {
            fail_ble(current.generation, ble_lifecycle::Operation::kEnable, result,
                     operation);
        }
        return;
    }
    if (action.kind == ActionKind::kBleDisable) {
        const ControlOperation operation = action.payload.operation.operation;
        const auto current = ble_state_.snapshot();
        if (active_operation_.load(std::memory_order_acquire) != operation ||
            current.desired != ble_lifecycle::DesiredExposure::kHidden ||
            current.observed != ble_lifecycle::ObservedState::kDisabling) {
            release_operation(operation);
            return;
        }
        // Stage A already made public readiness fail closed. Everything below,
        // including compound security retirement, runs only in this executor.
        clear_ble_hid_peer();
        retire_ble_route_if_unready();
        wipe_pairing_mailbox();
        pairing_deadline_us_ = 0;
        pairing_state_.disable();
        pairing_complete_seen_ = false;
        ble_backend_->cancel_pairing_timeout();
        const auto security = ble_backend_->security_snapshot();
        if (security.coherent && security.connected) {
            ble_backend_->retire_security(security.generation,
                                          security.connection_handle);
        }
        ble_backend_->set_generation(current.generation);
        if (current.advertising) {
            const std::int32_t result = ble_backend_->stop_advertising();
            if (result != 0) {
                fail_ble(current.generation, ble_lifecycle::Operation::kDisable,
                         result, operation);
                return;
            }
        }
        if (current.connected) {
            const std::int32_t result =
                ble_backend_->disconnect(ble_state_.connection_handle());
            if (result != 0) {
                fail_ble(current.generation, ble_lifecycle::Operation::kDisable,
                         result, operation);
            }
            return;
        }
        ble_state_.complete_disable(current.generation);
        ble_backend_->record_heap_checkpoint(
            BleBackend::HeapCheckpoint::kHiddenIdle);
        release_operation(operation);
        return;
    }
    if (action.kind == ActionKind::kRouteRelease) {
        const auto payload = action.payload.lifecycle;
        const hid_route::Snapshot route = state.route_snapshot();
        const usb_lifecycle::Snapshot lifecycle = state.usb_lifecycle_snapshot();
        const bool expected_route = route.coherent && !route.invalidation_pending &&
                                    route.desired == hid_route::OutputRoute::kNone &&
                                    route.active == hid_route::OutputRoute::kUsb &&
                                    route.transition == hid_route::Transition::kReleasing &&
                                    route.generation == payload.route.generation;
        const bool expected_transport =
            lifecycle.generation == payload.lifecycle.generation &&
            lifecycle.desired == usb_lifecycle::DesiredExposure::kExposed &&
            lifecycle.observed == usb_lifecycle::ObservedState::kMounted;
        if (active_operation_.load(std::memory_order_acquire) != payload.operation ||
            !expected_route || !expected_transport) {
            if (expected_route) {
                state.terminalize_route_release_schedule_failure(payload.route);
            }
            release_operation(payload.operation);
            return;
        }
#ifdef HID_CONTROL_EXECUTOR_NATIVE_TEST
        const hid_runtime::LifecycleSafetyResult safety =
            state.begin_route_release_safety(payload.route);
        if (safety != hid_runtime::LifecycleSafetyResult::kClean) {
            state.mark_lifecycle_detach_uncertain(payload.lifecycle.generation);
        }
#else
        (void)runtime_->run_route_release_safety(payload.route);
#endif
        state.complete_route_release(payload.route);
        release_operation(payload.operation);
        return;
    }
    const auto payload = action.payload.lifecycle;
    const usb_lifecycle::Snapshot current = state.usb_lifecycle_snapshot();
    const bool expected_lifecycle_state =
        action.kind == ActionKind::kUsbInstall
            ? current.desired == usb_lifecycle::DesiredExposure::kExposed &&
                  current.observed == usb_lifecycle::ObservedState::kAttaching
            : current.desired == usb_lifecycle::DesiredExposure::kHidden &&
                  current.observed == usb_lifecycle::ObservedState::kDetaching;
    if (active_operation_.load(std::memory_order_acquire) != payload.operation ||
        !expected_lifecycle_state ||
        current.desired != payload.lifecycle.desired ||
        current.observed != payload.lifecycle.observed ||
        current.generation != payload.lifecycle.generation) {
        release_operation(payload.operation);
        return;
    }
    if (action.kind == ActionKind::kUsbInstall) {
        const BackendResult result = backend_->install();
        switch (result.kind) {
            case BackendResultKind::kSuccess:
                // The mount callback may already have changed observed to
                // mounted; complete_install_success intentionally preserves it.
                state.complete_usb_install_success();
                break;
            case BackendResultKind::kCleanInstallFailure:
                state.complete_usb_install_clean_failure(result.error_code);
                break;
            case BackendResultKind::kAmbiguousInstallFailure:
                state.complete_usb_install_ambiguous_failure(result.error_code);
                break;
            case BackendResultKind::kUninstallFailure:
                state.complete_usb_install_ambiguous_failure(result.error_code);
                break;
        }
        release_operation(payload.operation);
        return;
    }

#ifdef HID_CONTROL_EXECUTOR_NATIVE_TEST
    const hid_runtime::LifecycleSafetyResult safety = state.begin_lifecycle_detach_safety();
    if (safety != hid_runtime::LifecycleSafetyResult::kClean) {
        state.mark_lifecycle_detach_uncertain(payload.lifecycle.generation);
    }
#else
    (void)runtime_->run_lifecycle_detach_safety();
#endif
    state.complete_usb_detach_route_invalidation(payload.route);
    state.begin_usb_uninstall();
    const BackendResult result = backend_->uninstall();
    if (result.kind == BackendResultKind::kSuccess) {
        state.on_driver_uninstalled();
        state.complete_usb_uninstall_success();
    } else {
        state.complete_usb_uninstall_failure(result.error_code);
    }
    release_operation(payload.operation);
}

bool Controller::reconcile_ble_fallbacks(const Action *action) {
    // The backend latch is boot-lifetime global truth, unlike the
    // generation/connection-scoped overflow mailbox. Reconcile it before
    // consuming an overflow identity so disable Stage A or disconnect cannot
    // make a lost detailed storage event stale.
    if (ble_backend_ != nullptr &&
        ble_backend_->persistent_store_failure_observed()) {
        const bool detailed =
            action != nullptr && action->kind == ActionKind::kBleEvent &&
            action->payload.ble_event.event.kind == BleEventKind::kStorageFailure;
        const auto reported_kind =
            detailed ? action->payload.ble_event.event.store_failure_kind
                     : ble_security::StoreFailureKind::kNone;
        const auto kind =
            reported_kind == ble_security::StoreFailureKind::kRead ||
                    reported_kind == ble_security::StoreFailureKind::kDelete
                ? reported_kind
                : ble_security::StoreFailureKind::kWrite;
        commit_persistent_store_failure(
            kind, detailed ? action->payload.ble_event.event.status : -3);
    }
    const bool lifecycle_handoff_failed =
        reconcile_ble_lifecycle_handoff_failure();
    // The handoff latch is boot-lifetime authoritative once set. Drop every
    // queued BLE callback after its fault commit so an old Reset cannot revive
    // the lifecycle into Enabling after the only Sync/timeout path was lost.
    if (lifecycle_handoff_failed) {
        (void)consume_ble_overflow();
        return true;
    }
    return consume_ble_overflow();
}

void Controller::fail_ble(ble_lifecycle::Generation generation,
                          ble_lifecycle::Operation operation, std::int32_t code,
                          ControlOperation owner) {
    const bool current_generation = generation == ble_state_.generation();
    ble_state_.complete_fault(generation, operation, code);
    if (current_generation) {
        clear_ble_hid_peer();
    }
    retire_ble_route_if_unready();
    release_operation(owner);
}

void Controller::commit_persistent_store_failure(
    ble_security::StoreFailureKind kind, std::int32_t status) {
    if (persistent_store_failure_committed_ || ble_backend_ == nullptr) {
        return;
    }
    persistent_store_failure_committed_ = true;
    ble_backend_->apply_persistent_store_failure(kind, status);
    const auto current = ble_state_.snapshot();
    const auto handle = ble_state_.connection_handle();
    pairing_deadline_us_ = 0;
    wipe_pairing_mailbox();
    pairing_complete_seen_ = false;
    pairing_state_.fail_closed(current.generation, handle,
                               ble_pairing::LastResult::kStorage);
    ble_backend_->cancel_pairing_timeout();
    const auto security = ble_backend_->security_snapshot();
    if (security.coherent && security.connected) {
        ble_backend_->retire_security(security.generation,
                                      security.connection_handle);
    }
    if (current.connected) {
        const std::int32_t disconnect_result =
            ble_backend_->disconnect(handle);
        note_ble_route_disconnect_result(current.generation, handle,
                                         disconnect_result);
    }
    const auto active = active_operation_.load(std::memory_order_acquire);
    const auto owner =
        active == ControlOperation::kBleEnable ||
                active == ControlOperation::kBleDisable
            ? active
            : ControlOperation::kNone;
    fail_ble(current.generation, ble_lifecycle::Operation::kRuntime, -3,
             owner);
}

bool Controller::event_targets_current_ble_authority(BleEvent event) const {
    const auto generation = ble_state_.generation();
    if (event.generation != generation) {
        return false;
    }
    const auto current = ble_state_.snapshot();
    const auto connection = ble_state_.connection_handle();
    if (current.generation != generation ||
        ble_state_.generation() != generation) {
        return false;
    }
    switch (event.kind) {
        case BleEventKind::kConnect:
            return !current.connected &&
                   current.desired ==
                       ble_lifecycle::DesiredExposure::kExposed &&
                   current.observed ==
                       ble_lifecycle::ObservedState::kAdvertising &&
                   event.connection_handle != ble_lifecycle::kNoConnection;
        case BleEventKind::kDisconnect:
        case BleEventKind::kPasskeyAction:
        case BleEventKind::kEncryptionChange:
        case BleEventKind::kPairingComplete:
        case BleEventKind::kIdentityResolved:
        case BleEventKind::kRepeatPairing:
        case BleEventKind::kPairingTimeout:
        case BleEventKind::kStoreFull:
        case BleEventKind::kSubscription:
        case BleEventKind::kControlPoint:
        case BleEventKind::kReportMapRead:
        case BleEventKind::kServiceChangedSubscription:
            return current.connected &&
                   event.connection_handle == connection;
        case BleEventKind::kSync:
        case BleEventKind::kAdvertisingComplete:
        case BleEventKind::kReset:
        case BleEventKind::kTimeout:
        case BleEventKind::kStorageFailure:
            return true;
    }
    return false;
}

bool Controller::event_immediately_loses_ble_hid_readiness(BleEvent event) {
    switch (event.kind) {
        case BleEventKind::kDisconnect:
        case BleEventKind::kReset:
        case BleEventKind::kTimeout:
        case BleEventKind::kPairingTimeout:
        case BleEventKind::kRepeatPairing:
        case BleEventKind::kPasskeyAction:
        case BleEventKind::kEncryptionChange:
        case BleEventKind::kPairingComplete:
        case BleEventKind::kIdentityResolved:
        case BleEventKind::kStoreFull:
        case BleEventKind::kStorageFailure:
            return true;
        case BleEventKind::kSubscription:
            return !event.notify_enabled;
        case BleEventKind::kControlPoint:
            return event.suspended;
        case BleEventKind::kReportMapRead:
        case BleEventKind::kServiceChangedSubscription:
        case BleEventKind::kSync:
        case BleEventKind::kConnect:
        case BleEventKind::kAdvertisingComplete:
            return false;
    }
    return false;
}

void Controller::mark_ble_route_loss(BleEvent event) {
    if (!event_immediately_loses_ble_hid_readiness(event) ||
        !event_targets_current_ble_authority(event)) {
        return;
    }
    const auto authority = event.generation;
    if (authority == 0) {
        ble_route_loss_authority_zero_.store(true, std::memory_order_release);
        return;
    }
    auto observed = ble_route_loss_authority_.load(std::memory_order_acquire);
    while (observed != authority) {
        if (!event_targets_current_ble_authority(event)) {
            return;
        }
        if (ble_route_loss_authority_.compare_exchange_weak(
                observed, authority, std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            return;
        }
    }
}

bool Controller::ble_route_loss_pending(
    ble_lifecycle::Generation generation) const {
    return generation == 0
               ? ble_route_loss_authority_zero_.load(std::memory_order_acquire)
               : ble_route_loss_authority_.load(std::memory_order_acquire) ==
                     generation;
}

void Controller::clear_ble_route_loss(
    ble_lifecycle::Generation generation) {
    if (generation == 0) {
        ble_route_loss_authority_zero_.store(false,
                                             std::memory_order_release);
        return;
    }
    auto expected = generation;
    (void)ble_route_loss_authority_.compare_exchange_strong(
        expected, 0, std::memory_order_acq_rel, std::memory_order_acquire);
}

void Controller::mark_ble_event_overflow(BleEvent event) {
    if (!event_targets_current_ble_authority(event)) {
        return;
    }
    const auto authority = event.generation;
    if (authority == 0) {
        if (event_targets_current_ble_authority(event)) {
            overflow_authority_zero_.store(true, std::memory_order_release);
            request_executor_wake();
        }
        return;
    }
    auto observed = overflow_authority_.load(std::memory_order_acquire);
    while (observed != authority) {
        // Revalidate immediately before the CAS. If the authority changed,
        // this producer must not overwrite a newer authority's pending bit.
        if (!event_targets_current_ble_authority(event)) {
            return;
        }
        if (overflow_authority_.compare_exchange_weak(
                observed, authority, std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            request_executor_wake();
            return;
        }
    }
    // A prior wake may already have been consumed by an executor that has not
    // yet reconciled this still-actionable sticky authority. Re-notify when a
    // producer confirms the same current authority; notifications may safely
    // coalesce because the atomic token remains the source of truth.
    request_executor_wake();
}

bool Controller::ble_event_overflow_pending(
    ble_lifecycle::Generation generation) const {
    return generation == 0
               ? overflow_authority_zero_.load(std::memory_order_acquire)
               : overflow_authority_.load(std::memory_order_acquire) ==
                     generation;
}

void Controller::fail_current_ble_queue_overflow() {
    // A UART Stage-A transition can advance the lifecycle concurrently with
    // this executor boundary. The handoff failure is boot-lifetime truth, so
    // retry against the newly current generation instead of acknowledging a
    // fault that lost that race.
    while (true) {
        const auto current = ble_state_.snapshot();
        if (current.generation != ble_state_.generation()) {
            continue;
        }
        const auto handle = ble_state_.connection_handle();
        pairing_state_.fail_closed(current.generation, handle,
                                   ble_pairing::LastResult::kQueueOverflow);
        pairing_deadline_us_ = 0;
        wipe_pairing_mailbox();
        ble_backend_->cancel_pairing_timeout();
        ble_backend_->mark_security_unhealthy(current.generation);
        if (current.connected) {
            const std::int32_t disconnect_result =
                ble_backend_->disconnect(handle);
            note_ble_route_disconnect_result(current.generation, handle,
                                             disconnect_result);
        }
        const auto active = active_operation_.load(std::memory_order_acquire);
        const auto ble_owner =
            current.observed == ble_lifecycle::ObservedState::kEnabling &&
                    active == ControlOperation::kBleEnable
                ? ControlOperation::kBleEnable
                : current.observed ==
                              ble_lifecycle::ObservedState::kDisabling &&
                          active == ControlOperation::kBleDisable
                      ? ControlOperation::kBleDisable
                      : ControlOperation::kNone;
        fail_ble(current.generation, ble_lifecycle::Operation::kRuntime, -2,
                 ble_owner);
        const auto terminal = ble_state_.snapshot();
        if (terminal.generation == current.generation &&
            terminal.observed == ble_lifecycle::ObservedState::kFault &&
            terminal.recovery_required) {
            return;
        }
    }
}

bool Controller::reconcile_ble_lifecycle_handoff_failure() {
    if (!ble_lifecycle_handoff_failure_.load(std::memory_order_acquire)) {
        return false;
    }
    if (!ble_lifecycle_handoff_failure_committed_) {
        // Persistent-store fatal truth was reconciled first and retains its
        // stronger Storage diagnosis. Otherwise commit QueueOverflow exactly
        // once against whichever lifecycle authority is current now.
        if (!persistent_store_failure_committed_) {
            fail_current_ble_queue_overflow();
        }
        ble_lifecycle_handoff_failure_committed_ = true;
    }
    return true;
}

bool Controller::consume_ble_overflow() {
    if (ble_backend_ == nullptr) {
        return false;
    }
    while (true) {
        const auto authority = ble_state_.generation();
        if (ble_event_overflow_pending(authority)) {
            const auto current = ble_state_.snapshot();
            if (current.generation != authority ||
                ble_state_.generation() != authority) {
                continue;
            }
            const auto clear_authority = [this, authority]() {
                if (authority == 0) {
                    overflow_authority_zero_.store(
                        false, std::memory_order_release);
                    return;
                }
                auto consumed = authority;
                (void)overflow_authority_.compare_exchange_strong(
                    consumed, 0, std::memory_order_acq_rel,
                    std::memory_order_acquire);
            };
            // The boot-global fatal store latch is reconciled first and has
            // already retired this authority more strongly. Do not replace
            // its retained storage diagnosis with the generic queue result.
            if (persistent_store_failure_committed_ ||
                ble_lifecycle_handoff_failure_committed_) {
                clear_authority();
                return true;
            }
            pairing_state_.fail_closed(
                current.generation, ble_state_.connection_handle(),
                ble_pairing::LastResult::kQueueOverflow);
            pairing_deadline_us_ = 0;
            wipe_pairing_mailbox();
            ble_backend_->cancel_pairing_timeout();
            ble_backend_->mark_security_unhealthy(current.generation);
            if (current.connected) {
                const auto handle = ble_state_.connection_handle();
                const std::int32_t disconnect_result =
                    ble_backend_->disconnect(handle);
                note_ble_route_disconnect_result(current.generation, handle,
                                                 disconnect_result);
            }
            const auto active =
                active_operation_.load(std::memory_order_acquire);
            const auto ble_owner =
                current.observed == ble_lifecycle::ObservedState::kEnabling &&
                        active == ControlOperation::kBleEnable
                    ? ControlOperation::kBleEnable
                    : current.observed ==
                                  ble_lifecycle::ObservedState::kDisabling &&
                              active == ControlOperation::kBleDisable
                          ? ControlOperation::kBleDisable
                          : ControlOperation::kNone;
            fail_ble(current.generation, ble_lifecycle::Operation::kRuntime,
                     -2, ble_owner);

            // Clear only the exact authority that was fail-closed. A producer
            // that has already published a newer current authority makes the
            // CAS fail, leaving that uncertainty pending for the next loop.
            clear_authority();
            return true;
        }
        if (authority == 0) {
            auto stale = overflow_authority_.load(std::memory_order_acquire);
            if (stale == 0) {
                return false;
            }
#ifdef HID_CONTROL_EXECUTOR_NATIVE_TEST
            if (overflow_consume_hook_ != nullptr) {
                const auto hook = overflow_consume_hook_;
                overflow_consume_hook_ = nullptr;
                hook(*this);
            }
#endif
            (void)overflow_authority_.compare_exchange_strong(
                stale, 0, std::memory_order_acq_rel,
                std::memory_order_acquire);
            continue;
        }
        auto stale_zero = true;
        if (overflow_authority_zero_.compare_exchange_strong(
                stale_zero, false, std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            continue;
        }
        auto stale = overflow_authority_.load(std::memory_order_acquire);
        if (stale == 0) {
            return false;
        }
#ifdef HID_CONTROL_EXECUTOR_NATIVE_TEST
        if (overflow_consume_hook_ != nullptr) {
            const auto hook = overflow_consume_hook_;
            overflow_consume_hook_ = nullptr;
            hook(*this);
        }
#endif
        (void)overflow_authority_.compare_exchange_strong(
            stale, 0, std::memory_order_acq_rel,
            std::memory_order_acquire);
    }
}

void Controller::terminate_security_connection(ble_pairing::LastResult result,
                                               bool fatal) {
    const auto current = ble_state_.snapshot();
    if (!current.connected || ble_backend_ == nullptr) {
        return;
    }
    // One failed SMP transaction can publish StoreFull, Pairing Complete, and
    // Encryption Change terminal events before the first Disconnect callback
    // is consumed. The first connection-local result owns the already-bounded
    // teardown; later events must neither overwrite it nor try to arm a second
    // Disconnect watchdog for the same authority. Persistent Storage failure
    // remains a separate boot-global fail-closed path.
    if (pairing_terminal_committed_) {
        return;
    }
    pairing_terminal_committed_ = true;
    const auto handle = ble_state_.connection_handle();
    pairing_deadline_us_ = 0;
    wipe_pairing_mailbox();
    (void)pairing_state_.complete(current.generation, handle, result);
    ble_backend_->cancel_pairing_timeout();
    ble_backend_->retire_security(current.generation, handle);
    const std::int32_t disconnect_result = ble_backend_->disconnect(handle);
    const bool already_disconnected =
        !fatal && ble_backend_->security_teardown_already_disconnected(
                      disconnect_result);
    note_ble_route_disconnect_result(current.generation, handle,
                                     already_disconnected ? 0
                                                          : disconnect_result);
    if (fatal) {
        ble_backend_->mark_security_unhealthy(current.generation);
        fail_ble(current.generation, ble_lifecycle::Operation::kRuntime, -3,
                 ControlOperation::kNone);
    } else if (already_disconnected) {
        (void)reconcile_security_disconnect_absent(current.generation, handle);
    } else if (disconnect_result != 0) {
        // The peer authority was revoked above, but without a successfully
        // established Disconnect operation there is no completion or watchdog
        // to advance the lifecycle. Terminalize that uncertainty now.
        ble_backend_->mark_security_unhealthy(current.generation);
        fail_ble(current.generation, ble_lifecycle::Operation::kRuntime,
                 disconnect_result, ControlOperation::kNone);
    }
}

bool Controller::reconcile_security_disconnect_absent(
    ble_lifecycle::Generation generation,
    std::uint16_t connection_handle) {
    const auto current = ble_state_.snapshot();
    if (current.generation != generation ||
        generation != ble_state_.generation() || !current.connected ||
        current.desired != ble_lifecycle::DesiredExposure::kExposed ||
        current.observed != ble_lifecycle::ObservedState::kConnected ||
        connection_handle != ble_state_.connection_handle()) {
        return false;
    }
    return reconcile_ble_disconnect(
        {.kind = BleEventKind::kDisconnect,
         .generation = generation,
         .connection_handle = connection_handle},
        false);
}

void Controller::reconcile_security(std::uint16_t connection_handle,
                                    bool terminal_evidence_ready) {
    const auto current = ble_state_.snapshot();
    if (!current.connected || current.generation != ble_state_.generation() ||
        connection_handle != ble_state_.connection_handle()) {
        return;
    }
    if (pairing_terminal_committed_) {
        return;
    }
    ble_backend_->refresh_security(connection_handle);
    if (ble_backend_->security_ready_for_hid(current.generation,
                                             connection_handle)) {
        pairing_state_.complete(current.generation, connection_handle,
                                ble_pairing::LastResult::kSucceeded);
        pairing_deadline_us_ = 0;
        wipe_pairing_mailbox();
        ble_backend_->cancel_pairing_timeout();
        reconcile_gatt_cache();
        return;
    }
    const auto security = ble_backend_->security_snapshot();
    if (terminal_evidence_ready && pairing_complete_seen_ &&
        security.coherent && security.connected && security.encrypted &&
        security.identity_resolved && security.store_healthy) {
        const auto &policy = kStrictProfile.security;
        if (security.authenticated != policy.authenticated ||
            security.key_size != policy.key_size ||
            (policy.secure_connections_required &&
             !security.secure_connections)) {
            terminate_security_connection(
                ble_pairing::LastResult::kSecurityPolicy, false);
        } else if (!security.project_verified_bond_persisted) {
            terminate_security_connection(ble_pairing::LastResult::kStorage,
                                          true);
        }
    }
}

void Controller::reconcile_gatt_cache() {
    using Kind = GattSchemaStoreResultKind;
    if (ble_backend_ == nullptr || !ble_hid_peer_.active ||
        ble_hid_peer_.generation != ble_state_.generation() ||
        ble_hid_peer_.connection_handle != ble_state_.connection_handle() ||
        !ble_backend_->security_ready_for_hid(
            ble_hid_peer_.generation, ble_hid_peer_.connection_handle) ||
        ble_backend_->gatt_schema_current_for_hid(
            ble_hid_peer_.generation, ble_hid_peer_.connection_handle)) {
        return;
    }
    const auto fail_store = [this](GattSchemaStoreResult result) {
        if (result.kind == Kind::kCapacityFull) {
            ble_backend_->apply_store_failure(
                ble_hid_peer_.generation, ble_hid_peer_.connection_handle,
                ble_security::StoreFailureKind::kCapacityFull, result.status);
            terminate_security_connection(ble_pairing::LastResult::kStoreFull,
                                          false);
        } else if (result.kind == Kind::kStorageFailure) {
            commit_persistent_store_failure(
                ble_security::StoreFailureKind::kWrite, result.status);
        }
    };
    if (!ble_hid_peer_.schema_checked) {
        const auto result = ble_backend_->gatt_schema_status(
            ble_hid_peer_.generation, ble_hid_peer_.connection_handle);
        if (result.kind == Kind::kCurrent) {
            ble_hid_peer_.schema_checked = true;
            return;
        }
        if (result.kind != Kind::kStale) {
            fail_store(result);
            return;
        }
        ble_hid_peer_.schema_checked = true;
    }
    if (ble_hid_peer_.report_map_read) {
        const auto result = ble_backend_->persist_gatt_schema_current(
            ble_hid_peer_.generation, ble_hid_peer_.connection_handle);
        if (result.kind != Kind::kCurrent) {
            fail_store(result);
        }
        return;
    }
    if (ble_hid_peer_.service_changed_indicate_enabled &&
        !ble_hid_peer_.refresh_requested) {
        ble_hid_peer_.refresh_requested = true;
        (void)ble_backend_->request_gatt_cache_refresh(
            ble_hid_peer_.generation, ble_hid_peer_.connection_handle,
            kGattChangedStartHandle, kGattChangedEndHandle);
    }
}

void Controller::reconcile_pairing_deadline() {
    if (ble_backend_ == nullptr || pairing_deadline_us_ == 0) {
        return;
    }
    const auto pending = pairing_state_.snapshot();
    if (pending.live_state != ble_pairing::LiveState::kWaitingInput ||
        !pending.pairing_active) {
        pairing_deadline_us_ = 0;
        wipe_pairing_mailbox();
        return;
    }
    if (ble_backend_->monotonic_time_us() < pairing_deadline_us_) {
        return;
    }
    if (pairing_state_.timeout(pending.generation, pending.connection_handle,
                               pending.pairing_id)) {
        pairing_deadline_us_ = 0;
        wipe_pairing_mailbox();
        terminate_security_connection(ble_pairing::LastResult::kTimeout, false);
    }
}

PairingStatusSnapshot Controller::current_pairing_status() const {
    PairingStatusSnapshot result{};
    result.available = true;
    result.ble_route_ready = ble_route_ready();
    result.pairing = pairing_state_.snapshot();
    const auto lifecycle = ble_state_.snapshot();
    result.generation = lifecycle.generation;
    result.connected = lifecycle.connected;
    if (ble_backend_ != nullptr) {
        const auto security = ble_backend_->security_snapshot();
        if (security.coherent && security.generation == lifecycle.generation &&
            security.connected == lifecycle.connected) {
            result.security = security;
        }
        if (result.pairing.live_state == ble_pairing::LiveState::kWaitingInput &&
            result.pairing.pairing_active && pairing_deadline_us_ != 0) {
            const std::uint64_t now = ble_backend_->monotonic_time_us();
            if (now < pairing_deadline_us_) {
                const std::uint64_t remaining = pairing_deadline_us_ - now;
                result.remaining_ms = static_cast<std::uint32_t>(
                    (remaining + 999U) / 1000U);
            }
        }
    }
    return result;
}

void Controller::wipe_pairing_mailbox() {
    secure_memory::zero(&pairing_mailbox_, sizeof(pairing_mailbox_));
}

void Controller::complete_pairing_rpc(std::uint32_t token) {
    std::uint32_t expected = token;
    if (!pairing_rpc_pending_.compare_exchange_strong(
            expected, 0, std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        return;
    }
#ifndef HID_CONTROL_EXECUTOR_NATIVE_TEST
    (void)xSemaphoreGive(s_pairing_rpc_completion);
#endif
}

bool Controller::bond_remove_eligible() const {
    if (runtime_ == nullptr || ble_backend_ == nullptr) {
        return false;
    }
    const auto lifecycle = ble_state_.snapshot();
    const auto route = runtime_->state_machine().route_snapshot();
    const auto pairing = pairing_state_.snapshot();
    return lifecycle.stack_ready &&
           lifecycle.desired == ble_lifecycle::DesiredExposure::kHidden &&
           lifecycle.observed == ble_lifecycle::ObservedState::kIdle &&
           !lifecycle.advertising && !lifecycle.connected &&
           !lifecycle.recovery_required && route.coherent &&
           !route.invalidation_pending &&
           route.desired != hid_route::OutputRoute::kBle &&
           route.active != hid_route::OutputRoute::kBle &&
           route.transition == hid_route::Transition::kStable &&
           pairing.coherent &&
           pairing.live_state == ble_pairing::LiveState::kIdle &&
           !pairing.pairing_active;
}

ble_pairing::RespondResult Controller::respond_to_pairing(
    ble_lifecycle::Generation generation, std::uint16_t connection_handle,
    std::uint32_t pairing_id,
    const std::array<char, 6> &six_digit_secret) {
    if (!initialized_ || ble_backend_ == nullptr) {
        return ble_pairing::RespondResult::kNotPending;
    }
    auto result = pairing_state_.validate_response(
        generation, connection_handle, pairing_id);
    if (result != ble_pairing::RespondResult::kAccepted) {
        return result;
    }
    std::uint32_t value = 0;
    for (const char digit : six_digit_secret) {
        if (digit < '0' || digit > '9') {
            return ble_pairing::RespondResult::kInvalidSecret;
        }
        value = value * 10U + static_cast<std::uint32_t>(digit - '0');
    }
    const std::int32_t injection_result =
        ble_backend_->inject_passkey(connection_handle, value);
    secure_memory::zero(&value, sizeof(value));
    if (injection_result != 0) {
        terminate_security_connection(ble_pairing::LastResult::kSmpFailed,
                                      false);
        return ble_pairing::RespondResult::kInjectionFailed;
    }
    pairing_state_.consume_response(generation, connection_handle, pairing_id);
    pairing_deadline_us_ = 0;
    ble_backend_->cancel_pairing_timeout();
    return ble_pairing::RespondResult::kAccepted;
}

bool Controller::reconcile_ble_disconnect(BleEvent event, bool expected) {
    if (!ble_state_.observe_disconnect(event.generation,
                                       event.connection_handle, expected)) {
        return false;
    }
    // The same exact physical-loss proof completes any already-releasing BLE
    // route. This call is idempotent when the queued Disconnect path already
    // performed the route transition before entering process_ble_event().
    complete_ble_route_release_on_disconnect(event);
    ble_backend_->cancel_pairing_timeout();
    pairing_deadline_us_ = 0;
    wipe_pairing_mailbox();
    pairing_complete_seen_ = false;
    pairing_terminal_committed_ = false;
    (void)pairing_state_.disconnect(event.generation,
                                    event.connection_handle);
    ble_backend_->retire_security(event.generation,
                                  event.connection_handle);
    clear_ble_hid_peer();
    if (expected) {
        ble_state_.complete_disable(event.generation);
        ble_backend_->record_heap_checkpoint(
            BleBackend::HeapCheckpoint::kHiddenIdle);
        release_operation(ControlOperation::kBleDisable);
        return true;
    }
    if (ble_backend_->persistent_store_failure_observed()) {
        return true;
    }
    const auto generation = ble_state_.generation();
    ble_backend_->set_generation(generation);
    const std::int32_t result = ble_backend_->start_advertising();
    if (result == 0) {
        ble_state_.complete_advertising(generation);
        ble_backend_->record_heap_checkpoint(
            BleBackend::HeapCheckpoint::kReadvertising);
    } else {
        fail_ble(generation, ble_lifecycle::Operation::kRuntime, result,
                 ControlOperation::kNone);
    }
    return true;
}

void Controller::process_ble_event(BleEvent event) {
    if (ble_backend_ == nullptr) {
        return;
    }
    switch (event.kind) {
        case BleEventKind::kSync: {
            if (!ble_state_.complete_sync(event.generation)) {
                return;
            }
            if (ble_backend_->persistent_store_failure_observed()) {
                return;
            }
            const std::int32_t result = ble_backend_->start_advertising();
            if (result == 0) {
                ble_state_.complete_advertising(event.generation);
                ble_backend_->record_heap_checkpoint(
                    BleBackend::HeapCheckpoint::kAdvertising);
                const ControlOperation owner =
                    active_operation_.load(std::memory_order_acquire);
                if (owner == ControlOperation::kBleEnable) {
                    release_operation(owner);
                }
            } else {
                fail_ble(event.generation, ble_lifecycle::Operation::kEnable,
                         result, ControlOperation::kBleEnable);
            }
            return;
        }
        case BleEventKind::kConnect:
            if (ble_state_.observe_connect(event.generation,
                                           event.connection_handle)) {
                begin_ble_hid_peer(event.generation, event.connection_handle);
                pairing_complete_seen_ = false;
                pairing_terminal_committed_ = false;
                pairing_deadline_us_ = 0;
                wipe_pairing_mailbox();
                pairing_state_.begin_connection(event.generation,
                                                event.connection_handle);
                ble_backend_->begin_security(event.generation,
                                             event.connection_handle);
                (void)ble_backend_->configure_connection(event.connection_handle);
                ble_backend_->record_heap_checkpoint(
                    BleBackend::HeapCheckpoint::kConnected);
                const std::int32_t result =
                    ble_backend_->initiate_security(event.connection_handle);
                if (claim_dle(event)) {
                    // Admission is consumed even on error. No product retry or
                    // teardown solely for DLE; SDK HCI fault handling remains.
                    (void)ble_backend_->set_connection_data_length(
                        event.connection_handle);
                }
                if (result != 0) {
                    terminate_security_connection(
                        ble_pairing::LastResult::kSmpFailed, false);
                }
            }
            return;
        case BleEventKind::kDisconnect: {
            const bool expected = active_operation_.load(std::memory_order_acquire) ==
                                  ControlOperation::kBleDisable;
            (void)reconcile_ble_disconnect(event, expected);
            return;
        }
        case BleEventKind::kAdvertisingComplete:
            // A completion for an obsolete generation is ignored. An active
            // exposed incarnation is restarted by the serialized owner.
            if (event.generation == ble_state_.generation() &&
                ble_state_.snapshot().desired ==
                    ble_lifecycle::DesiredExposure::kExposed &&
                !ble_state_.snapshot().connected) {
                if (ble_backend_->persistent_store_failure_observed()) {
                    return;
                }
                const std::int32_t result = ble_backend_->start_advertising();
                if (result != 0) {
                    fail_ble(event.generation, ble_lifecycle::Operation::kRuntime,
                             result, ControlOperation::kNone);
                }
            }
            return;
        case BleEventKind::kReset: {
            const auto before_reset = ble_state_.snapshot();
            clear_ble_hid_peer();
            pairing_state_.reset();
            pairing_deadline_us_ = 0;
            wipe_pairing_mailbox();
            ble_backend_->cancel_pairing_timeout();
            ble_backend_->mark_security_unhealthy(event.generation);
            if (before_reset.connected) {
                ble_backend_->retire_security(event.generation,
                                              ble_state_.connection_handle());
            }
            pairing_complete_seen_ = false;
            pairing_terminal_committed_ = false;
            if (!ble_state_.begin_reset_recovery(event.generation, event.status)) {
                release_operation(active_operation_.load(std::memory_order_acquire));
                return;
            }
            const auto generation = ble_state_.generation();
            ble_backend_->set_generation(generation);
            return;
        }
        case BleEventKind::kTimeout:
            fail_ble(event.generation, ble_lifecycle::Operation::kRuntime,
                     event.status, active_operation_.load(std::memory_order_acquire));
            return;
        case BleEventKind::kPasskeyAction: {
            if (event.status == 1) {
                const auto decision = pairing_state_.begin_passkey_input(
                    event.generation, event.connection_handle);
                if (decision == ble_pairing::PasskeyActionResult::kStarted) {
                    pairing_deadline_us_ = ble_backend_->monotonic_time_us() +
                        static_cast<std::uint64_t>(ble_pairing::kInputTimeoutMs) * 1000U;
                    ble_backend_->arm_pairing_timeout(
                        event.generation, event.connection_handle,
                        pairing_state_.snapshot().pairing_id);
                } else if (decision ==
                           ble_pairing::PasskeyActionResult::kIdExhausted) {
                    terminate_security_connection(
                        ble_pairing::LastResult::kSecurityPolicy, true);
                }
            } else {
                const auto decision = pairing_state_.reject_unsupported_action(
                    event.generation, event.connection_handle);
                if (decision ==
                    ble_pairing::PasskeyActionResult::kUnsupported) {
                    terminate_security_connection(
                        ble_pairing::LastResult::kSecurityPolicy, false);
                }
            }
            return;
        }
        case BleEventKind::kEncryptionChange:
            if (event.generation != ble_state_.generation() ||
                event.connection_handle != ble_state_.connection_handle()) {
                return;
            }
            if (event.status != 0) {
                terminate_security_connection(
                    ble_pairing::LastResult::kSmpFailed, false);
                return;
            }
            // ESP-NimBLE v5.5.4 publishes Encryption Change only after its
            // successful bonding path has completed the synchronous OUR_SEC
            // and PEER_SEC persistence attempts. This is the first event at
            // which an absent record is terminal evidence rather than a
            // transient store ordering gap.
            reconcile_security(event.connection_handle, true);
            return;
        case BleEventKind::kPairingComplete:
            if (event.generation != ble_state_.generation() ||
                event.connection_handle != ble_state_.connection_handle()) {
                return;
            }
            if (event.status != 0) {
                terminate_security_connection(
                    ble_pairing::LastResult::kSmpFailed, false);
                return;
            }
            // ESP-NimBLE v5.5.4 publishes Pairing Complete before persisting
            // the new bond and can publish Identity Resolved from inside that
            // persistence path before either security record is written.
            // Retain completion here, but wait for the subsequent Encryption
            // Change before treating an absent persisted record as fatal.
            pairing_complete_seen_ = true;
            return;
        case BleEventKind::kIdentityResolved:
            if (event.generation == ble_state_.generation() &&
                event.connection_handle == ble_state_.connection_handle()) {
                ble_backend_->refresh_security(event.connection_handle, true);
                reconcile_security(event.connection_handle, false);
            }
            return;
        case BleEventKind::kRepeatPairing:
            if (event.generation == ble_state_.generation() &&
                event.connection_handle == ble_state_.connection_handle()) {
                terminate_security_connection(
                    ble_pairing::LastResult::kRepeatPairing, false);
            }
            return;
        case BleEventKind::kPairingTimeout:
            if (pairing_state_.timeout(event.generation,
                                       event.connection_handle,
                                       event.pairing_id)) {
                pairing_deadline_us_ = 0;
                wipe_pairing_mailbox();
                terminate_security_connection(
                    ble_pairing::LastResult::kTimeout, false);
            }
            return;
        case BleEventKind::kStoreFull:
            if (event.generation == ble_state_.generation() &&
                event.connection_handle == ble_state_.connection_handle()) {
                ble_backend_->apply_store_failure(
                    event.generation, event.connection_handle,
                    ble_security::StoreFailureKind::kCapacityFull, event.status);
                terminate_security_connection(
                    ble_pairing::LastResult::kStoreFull, false);
            }
            return;
        case BleEventKind::kStorageFailure: {
            // Persistent storage is a subsystem-global authority. Its fault
            // commit must survive retirement of the connection that exposed it.
            const auto kind =
                event.store_failure_kind == ble_security::StoreFailureKind::kNone ||
                        event.store_failure_kind ==
                            ble_security::StoreFailureKind::kCapacityFull
                    ? ble_security::StoreFailureKind::kWrite
                    : event.store_failure_kind;
            commit_persistent_store_failure(kind, event.status);
            return;
        }
        case BleEventKind::kSubscription: {
            if (!ble_hid_peer_.active ||
                event.generation != ble_hid_peer_.generation ||
                event.connection_handle != ble_hid_peer_.connection_handle ||
                event.subscription_reason == BleSubscriptionReason::kUnknown) {
                return;
            }
            if (event.hid_interface == BleHidInterface::kKeyboard &&
                event.attribute_handle == ble_hid_peer_.handles.keyboard_value) {
                ble_hid_peer_.keyboard_notify_enabled = event.notify_enabled;
            } else if (event.hid_interface == BleHidInterface::kMouse &&
                       event.attribute_handle ==
                           ble_hid_peer_.handles.mouse_value) {
                ble_hid_peer_.mouse_notify_enabled = event.notify_enabled;
            }
            return;
        }
        case BleEventKind::kControlPoint:
            if (ble_hid_peer_.active &&
                event.generation == ble_hid_peer_.generation &&
                event.connection_handle == ble_hid_peer_.connection_handle &&
                event.attribute_handle ==
                    ble_hid_peer_.handles.control_point_value) {
                ble_hid_peer_.suspended = event.suspended;
            }
            return;
        case BleEventKind::kReportMapRead:
            if (ble_hid_peer_.active &&
                event.generation == ble_hid_peer_.generation &&
                event.connection_handle == ble_hid_peer_.connection_handle &&
                event.attribute_handle ==
                    ble_hid_peer_.handles.report_map_value) {
                ble_hid_peer_.report_map_read = true;
                reconcile_gatt_cache();
            }
            return;
        case BleEventKind::kServiceChangedSubscription:
            if (ble_hid_peer_.active &&
                event.generation == ble_hid_peer_.generation &&
                event.connection_handle == ble_hid_peer_.connection_handle &&
                event.attribute_handle ==
                    ble_backend_->service_changed_value_handle() &&
                event.subscription_reason !=
                    BleSubscriptionReason::kUnknown) {
                ble_hid_peer_.service_changed_indicate_enabled =
                    event.indicate_enabled;
                reconcile_gatt_cache();
            }
            return;
    }
}

#ifdef HID_CONTROL_EXECUTOR_NATIVE_TEST
bool Controller::process_one_for_test() {
    Action action{};
    if (!dequeue_one_for_test(action)) {
        return false;
    }
    process(action);
    return true;
}

bool Controller::process_wake_cycle_for_test() {
    if (!native_executor_wake_pending_.exchange(false,
                                                std::memory_order_acq_rel)) {
        return false;
    }
    (void)reconcile_usb_runtime_fault();
    Action action{};
    while (dequeue_one_for_test(action)) {
        process(action);
    }
    (void)reconcile_usb_runtime_fault();
    reconcile_usb_lifecycle_logs();
    (void)reconcile_ble_fallbacks(nullptr);
    drive_ble_explicit_release();
    retire_ble_route_if_unready();
    return true;
}

bool Controller::executor_wake_pending_for_test() const {
    return native_executor_wake_pending_.load(std::memory_order_acquire);
}

bool Controller::dequeue_one_for_test(Action &action) {
    if (native_count_ == 0) {
        return false;
    }
    action = native_queue_[native_head_];
    native_head_ = static_cast<std::uint8_t>(
        (native_head_ + 1U) % kActionQueueDepth);
    --native_count_;
    return true;
}

void Controller::process_for_test(Action action) { process(action); }

void Controller::set_overflow_consume_hook_for_test(
    OverflowConsumeHook hook) {
    overflow_consume_hook_ = hook;
}

void Controller::set_ble_enqueue_failure_hook_for_test(
    BleEnqueueFailurePhase phase, BleEnqueueFailureHook hook) {
    ble_enqueue_failure_phase_ = phase;
    ble_enqueue_failure_hook_ = hook;
}

void Controller::set_process_after_reconciliation_hook_for_test(
    ProcessAfterReconciliationHook hook) {
    process_after_reconciliation_hook_ = hook;
}

void Controller::set_ble_grace_signal_hook_for_test(
    BleGraceSignalPhase phase, BleGraceSignalHook hook) {
    ble_grace_signal_phase_ = phase;
    ble_grace_signal_hook_ = hook;
}

void Controller::set_ble_generation_for_test(
    ble_lifecycle::Generation generation) {
    ble_state_.set_generation_for_test(generation);
}

ControlOperation Controller::active_operation_for_test() const {
    return active_operation_.load(std::memory_order_acquire);
}

bool Controller::reserve_operation_for_test(ControlOperation operation) {
    return operation != ControlOperation::kNone && claim_operation(operation);
}

void Controller::release_operation_for_test(ControlOperation operation) {
    release_operation(operation);
}

void Controller::fail_next_enqueue_for_test() { fail_next_enqueue_ = true; }

void Controller::set_next_pairing_id_for_test(std::uint32_t value) {
    pairing_state_.set_next_pairing_id_for_test(value);
}

bool Controller::pairing_mailbox_zero_for_test() const {
    const auto *bytes =
        reinterpret_cast<const unsigned char *>(&pairing_mailbox_);
    for (std::size_t index = 0; index < sizeof(pairing_mailbox_); ++index) {
        if (bytes[index] != 0) {
            return false;
        }
    }
    return true;
}

bool Controller::expire_ble_route_release_grace_for_test() {
    return signal_ble_route_release_grace(ble_route_release_);
}

BleRouteReleaseIdentity
Controller::ble_route_release_identity_for_test() const {
    return ble_route_release_;
}

std::size_t Controller::ble_grace_available_slots_for_test() const {
    return ble_route_grace_authority_.available_slots_for_test();
}

void Controller::set_ble_grace_next_incarnation_for_test(
    std::uint64_t value) {
    ble_route_grace_authority_.set_next_incarnation_for_test(value);
}

bool Controller::reconcile_security_disconnect_absent_for_test(
    ble_lifecycle::Generation generation,
    std::uint16_t connection_handle) {
    return reconcile_security_disconnect_absent(generation,
                                                connection_handle);
}

void Controller::set_usb_runtime_fault_occurrences_for_test(
    std::uint32_t value) {
    usb_runtime_fault_occurrences_.store(value, std::memory_order_release);
}

bool Controller::service_usb_sof_watchdog_for_test(std::uint32_t now_ms) {
    return reconcile_usb_sof_watchdog(now_ms);
}

bool Controller::usb_sof_watchdog_armed_for_test() const {
    return usb_sof_watchdog_.armed;
}

void Controller::set_sof_watchdog_before_fence_hook_for_test(
    SofWatchdogBeforeFenceHook hook) {
    sof_watchdog_before_fence_hook_ = hook;
}
#else
void Controller::task_entry(void *context) {
    static_cast<Controller *>(context)->task_loop();
}

void Controller::task_loop() {
    while (true) {
        // A direct task notification is the shared capacity-independent wake
        // for both normal queue work and sticky BLE fallbacks. pdTRUE coalesces
        // any accumulated notifications; the authoritative details remain in
        // the queue and fallback atomics. A notification given before this
        // wait remains pending, closing the check-then-sleep race.
        const TickType_t wait_ticks =
            usb_sof_watchdog_.armed
                ? pdMS_TO_TICKS(kUsbSofWatchdogSampleMs)
                : portMAX_DELAY;
        (void)ulTaskNotifyTake(pdTRUE, wait_ticks);
        (void)reconcile_usb_runtime_fault();
        Action action{};
        while (xQueueReceive(s_action_queue, &action, 0) == pdPASS) {
            process(action);
        }
        // Catch publication while the last action was active. If publication
        // instead races after this check, its retained notification makes the
        // next wait return immediately.
        (void)reconcile_usb_runtime_fault();
        const std::uint32_t now_ms = static_cast<std::uint32_t>(
            static_cast<std::uint64_t>(xTaskGetTickCount()) *
            portTICK_PERIOD_MS);
        (void)reconcile_usb_sof_watchdog(now_ms);
        reconcile_usb_lifecycle_logs();
        (void)reconcile_ble_fallbacks(nullptr);
        drive_ble_explicit_release();
        retire_ble_route_if_unready();
    }
}
#endif

}  // namespace hid_control_executor
