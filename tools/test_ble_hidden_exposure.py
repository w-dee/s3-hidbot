#!/usr/bin/env python3
"""Execute the actual pinned-host hide adapter with adversarial host state."""
from pathlib import Path
import os
import subprocess
import tempfile
from test_bond_delete_nvs import ROOT, body

PRE = r'''
#include <cassert>
#include <cstdint>
namespace ble_lifecycle {constexpr uint16_t kNoConnection=0xffff;}
constexpr int BLE_HS_EALREADY=2, BLE_HS_ENOTCONN=7, BLE_ERR_REM_USER_CONN_TERM=19;
bool locked=false, enabled=true, advertising=true;
uint16_t peer=ble_lifecycle::kNoConnection;
int stop_result=0, terminate_result=0, term_calls=0, stop_calls=0;
void ble_hs_lock(){assert(!locked);locked=true;}
void ble_hs_unlock(){assert(locked);locked=false;}
bool ble_hs_is_enabled(){return enabled;}
int ble_gap_adv_stop(){assert(!locked);++stop_calls;if(stop_result==0||stop_result==2)advertising=false;return stop_result;}
int ble_gap_adv_active(){assert(locked);return advertising;}
void ble_gap_conn_foreach_handle(int(*callback)(uint16_t,void*),void*arg){assert(locked);if(peer!=ble_lifecycle::kNoConnection)callback(peer,arg);}
int ble_gap_terminate(uint16_t handle,int reason){assert(!locked);assert(handle==peer);assert(!advertising);assert(reason==19);++term_calls;return terminate_result;}
struct Backend {int32_t begin_hidden_exposure();bool physical_exposure_hidden() const;};
'''
TEST = r'''
int main(){
 Backend b;
 // Connection establishment already stopped advertising. EALREADY is not
 // physical absence: the exact peer still needs termination and observation.
 peer=41;stop_result=BLE_HS_EALREADY;
 assert(b.begin_hidden_exposure()==0);assert(stop_calls==1&&term_calls==1);
 assert(!b.physical_exposure_hidden());
 peer=ble_lifecycle::kNoConnection;assert(b.physical_exposure_hidden());
 for(int result:{BLE_HS_EALREADY,BLE_HS_ENOTCONN}){
  peer=42;terminate_result=result;assert(b.begin_hidden_exposure()==0);
  assert(!b.physical_exposure_hidden()); // An API result never manufactures absence.
 }
 terminate_result=-91;assert(b.begin_hidden_exposure()==-91);
 auto previous=term_calls;stop_result=-92;assert(b.begin_hidden_exposure()==-92);assert(term_calls==previous);
 peer=ble_lifecycle::kNoConnection;stop_result=2;assert(b.begin_hidden_exposure()==0);assert(term_calls==previous);
 advertising=true;assert(!b.physical_exposure_hidden());advertising=false;
 enabled=false;assert(!b.physical_exposure_hidden());enabled=true;assert(b.physical_exposure_hidden());
}
'''

def main():
    source=(ROOT/'firmware/components/ble_transport/ble_transport.cpp').read_text()
    code='#include <initializer_list>\n'+PRE+'\n'+body(source,'std::int32_t Backend::begin_hidden_exposure()')+'\n'+body(source,'bool Backend::physical_exposure_hidden() const')+'\n'+TEST
    with tempfile.TemporaryDirectory(prefix='hidbot-hidden-') as temp:
        cpp=Path(temp)/'test.cpp';out=Path(temp)/'test';cpp.write_text(code)
        subprocess.run([os.environ.get('CXX','c++'),'-std=c++20','-Wall','-Wextra','-Werror',str(cpp),'-o',str(out)],check=True)
        subprocess.run([str(out)],check=True)
    print('PASS: actual BLE hidden exposure adapter and physical-absence authority')

if __name__=='__main__':main()
