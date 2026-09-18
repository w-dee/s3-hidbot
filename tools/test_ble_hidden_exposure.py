#!/usr/bin/env python3
"""Execute the actual pinned-host hide adapter with adversarial host state."""
from pathlib import Path
import os
import subprocess
import tempfile
from test_bond_delete_nvs import ROOT, body

PRE = r'''
#include <atomic>
#include <cassert>
#include <cstdint>
namespace ble_lifecycle {constexpr uint16_t kNoConnection=0xffff;}
constexpr int BLE_HS_EALREADY=2, BLE_HS_ENOTCONN=7, BLE_ERR_REM_USER_CONN_TERM=19,
 ESP_ERR_INVALID_STATE=103;
bool locked=false, enabled=true, advertising=true;
uint16_t peer=ble_lifecycle::kNoConnection;
int stop_result=0, terminate_result=0, term_calls=0, stop_calls=0;
struct ble_npl_event {void(*fn)(ble_npl_event*)=nullptr;void*arg=nullptr;bool queued=false;};
struct ble_npl_eventq {} eventq;
void ble_hs_lock(){assert(!locked);locked=true;}
void ble_hs_unlock(){assert(locked);locked=false;}
bool ble_hs_is_enabled(){return enabled;}
int ble_gap_adv_stop(){assert(!locked);++stop_calls;if(stop_result==0||stop_result==2)advertising=false;return stop_result;}
int ble_gap_adv_active(){assert(locked);return advertising;}
void ble_gap_conn_foreach_handle(int(*callback)(uint16_t,void*),void*arg){assert(locked);if(peer!=ble_lifecycle::kNoConnection)callback(peer,arg);}
int ble_gap_terminate(uint16_t handle,int reason){assert(!locked);assert(handle==peer);assert(!advertising);assert(reason==19);++term_calls;return terminate_result;}
bool ble_npl_event_is_queued(ble_npl_event*event){return event->queued;}
void*ble_npl_event_get_arg(ble_npl_event*event){return event->arg;}
ble_npl_eventq*nimble_port_get_dflt_eventq(){return &eventq;}
void ble_npl_eventq_put(ble_npl_eventq*,ble_npl_event*event){assert(!event->queued);event->queued=true;}
void run_barrier(ble_npl_event&event){assert(event.queued);event.queued=false;event.fn(&event);}
struct Backend {
 Backend(){hidden_exposure_barrier_.fn=hidden_exposure_barrier_callback;hidden_exposure_barrier_.arg=this;}
 int32_t begin_hidden_exposure();bool physical_exposure_hidden() const;
 static void hidden_exposure_barrier_callback(ble_npl_event*);
 void terminate_hidden_connection(uint16_t);
 ble_npl_event hidden_exposure_barrier_{};
 bool hidden_exposure_barrier_initialized_=true;
 std::atomic_bool hidden_exposure_requested_{false};
 std::atomic_bool hidden_exposure_barrier_passed_{false};
 std::atomic_bool hidden_exposure_termination_claimed_{false};
 std::atomic<std::uint64_t> hci_establishment_id_{0};
};
'''
TEST = r'''
int main(){
 Backend b;
 // Connection establishment already stopped advertising. EALREADY is not
 // physical absence: the exact peer still needs termination and observation.
 peer=41;stop_result=BLE_HS_EALREADY;
 assert(b.begin_hidden_exposure()==0);assert(stop_calls==1&&term_calls==1);
 assert(!b.physical_exposure_hidden());
 peer=ble_lifecycle::kNoConnection;assert(!b.physical_exposure_hidden());
 run_barrier(b.hidden_exposure_barrier_);assert(b.physical_exposure_hidden());
 for(int result:{BLE_HS_EALREADY,BLE_HS_ENOTCONN}){
  peer=42;terminate_result=result;assert(b.begin_hidden_exposure()==0);
  assert(!b.physical_exposure_hidden()); // An API result never manufactures absence.
  peer=ble_lifecycle::kNoConnection;run_barrier(b.hidden_exposure_barrier_);
 }
 peer=42;terminate_result=-91;assert(b.begin_hidden_exposure()==-91);
 auto previous=term_calls;stop_result=-92;assert(b.begin_hidden_exposure()==-92);assert(term_calls==previous);
 peer=ble_lifecycle::kNoConnection;stop_result=2;assert(b.begin_hidden_exposure()==0);assert(term_calls==previous);
 assert(!b.physical_exposure_hidden());run_barrier(b.hidden_exposure_barrier_);
 advertising=true;assert(!b.physical_exposure_hidden());advertising=false;
 enabled=false;assert(!b.physical_exposure_hidden());enabled=true;assert(b.physical_exposure_hidden());
 b.hci_establishment_id_=1;assert(!b.physical_exposure_hidden());
 b.hci_establishment_id_=0;assert(b.physical_exposure_hidden());

 // F1 schedule: Connection Complete is accepted, hide sees no registered
 // peer, then the GAP callback publishes the peer before the host barrier.
 terminate_result=0;peer=ble_lifecycle::kNoConnection;stop_result=BLE_HS_EALREADY;
 assert(b.begin_hidden_exposure()==0);previous=term_calls;
 peer=55;b.terminate_hidden_connection(55);
 assert(term_calls==previous+1);
 b.terminate_hidden_connection(55);assert(term_calls==previous+1);
 assert(!b.physical_exposure_hidden());
 run_barrier(b.hidden_exposure_barrier_);assert(!b.physical_exposure_hidden());
 peer=ble_lifecycle::kNoConnection;assert(b.physical_exposure_hidden());
}
'''

def main():
    source=(ROOT/'firmware/components/ble_transport/ble_transport.cpp').read_text()
    connect_case=source[source.index('case BLE_GAP_EVENT_CONNECT:'):
                        source.index('case BLE_GAP_EVENT_DISCONNECT:')]
    assert connect_case.count(
        'backend->terminate_hidden_connection(\n                    event->connect.conn_handle);'
    ) == 1
    assert connect_case.index('backend->terminate_hidden_connection(') < \
        connect_case.index('(void)backend->signal(')
    assert source.count(
        'ble_npl_eventq_put(nimble_port_get_dflt_eventq(),\n'
        '                       &hidden_exposure_barrier_);'
    ) == 1
    code='#include <initializer_list>\n'+PRE+'\n'+body(source,'std::int32_t Backend::begin_hidden_exposure()')+'\n'+body(source,'bool Backend::physical_exposure_hidden() const')+'\n'+body(source,'void Backend::hidden_exposure_barrier_callback')+'\n'+body(source,'void Backend::terminate_hidden_connection')+'\n'+TEST
    with tempfile.TemporaryDirectory(prefix='hidbot-hidden-') as temp:
        cpp=Path(temp)/'test.cpp';out=Path(temp)/'test';cpp.write_text(code)
        subprocess.run([os.environ.get('CXX','c++'),'-std=c++20','-Wall','-Wextra','-Werror',str(cpp),'-o',str(out)],check=True)
        subprocess.run([str(out)],check=True)
    print('PASS: actual BLE hidden exposure adapter and physical-absence authority')

if __name__=='__main__':main()
