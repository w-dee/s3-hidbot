#!/usr/bin/env python3
"""Exercise production HCI-ingress establishment authority without hardware."""
from pathlib import Path
import subprocess
import tempfile

from test_bond_delete_nvs import ROOT, body

SOURCE = (ROOT / "firmware/components/ble_transport/ble_transport.cpp").read_text()

BEGIN_STOP = body(SOURCE, "std::uint64_t Backend::begin_stop")
assert BEGIN_STOP.index("hci_ingress_enabled_.store(false") < BEGIN_STOP.index(
    "close_hci_event_users()"
) < BEGIN_STOP.index("xTaskCreate(")
STOP_DEINIT = SOURCE[SOURCE.index("struct Backend::StopOperations"):
                     SOURCE.index("void Backend::stop_task")]
assert "if (!backend.deinitialize_hci_event()) return false;" in STOP_DEINIT
DISCONNECT_INGRESS = SOURCE[
    SOURCE.index("if (event[0] == BLE_HCI_EVCODE_DISCONN_CMP"):
    SOURCE.index("if (event[0] != BLE_HCI_EVCODE_LE_META")
]
assert "resolve_hci_establishment" not in DISCONNECT_INGRESS
RESET = body(SOURCE, "void Backend::on_reset")
assert "kHciEventUsersClosing" in RESET
assert RESET.index("retire_hci_establishment()") < RESET.index(
    "hci_ingress_enabled_.store(true"
)

PRE = r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <endian.h>
#include <functional>
#include <iostream>
#include <thread>
#include <utility>
#define BLE_HCI_EVCODE_DISCONN_CMP 0x05
#define BLE_HCI_EVCODE_LE_META 0x3e
#define BLE_HCI_LE_SUBEV_CONN_COMPLETE 0x01
#define BLE_HCI_LE_SUBEV_ENH_CONN_COMPLETE 0x0a
#define BLE_HCI_LE_SUBEV_ENH_CONN_COMPLETE_V2 0x29
#define BLE_HCI_LE_CONN_COMPLETE_ROLE_SLAVE 1
#define BLE_HCI_OGF_LINK_CTRL 1
#define BLE_HCI_OCF_DISCONNECT_CMD 6
#define BLE_HCI_OP(ogf, ocf) ((ocf) | ((ogf) << 10))
#define BLE_HS_HCI_ERR(x) ((x) == 0 ? 0 : 0x200 + (x))
#define BLE_ERR_SUCCESS 0
#define BLE_ERR_UNK_CONN_ID 2
#define BLE_ERR_REM_USER_CONN_TERM 19
#define MYNEWT_VAL(x) 0
using esp_err_t = int;
constexpr int ESP_ERR_INVALID_STATE = 0x103;
constexpr int ESP_ERR_INVALID_ARG = 0x102;
struct esp_vhci_host_callback_t {
 void (*notify_host_send_available)(void);
 int (*notify_host_recv)(uint8_t *, uint16_t);
};
const esp_vhci_host_callback_t *registered_vhci = nullptr;
int real_registration_result = 0;
int original_recv = 0;
int replacement_recv = 0;
int original_send = 0;
int queue_puts = 0;
int queue_checks = 0;
bool event_alive = true;
std::function<void()> queue_put_hook;
std::function<void()> command_hook;
std::function<void()> delay_hook;
int64_t fake_time_us = 0;
int64_t esp_timer_get_time() { return fake_time_us += 100'000; }
void vTaskDelay(int) { if (delay_hook) delay_hook(); }
extern "C" esp_err_t __real_esp_vhci_host_register_callback(
    const esp_vhci_host_callback_t *callback) {
 if (real_registration_result == 0) registered_vhci = callback;
 return real_registration_result;
}
void original_send_available() { ++original_send; }
int original_receive(uint8_t *, uint16_t) { ++original_recv; return 23; }
int replacement_receive(uint8_t *, uint16_t) { ++replacement_recv; return 29; }
struct ble_hci_lc_disconnect_cp {
 uint16_t conn_handle;
 uint8_t reason;
} __attribute__((packed));
struct ble_hci_ev_le_subev_conn_complete {
 uint8_t subevent_code;
 uint8_t status;
 uint16_t connection_handle;
 uint8_t role;
 uint8_t peer_address_type;
 uint8_t peer_address[6];
 uint16_t connection_interval;
 uint16_t connection_latency;
 uint16_t supervision_timeout;
 uint8_t master_clock_accuracy;
} __attribute__((packed));
struct ble_hci_ev_le_subev_enh_conn_complete {
 uint8_t subevent_code;
 uint8_t status;
 uint16_t connection_handle;
 uint8_t role;
 uint8_t peer_address_type;
 uint8_t peer_address[6];
 uint8_t local_resolvable_private_address[6];
 uint8_t peer_resolvable_private_address[6];
 uint16_t connection_interval;
 uint16_t connection_latency;
 uint16_t supervision_timeout;
 uint8_t master_clock_accuracy;
} __attribute__((packed));
static_assert(sizeof(ble_hci_ev_le_subev_conn_complete) == 19);
static_assert(sizeof(ble_hci_ev_le_subev_enh_conn_complete) == 31);
struct ble_npl_event {
 void (*fn)(ble_npl_event *) = nullptr;
 void *arg = nullptr;
 bool queued = false;
};
struct ble_npl_eventq {} queue;
void *ble_npl_event_get_arg(ble_npl_event *event) { return event->arg; }
bool ble_npl_event_is_queued(ble_npl_event *event) {
 ++queue_checks;
 assert(event_alive);
 return event->queued;
}
ble_npl_eventq *nimble_port_get_dflt_eventq() { return &queue; }
void ble_npl_eventq_put(ble_npl_eventq *, ble_npl_event *event) {
 if (queue_put_hook) queue_put_hook();
 assert(event_alive);
 assert(!event->queued);
 ++queue_puts;
 event->queued = true;
}
void ble_npl_event_deinit(ble_npl_event *) {
 assert(event_alive);
 event_alive = false;
}
void run(ble_npl_event &event) {
 assert(event.queued);
 event.queued = false;
 event.fn(&event);
}
bool enabled = true;
bool advertising = false;
bool locked = false;
uint16_t peer = 0xffff;
int raw_disconnects = 0;
int normal_terminations = 0;
int last_raw_handle = -1;
int raw_result = 0;
bool ble_hs_is_enabled() { return enabled; }
int ble_gap_adv_active() { assert(locked); return advertising; }
void ble_hs_lock() { assert(!locked); locked = true; }
void ble_hs_unlock() { assert(locked); locked = false; }
using foreach_fn = int(uint16_t, void *);
void ble_gap_conn_foreach_handle(foreach_fn *fn, void *arg) {
 assert(locked);
 if (peer != 0xffff) fn(peer, arg);
}
int ble_hs_hci_cmd_tx(uint16_t, const void *value, uint8_t, void *, uint8_t) {
 auto *command = static_cast<const ble_hci_lc_disconnect_cp *>(value);
 ++raw_disconnects;
 last_raw_handle = le16toh(command->conn_handle);
 if (command_hook) command_hook();
 return raw_result;
}
namespace ble_lifecycle {
using Generation = uint64_t;
constexpr uint16_t kNoConnection = 0xffff;
}
namespace hid_control_executor {
struct BleEventSink {
 int faults = 0;
 void signal_ble_lifecycle_handoff_failure() { ++faults; }
};
}
namespace ble_transport {
constexpr uint32_t kHciEventUsersClosing = UINT32_C(1) << 31;
constexpr uint32_t kHciEventUsersMask = kHciEventUsersClosing - 1U;
constexpr int64_t kHciEventUsersDrainTimeoutUs = 1'000'000;
bool allocate_hci_establishment_id(std::atomic<uint64_t> *, uint64_t *);
int disconnect_controller_handle(uint16_t);
class Backend {
public:
 static Backend *instance_;
 static bool hci_callback_registration_allowed();
 static bool observe_hci_ingress(const uint8_t *);
 static void queue_hci_establishment_resolution();
 static void hci_establishment_callback(ble_npl_event *);
 bool acquire_hci_event_user();
 void release_hci_event_user();
 void close_hci_event_users();
 bool await_hci_event_users();
 bool deinitialize_hci_event();
 bool open_hci_event_users();
 bool owns_hci_establishment(uint64_t, uint32_t,
                             ble_lifecycle::Generation, uint16_t) const;
 void resolve_hci_establishment(uint64_t);
 void retire_hci_establishment();
 void process_hci_establishment();
 bool physical_exposure_hidden() const;
 void terminate_hidden_connection(uint16_t handle) {
  assert(handle == peer);
  ++normal_terminations;
 }
 uint32_t stack_incarnation_ = 7;
 std::atomic<ble_lifecycle::Generation> generation_{11};
 hid_control_executor::BleEventSink *sink_ = nullptr;
 ble_npl_event hci_establishment_event_{};
 std::atomic_bool hci_establishment_event_initialized_{true};
 std::atomic<uint32_t> hci_event_users_{0};
 std::atomic<uint64_t> hci_establishment_id_{0};
 std::atomic<uint64_t> next_hci_establishment_id_{1};
 std::atomic<uint64_t> hci_establishment_disconnect_owner_{0};
 std::atomic<uint32_t> hci_establishment_stack_{0};
 std::atomic<ble_lifecycle::Generation> hci_establishment_generation_{0};
 std::atomic<uint16_t> hci_establishment_handle_{ble_lifecycle::kNoConnection};
 std::atomic_bool hci_ingress_enabled_{false};
 std::atomic<uint64_t> hci_ingress_epoch_{1};
 std::atomic_bool hidden_exposure_requested_{true};
 std::atomic_bool hidden_exposure_barrier_passed_{true};
};
Backend *Backend::instance_ = nullptr;
}
'''

POST = r'''
namespace {
void put_u16(uint8_t *destination, uint16_t value) {
 value = htole16(value);
 std::memcpy(destination, &value, sizeof(value));
}

void expect_ignored(uint8_t *event, uint16_t size) {
 const int before = original_recv;
 assert(registered_vhci->notify_host_recv(event, size) == 23);
 assert(original_recv == before + 1);
}
}

int main() {
 using ble_transport::Backend;
 Backend backend;
 hid_control_executor::BleEventSink sink;
 backend.sink_ = &sink;
 Backend::instance_ = &backend;
 backend.hci_establishment_event_.fn = Backend::hci_establishment_callback;
 backend.hci_establishment_event_.arg = &backend;

 assert(__wrap_esp_vhci_host_register_callback(nullptr) == ESP_ERR_INVALID_ARG);
 const esp_vhci_host_callback_t original{original_send_available, original_receive};
 assert(__wrap_esp_vhci_host_register_callback(&original) == 0);
 assert(registered_vhci != &original);
 registered_vhci->notify_host_send_available();
 assert(original_send == 1);
 backend.hci_ingress_enabled_.store(true);

 const esp_vhci_host_callback_t replacement{original_send_available,
                                             replacement_receive};
 assert(__wrap_esp_vhci_host_register_callback(&replacement) ==
        ESP_ERR_INVALID_STATE);
 assert(registered_vhci->notify_host_recv(nullptr, 0) == 23);

 uint8_t legacy[22] = {4, BLE_HCI_EVCODE_LE_META, 19,
                       BLE_HCI_LE_SUBEV_CONN_COMPLETE, BLE_ERR_SUCCESS};
 put_u16(legacy + 5, 55);
 legacy[7] = BLE_HCI_LE_CONN_COMPLETE_ROLE_SLAVE;
 assert(registered_vhci->notify_host_recv(legacy, sizeof(legacy)) == 23);
 const uint64_t first_id = backend.hci_establishment_id_.load();
 assert(first_id != 0 && first_id != UINT64_MAX);
 assert(registered_vhci->notify_host_recv(legacy, sizeof(legacy)) == 23);
 assert(backend.hci_establishment_id_.load() == first_id);
 assert(!backend.physical_exposure_hidden());
 run(backend.hci_establishment_event_);
 assert(raw_disconnects == 1 && last_raw_handle == 55);
 assert(backend.hci_establishment_disconnect_owner_.load() == first_id);

 // A Disconnect Complete is only a resolver wake. Even a matching handle
 // cannot resolve the exact establishment without a fresh absence probe.
 uint8_t wrong_disconnect[] = {4, BLE_HCI_EVCODE_DISCONN_CMP, 4,
                               BLE_ERR_SUCCESS, 54, 0, 19};
 expect_ignored(wrong_disconnect, sizeof(wrong_disconnect));
 assert(backend.hci_establishment_id_.load() == first_id);
 assert(backend.hci_establishment_event_.queued);
 run(backend.hci_establishment_event_);
 assert(backend.hci_establishment_id_.load() == first_id);
 uint8_t disconnected[] = {4, BLE_HCI_EVCODE_DISCONN_CMP, 4,
                           BLE_ERR_SUCCESS, 55, 0, 19};

 // A new Connection Complete after A owns a raw probe creates a new exact B
 // identity even when the controller reuses handle 55.
 assert(registered_vhci->notify_host_recv(legacy, sizeof(legacy)) == 23);
 const uint64_t second_id = backend.hci_establishment_id_.load();
 assert(second_id != first_id);
 assert(backend.hci_establishment_disconnect_owner_.load() == 0);

 // A stale completion from A cannot clear B, either before or after B owns a
 // successful Disconnect probe. It only wakes the exact probe resolver.
 expect_ignored(disconnected, sizeof(disconnected));
 assert(backend.hci_establishment_id_.load() == second_id);
 run(backend.hci_establishment_event_);
 assert(backend.hci_establishment_id_.load() == second_id);
 assert(backend.hci_establishment_disconnect_owner_.load() == second_id);
 expect_ignored(disconnected, sizeof(disconnected));
 assert(backend.hci_establishment_id_.load() == second_id);
 run(backend.hci_establishment_event_);
 assert(backend.hci_establishment_id_.load() == second_id);

 // Even B's real completion is only a wake. A subsequent exact converted
 // Unknown Connection ID proves current controller absence and resolves B.
 raw_result = BLE_HS_HCI_ERR(BLE_ERR_UNK_CONN_ID);
 expect_ignored(disconnected, sizeof(disconnected));
 run(backend.hci_establishment_event_);
 assert(backend.physical_exposure_hidden());

 // Immediate converted absence closes the exact token. Raw 2 and unrelated
 // host errors remain faults and keep authority live.
 raw_result = BLE_HS_HCI_ERR(BLE_ERR_UNK_CONN_ID);
 assert(registered_vhci->notify_host_recv(legacy, sizeof(legacy)) == 23);
 run(backend.hci_establishment_event_);
 assert(backend.physical_exposure_hidden());
 raw_result = BLE_ERR_UNK_CONN_ID;
 assert(registered_vhci->notify_host_recv(legacy, sizeof(legacy)) == 23);
 run(backend.hci_establishment_event_);
 assert(!backend.physical_exposure_hidden());
 backend.retire_hci_establishment();
 backend.hci_ingress_enabled_.store(true);
 raw_result = 7;
 assert(registered_vhci->notify_host_recv(legacy, sizeof(legacy)) == 23);
 run(backend.hci_establishment_event_);
 assert(!backend.physical_exposure_hidden());
 backend.retire_hci_establishment();
 backend.hci_ingress_enabled_.store(true);

 // Replacement during a synchronous absence probe cannot be erased by the
 // predecessor's command result; the complete exact tuple is rechecked.
 uint64_t replacement_id = 0;
 command_hook = [&]() {
  assert(Backend::observe_hci_ingress(legacy + 1));
  replacement_id = backend.hci_establishment_id_.load();
  command_hook = nullptr;
 };
 raw_result = BLE_HS_HCI_ERR(BLE_ERR_UNK_CONN_ID);
 assert(registered_vhci->notify_host_recv(legacy, sizeof(legacy)) == 23);
 run(backend.hci_establishment_event_);
 assert(replacement_id != 0);
 assert(backend.hci_establishment_id_.load() == replacement_id);
 backend.retire_hci_establishment();
 backend.hci_ingress_enabled_.store(true);
 raw_result = 0;

 // A host-registered exact handle transfers teardown to the normal GAP path.
 peer = 56;
 put_u16(legacy + 5, 56);
 assert(registered_vhci->notify_host_recv(legacy, sizeof(legacy)) == 23);
 run(backend.hci_establishment_event_);
 assert(normal_terminations == 1);
 assert(backend.hci_establishment_id_.load() == 0);
 peer = 0xffff;

 // Each supported event class must carry its full pinned structure.
 uint8_t enhanced[34] = {4, BLE_HCI_EVCODE_LE_META, 31,
                         BLE_HCI_LE_SUBEV_ENH_CONN_COMPLETE, BLE_ERR_SUCCESS};
 put_u16(enhanced + 5, 57);
 enhanced[7] = BLE_HCI_LE_CONN_COMPLETE_ROLE_SLAVE;
 assert(registered_vhci->notify_host_recv(enhanced, sizeof(enhanced)) == 23);
 backend.resolve_hci_establishment(backend.hci_establishment_id_.load());
 enhanced[3] = BLE_HCI_LE_SUBEV_ENH_CONN_COMPLETE_V2;
 expect_ignored(enhanced, sizeof(enhanced));
 assert(backend.hci_establishment_id_.load() == 0);

 uint8_t short_legacy[8] = {4, BLE_HCI_EVCODE_LE_META, 5,
                            BLE_HCI_LE_SUBEV_CONN_COMPLETE, 0, 58, 0, 1};
 uint8_t short_enhanced[8] = {4, BLE_HCI_EVCODE_LE_META, 5,
                              BLE_HCI_LE_SUBEV_ENH_CONN_COMPLETE, 0, 58, 0, 1};
 uint8_t short_v2[8] = {4, BLE_HCI_EVCODE_LE_META, 5,
                        BLE_HCI_LE_SUBEV_ENH_CONN_COMPLETE_V2, 0, 58, 0, 1};
 uint8_t truncated_header[] = {4, BLE_HCI_EVCODE_LE_META};
 expect_ignored(truncated_header, sizeof(truncated_header));
 expect_ignored(short_legacy, sizeof(short_legacy));
 expect_ignored(short_enhanced, sizeof(short_enhanced));
 expect_ignored(short_v2, sizeof(short_v2));
 uint8_t mismatched_h4[8] = {4, BLE_HCI_EVCODE_LE_META, 19,
                             BLE_HCI_LE_SUBEV_CONN_COMPLETE, 0, 58, 0, 1};
 expect_ignored(mismatched_h4, sizeof(mismatched_h4));
 uint8_t central[sizeof(legacy)];
 std::memcpy(central, legacy, sizeof(legacy));
 central[7] = 0;
 expect_ignored(central, sizeof(central));
 uint8_t failed[sizeof(legacy)];
 std::memcpy(failed, legacy, sizeof(legacy));
 failed[4] = 1;
 expect_ignored(failed, sizeof(failed));
 uint8_t unrelated[] = {4, BLE_HCI_EVCODE_LE_META, 1, 0x02};
 expect_ignored(unrelated, sizeof(unrelated));
 assert(backend.hci_establishment_id_.load() == 0);

 // Queue-use leases make the check/use interval indivisible from event
 // lifetime. Stop closes admission while the callback owns one exact user;
 // deinit is legal only after that user releases.
 backend.hci_ingress_enabled_.store(true);
 backend.hci_establishment_event_.queued = false;
 const int puts_before_lease_seam = queue_puts;
 queue_put_hook = [&]() {
  assert((backend.hci_event_users_.load() &
          ble_transport::kHciEventUsersMask) == 1);
  backend.close_hci_event_users();
  assert(!backend.acquire_hci_event_user());
  assert(event_alive);
  queue_put_hook = nullptr;
 };
 Backend::queue_hci_establishment_resolution();
 assert(queue_puts == puts_before_lease_seam + 1);
 assert(backend.hci_event_users_.load() ==
        ble_transport::kHciEventUsersClosing);
 event_alive = false; // Models deinit only after the admitted user drained.

 // New init cannot open while users remain. Several callback leases are
 // counted exactly and a callback that decides not to queue still releases.
 event_alive = true;
 assert(backend.open_hci_event_users());
 assert(backend.acquire_hci_event_user());
 assert(backend.acquire_hci_event_user());
 backend.close_hci_event_users();
 assert(!backend.open_hci_event_users());
 backend.release_hci_event_user();
 assert(!backend.open_hci_event_users());
 backend.release_hci_event_user();
 assert(backend.open_hci_event_users());
 backend.hci_ingress_enabled_.store(false);
 const int checks_before_skip = queue_checks;
 Backend::queue_hci_establishment_resolution();
 assert(queue_checks == checks_before_skip);

 // Normal stop context waits for all admitted users. Timeout leaves the
 // event alive and the user counted; it never licenses forced deinit.
 backend.close_hci_event_users();
 assert(backend.open_hci_event_users());
 assert(backend.acquire_hci_event_user());
 backend.close_hci_event_users();
 delay_hook = [&]() {
  backend.release_hci_event_user();
  delay_hook = nullptr;
 };
 assert(backend.await_hci_event_users());
 assert(backend.open_hci_event_users());
 assert(backend.acquire_hci_event_user());
 backend.close_hci_event_users();
 fake_time_us = 0;
 assert(!backend.await_hci_event_users());
 assert((backend.hci_event_users_.load() &
         ble_transport::kHciEventUsersMask) == 1);
 assert(event_alive);
 assert(!backend.deinitialize_hci_event());
 assert(event_alive);
 backend.release_hci_event_user();
 assert(backend.deinitialize_hci_event());
 assert(!event_alive);
 assert(backend.hci_event_users_.load() ==
        ble_transport::kHciEventUsersClosing);
 backend.close_hci_event_users();
 Backend::queue_hci_establishment_resolution();
 assert(queue_checks == checks_before_skip);

 backend.retire_hci_establishment();
 real_registration_result = -77;
 assert(__wrap_esp_vhci_host_register_callback(&replacement) == -77);
 assert(registered_vhci->notify_host_recv(nullptr, 0) == 23);
 real_registration_result = 0;
 assert(__wrap_esp_vhci_host_register_callback(&replacement) == 0);
 assert(registered_vhci->notify_host_recv(legacy, sizeof(legacy)) == 29);
 assert(replacement_recv == 1);
 assert(backend.hci_establishment_id_.load() == 0);
 std::cout << "PASS: exact HCI establishment structure, domain, and identity\n";
}
'''

proxy_start = SOURCE.index("namespace {\nusing VhciSendAvailable")
wrapper_end = SOURCE.index("\nnamespace ble_transport {", proxy_start)
code = PRE + "\n" + SOURCE[proxy_start:wrapper_end] + "\nnamespace ble_transport {\n"
for signature in (
    "bool allocate_hci_establishment_id",
    "int disconnect_controller_handle",
    "bool Backend::physical_exposure_hidden() const",
    "bool Backend::hci_callback_registration_allowed",
    "bool Backend::observe_hci_ingress",
    "void Backend::queue_hci_establishment_resolution",
    "void Backend::hci_establishment_callback",
    "bool Backend::acquire_hci_event_user",
    "void Backend::release_hci_event_user",
    "void Backend::close_hci_event_users",
    "bool Backend::await_hci_event_users",
    "bool Backend::deinitialize_hci_event",
    "bool Backend::open_hci_event_users",
    "bool Backend::owns_hci_establishment",
    "void Backend::resolve_hci_establishment",
    "void Backend::retire_hci_establishment",
    "void Backend::process_hci_establishment",
):
    code += "\n" + body(SOURCE, signature) + "\n"
code += "\n}\n" + POST

with tempfile.TemporaryDirectory(prefix="hidbot-hci-establishment-") as temp:
    cpp = Path(temp) / "test.cpp"
    out = Path(temp) / "test"
    cpp.write_text(code)
    subprocess.run(
        ["c++", "-std=c++20", "-Wall", "-Wextra", "-Werror", str(cpp),
         "-o", str(out)],
        check=True,
    )
    subprocess.run([str(out)], check=True)
