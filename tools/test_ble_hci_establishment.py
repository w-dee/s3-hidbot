#!/usr/bin/env python3
"""Exercise the production HCI-ingress establishment authority without hardware."""
from pathlib import Path
import subprocess
import tempfile

from test_bond_delete_nvs import ROOT, body

SOURCE = (ROOT / "firmware/components/ble_transport/ble_transport.cpp").read_text()

PRE = r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <utility>
#include <iostream>
#define BLE_HCI_EVCODE_DISCONN_CMP 0x05
#define BLE_HCI_EVCODE_LE_META 0x3e
#define BLE_HCI_LE_SUBEV_CONN_COMPLETE 0x01
#define BLE_HCI_LE_SUBEV_ENH_CONN_COMPLETE 0x0a
#define BLE_HCI_LE_SUBEV_ENH_CONN_COMPLETE_V2 0x29
#define BLE_HCI_LE_CONN_COMPLETE_ROLE_SLAVE 1
#define BLE_HCI_OGF_LINK_CTRL 1
#define BLE_HCI_OCF_DISCONNECT_CMD 6
#define BLE_HCI_OP(ogf, ocf) ((ocf) | ((ogf) << 10))
#define BLE_HCI_UART_H4_EVT 4
#define BLE_ERR_SUCCESS 0
#define BLE_ERR_UNK_CONN_ID 2
#define BLE_ERR_REM_USER_CONN_TERM 19
using esp_err_t=int;
constexpr int ESP_ERR_INVALID_STATE=0x103,ESP_ERR_INVALID_ARG=0x102;
struct esp_vhci_host_callback_t {void(*notify_host_send_available)(void);int(*notify_host_recv)(uint8_t*,uint16_t);};
const esp_vhci_host_callback_t*registered_vhci=nullptr;int original_recv=0,original_send=0;
extern "C" esp_err_t __real_esp_vhci_host_register_callback(const esp_vhci_host_callback_t*callback){registered_vhci=callback;return 0;}
void original_send_available(){++original_send;}
int original_receive(uint8_t*,uint16_t){++original_recv;return 23;}
struct ble_hci_lc_disconnect_cp {uint16_t conn_handle;uint8_t reason;} __attribute__((packed));
struct ble_npl_event {void(*fn)(ble_npl_event*)=nullptr;void*arg=nullptr;bool queued=false;};
struct ble_npl_eventq{} queue;
void*ble_npl_event_get_arg(ble_npl_event*e){return e->arg;}
bool ble_npl_event_is_queued(ble_npl_event*e){return e->queued;}
ble_npl_eventq*nimble_port_get_dflt_eventq(){return &queue;}
void ble_npl_eventq_put(ble_npl_eventq*,ble_npl_event*e){assert(!e->queued);e->queued=true;}
void run(ble_npl_event&e){assert(e.queued);e.queued=false;e.fn(&e);}
bool enabled=true,advertising=false,locked=false;uint16_t peer=0xffff;
int raw_disconnects=0,normal_terminations=0,last_raw_handle=-1;
bool ble_hs_is_enabled(){return enabled;}int ble_gap_adv_active(){assert(locked);return advertising;}
void ble_hs_lock(){assert(!locked);locked=true;}void ble_hs_unlock(){assert(locked);locked=false;}
using foreach_fn=int(uint16_t,void*);
void ble_gap_conn_foreach_handle(foreach_fn*fn,void*arg){assert(locked);if(peer!=0xffff)fn(peer,arg);}
int ble_hs_hci_cmd_tx(uint16_t,const void*value,uint8_t,void*,uint8_t){
 auto*c=static_cast<const ble_hci_lc_disconnect_cp*>(value);++raw_disconnects;last_raw_handle=c->conn_handle;return 0;}
namespace hid_control_executor {struct BleEventSink{int faults=0;void signal_ble_lifecycle_handoff_failure(){++faults;}};}
namespace ble_transport {
constexpr uint64_t kHciEstablishmentPresent=UINT64_C(1)<<48;
uint64_t hci_establishment_token(uint32_t stack,uint16_t handle);
uint16_t hci_establishment_handle(uint64_t token);
int disconnect_controller_handle(uint16_t handle);
class Backend {
public:
 static Backend*instance_;
 static bool observe_hci_ingress(const uint8_t*);
 static void queue_hci_establishment_resolution();
 static void hci_establishment_callback(ble_npl_event*);
 void resolve_hci_establishment(uint16_t);
 void process_hci_establishment();
 bool physical_exposure_hidden()const;
 void terminate_hidden_connection(uint16_t h){assert(h==peer);++normal_terminations;}
 uint32_t stack_incarnation_=7;
 hid_control_executor::BleEventSink*sink_=nullptr;
 ble_npl_event hci_establishment_event_{};
 bool hci_establishment_event_initialized_=true;
 std::atomic<uint64_t>hci_establishment_{0};
 std::atomic_bool hci_establishment_teardown_claimed_{false};
 std::atomic_bool hidden_exposure_requested_{true};
 std::atomic_bool hidden_exposure_barrier_passed_{true};
};
Backend*Backend::instance_=nullptr;
}
'''

POST = r'''
int main(){using ble_transport::Backend;Backend backend;hid_control_executor::BleEventSink sink;
 backend.sink_=&sink;Backend::instance_=&backend;backend.hci_establishment_event_.fn=Backend::hci_establishment_callback;backend.hci_establishment_event_.arg=&backend;
 const esp_vhci_host_callback_t original{original_send_available,original_receive};
 assert(__wrap_esp_vhci_host_register_callback(&original)==0);assert(registered_vhci!=&original);
 registered_vhci->notify_host_send_available();assert(original_send==1);
 uint8_t complete[]={4,0x3e,5,1,0,55,0,1};
 assert(registered_vhci->notify_host_recv(complete,sizeof complete)==23);assert(original_recv==1);
 assert(registered_vhci->notify_host_recv(complete,sizeof complete)==23);assert(original_recv==2);
 assert(!backend.physical_exposure_hidden());run(backend.hci_establishment_event_);
 assert(raw_disconnects==1&&last_raw_handle==55);assert(!backend.physical_exposure_hidden());
 backend.process_hci_establishment();assert(raw_disconnects==1);
 uint8_t disconnected[]={4,5,4,0,55,0,19};
 assert(registered_vhci->notify_host_recv(disconnected,sizeof disconnected)==23);assert(backend.physical_exposure_hidden());
 assert(registered_vhci->notify_host_recv(disconnected,sizeof disconnected)==23);assert(backend.physical_exposure_hidden());
 backend.stack_incarnation_=8;peer=56;uint8_t registered[]={4,0x3e,5,1,0,56,0,1};
 assert(registered_vhci->notify_host_recv(registered,sizeof registered)==23);run(backend.hci_establishment_event_);
 assert(normal_terminations==1&&raw_disconnects==1);assert(!backend.physical_exposure_hidden());
 peer=0xffff;assert(backend.physical_exposure_hidden());assert(sink.faults==0);
 std::cout<<"PASS: pre-admission HCI establishment authority and exact teardown\n";
}
'''

proxy_start = SOURCE.index("namespace {\nusing VhciSendAvailable")
wrapper_end = SOURCE.index("\nnamespace ble_transport {", proxy_start)
code = PRE + "\n" + SOURCE[proxy_start:wrapper_end] + "\nnamespace ble_transport {\n"
for signature in (
    "std::uint64_t hci_establishment_token",
    "std::uint16_t hci_establishment_handle",
    "int disconnect_controller_handle",
    "bool Backend::physical_exposure_hidden() const",
    "bool Backend::observe_hci_ingress",
    "void Backend::queue_hci_establishment_resolution",
    "void Backend::hci_establishment_callback",
    "void Backend::resolve_hci_establishment",
    "void Backend::process_hci_establishment",
):
    code += "\n" + body(SOURCE, signature) + "\n"
code += "\n}\n" + POST

with tempfile.TemporaryDirectory(prefix="hidbot-hci-establishment-") as temp:
    cpp=Path(temp)/"test.cpp";out=Path(temp)/"test";cpp.write_text(code)
    subprocess.run(["c++","-std=c++20","-Wall","-Wextra","-Werror",str(cpp),"-o",str(out)],check=True)
    subprocess.run([str(out)],check=True)
