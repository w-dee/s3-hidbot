#!/usr/bin/env python3
"""Run actual association NVS adapter and NimBLE store interposition with fakes."""
from pathlib import Path
import os
import subprocess
import tempfile
from test_bond_delete_nvs import PRE, ROOT, body

EXTRA = r'''
#include <atomic>
#define ESP_LOGW(...) ((void)0)
#include <algorithm>
constexpr int ESP_ERR_NVS_NOT_ENOUGH_SPACE=104, BLE_HS_EINVAL=21,
 BLE_HS_ENOENT=22, BLE_HS_ESTORE_FAIL=23, BLE_HS_ESTORE_CAP=24;
constexpr int BLE_ADDR_PUBLIC=0, BLE_ADDR_RANDOM=1;
constexpr int BLE_STORE_OBJ_TYPE_OUR_SEC=1, BLE_STORE_OBJ_TYPE_PEER_SEC=2, BLE_STORE_OBJ_TYPE_CCCD=3;
struct ble_store_value_sec {
 ble_addr_t peer_addr; uint8_t ltk_present,authenticated,sc,key_size;
 uint8_t synthetic_ltk[16];
};
struct ble_store_key_sec {ble_addr_t peer_addr; uint8_t idx;};
struct Cccd {ble_addr_t peer_addr;uint16_t chr_val_handle;uint8_t idx;};
union ble_store_key {ble_store_key_sec sec; Cccd cccd;};
union ble_store_value {ble_store_value_sec sec; Cccd cccd;};
using ble_store_read_fn=int(int,const ble_store_key*,ble_store_value*);
using ble_store_write_fn=int(int,const ble_store_value*);
constexpr std::array<const char*,3> kNimbleOurSecurityKeys{"our_sec_1","our_sec_2","our_sec_3"};
constexpr std::array<const char*,3> kNimblePeerSecurityKeys{"peer_sec_1","peer_sec_2","peer_sec_3"};
int ble_addr_cmp(const ble_addr_t*a,const ble_addr_t*b){return std::memcmp(a,b,sizeof(*a));}
bool has_exact_identity(const ble_addr_t&a){return a.type<=1 && (a.type||std::any_of(a.val,a.val+6,[](auto b){return b!=0;}));}
ble_addr_t connected{1,{2,3,4,5,6,7}};
bool host_locked=false;
struct HostLock {HostLock(){assert(!host_locked);host_locked=true;} ~HostLock(){assert(host_locked);host_locked=false;}};
[[maybe_unused]] bool peer_identity(uint16_t connection,ble_addr_t&out){HostLock lock;out=connected;return connection==7;}
constexpr int kInventoryOurSecurity=0x7001,kInventoryPeerSecurity=0x7002;
int ble_store_read(int,const ble_store_key*,ble_store_value*);
// Match the pinned SDK's public store wrapper lock contract. The production
// callback and inventory bridge below are extracted, not reimplemented.

int nvs_set_u32(int h,const char*k,uint32_t value){
 const bool error=fail();if(error&&!after)return 77;
 std::vector<uint8_t> bytes(4);std::memcpy(bytes.data(),&value,4);
 disk[{handles.at(h),k}]={NVS_TYPE_U32,bytes};return error?77:0;
}
int raw_reads=0,raw_writes=0,store_failures=0,raw_fail=0;
std::map<int,ble_store_value> ram;
int raw_read(int type,const ble_store_key*,ble_store_value*out){
 assert(host_locked);++raw_reads;if(!ram.count(type))return BLE_HS_ENOENT;*out=ram.at(type);return 0;
}
int raw_write(int type,const ble_store_value*value){
 ++raw_writes;if(raw_fail==1)return 77;ram[type]=*value;
 if(raw_fail==2)return 77;
 if(type==BLE_STORE_OBJ_TYPE_OUR_SEC||type==BLE_STORE_OBJ_TYPE_PEER_SEC){
  auto key=type==BLE_STORE_OBJ_TYPE_OUR_SEC?"our_sec_1":"peer_sec_1";
  std::vector<uint8_t> bytes(sizeof(value->sec));std::memcpy(bytes.data(),&value->sec,bytes.size());
  disk[{kNimbleStoreNamespace,key}]={NVS_TYPE_BLOB,bytes};
 }
 return raw_fail==3?77:0;
}
struct Backend {
 static Backend* instance_;
 ble_store_read_fn*original_store_read_=raw_read;
 ble_store_write_fn*original_store_write_=raw_write;
 const ble_fixture_profile::ProfileDefinition* profile_=&ble_fixture_profile::kStandaloneMouseJustWorks;
 detail::AssociationCreation association_creation_{};
 uint64_t active_host_connection_=1;
 uint16_t host_connection_handle_=7;
 ble_addr_t host_connection_identity_=connected;
 bool host_identity_valid_=true;
 std::atomic<uint16_t> current_connection_{7};
 std::atomic<uint32_t> generation_{1};
 ble_security::ReadinessInhibit security_inhibit_{};
 ble_security::State security_{};
 int read_security_raw(bool,const ble_store_key_sec&,ble_store_value_sec&)const;
 bool compatible_association(const ble_addr_t&)const;
 static int store_read(int,const ble_store_key*,ble_store_value*);
 static int store_write(int,const ble_store_value*);
 void observe_store_failure(ble_security::StoreFailureKind,int,bool persistent,uint16_t handle){
  ++store_failures; (void)security_inhibit_.inhibit(generation_.load(),handle,persistent);
 }
};
Backend* Backend::instance_=nullptr;
int ble_store_read(int type,const ble_store_key*key,ble_store_value*value){
 HostLock lock;return Backend::store_read(type,key,value);
}
int ble_store_write(int type,const ble_store_value*value){
 HostLock lock;return Backend::store_write(type,value);
}
'''

POST = r'''
void reset(){assert(handles.empty()&&live_iterators==0);disk.clear();ram.clear();calls=0;cut=-1;after=false;raw_reads=raw_writes=store_failures=raw_fail=0;}
ble_store_value key_value(bool authenticated=false){ble_store_value result{};
 result.sec.peer_addr=connected;result.sec.ltk_present=1;result.sec.authenticated=authenticated;
 result.sec.sc=1;result.sec.key_size=16;result.sec.synthetic_ltk[0]=42;return result;}
void both(Backend&b){Backend::instance_=&b;auto value=key_value();
 assert(ble_store_write(BLE_STORE_OBJ_TYPE_OUR_SEC,&value)==0);
 assert(read_association(connected).record.state==detail::AssociationState::kPending);
 assert(ble_store_write(BLE_STORE_OBJ_TYPE_PEER_SEC,&value)==0);
 assert(read_association(connected).record.state==detail::AssociationState::kComplete);}
int main(){
 using S=detail::AssociationState;
 reset();Backend b;both(b);assert(validate_complete_associations()==0);
 auto complete=disk;auto retained_ram=ram;
 ble_store_key key{};key.sec.peer_addr=connected;ble_store_value out{};
 assert(ble_store_read(BLE_STORE_OBJ_TYPE_OUR_SEC,&key,&out)==0);
 assert(out.sec.synthetic_ltk[0]==42);
 // Exact retained class mismatch cannot look like missing or leak key bytes.
 b.profile_=&ble_fixture_profile::kStrictComposite;
 auto before=disk;assert(ble_store_read(BLE_STORE_OBJ_TYPE_OUR_SEC,&key,&out)==BLE_HS_ESTORE_FAIL);
 assert(disk==before&&store_failures==0);
 auto value=key_value(true);assert(ble_store_write(BLE_STORE_OBJ_TYPE_OUR_SEC,&value)==BLE_HS_ESTORE_FAIL);
 assert(disk==before&&store_failures==0);
 ble_store_value_sec raw{};assert(b.read_security_raw(true,key.sec,raw)==0);
 ble_store_key enumeration{};assert(ble_store_read(BLE_STORE_OBJ_TYPE_OUR_SEC,&enumeration,&out)==0);
 // Metadata-less authenticated strict survives; mouse must not adopt it.
 reset();Backend strict;Backend::instance_=&strict;strict.profile_=&ble_fixture_profile::kStrictComposite;
 value=key_value(true);assert(ble_store_write(BLE_STORE_OBJ_TYPE_OUR_SEC,&value)==0);
 assert(read_association(connected).record.state==S::kMissing);
 strict.profile_=&ble_fixture_profile::kStandaloneMouseJustWorks;
 assert(ble_store_read(BLE_STORE_OBJ_TYPE_OUR_SEC,&key,&out)==BLE_HS_ESTORE_FAIL);
 assert(out.sec.synthetic_ltk[0]==0);
 value=key_value();assert(ble_store_write(BLE_STORE_OBJ_TYPE_PEER_SEC,&value)==BLE_HS_ESTORE_FAIL);
 assert(read_association(connected).record.state==S::kMissing);
 // Valid CCCD restore requires a matching durable schema before stack consumption.
 reset();Backend c;both(c);ble_store_value cccd{};cccd.cccd.peer_addr=connected;cccd.cccd.chr_val_handle=25;
 assert(ble_store_write(BLE_STORE_OBJ_TYPE_CCCD,&cccd)==0);
 ble_store_key ck{};ck.cccd.peer_addr=connected;
 assert(ble_store_read(BLE_STORE_OBJ_TYPE_CCCD,&ck,&out)==BLE_HS_ENOENT);
 char name[16]{};schema_key(connected,name);disk[{kSchemaNamespace,name}]={NVS_TYPE_U8,{2}};
 assert(ble_store_read(BLE_STORE_OBJ_TYPE_CCCD,&ck,&out)==0&&out.cccd.chr_val_handle==25);
 disk[{kSchemaNamespace,name}].bytes={1};
 assert(ble_store_read(BLE_STORE_OBJ_TYPE_CCCD,&ck,&out)==BLE_HS_ENOENT);
 // Metadata-only, half-pair and fully-written pending are all quarantined at boot.
 for(unsigned phase=0;phase<3;++phase){
  reset();Backend pending;Backend::instance_=&pending;AssociationStore store{raw_read};
  auto peer=association_peer(connected);
  assert(store.write(peer,{S::kPending,detail::BondClass::kStandaloneMouseJustWorks})==0);
  value=key_value();if(phase>0)assert(raw_write(BLE_STORE_OBJ_TYPE_OUR_SEC,&value)==0);
  if(phase>1)assert(raw_write(BLE_STORE_OBJ_TYPE_PEER_SEC,&value)==0);
  before=disk;assert(validate_complete_associations()!=0&&disk==before);
  assert(ble_store_read(BLE_STORE_OBJ_TYPE_OUR_SEC,&key,&out)==BLE_HS_ESTORE_FAIL);
  assert(ble_store_write(BLE_STORE_OBJ_TYPE_PEER_SEC,&value)==BLE_HS_ESTORE_FAIL&&disk==before);
 }
 // Every actual adapter I/O interruption either leaves inert pending/absence,
 // or a complete pair with durable exact provenance. It never auto-repairs.
 reset();Backend count;both(count);const int boundaries=calls;
 for(bool side:{false,true})for(int point=0;point<boundaries;++point){
  reset();Backend fault;Backend::instance_=&fault;cut=point;after=side;
  value=key_value();(void)ble_store_write(BLE_STORE_OBJ_TYPE_OUR_SEC,&value);
  (void)ble_store_write(BLE_STORE_OBJ_TYPE_PEER_SEC,&value);
  cut=-1;assert(handles.empty()&&live_iterators==0);
  auto association=read_association(connected);before=disk;
  auto status=validate_complete_associations();assert(disk==before);
  if(association.record.state==S::kComplete)assert(status==0);
  else if(association.record.state==S::kPending)assert(status!=0);
  else assert(ram.empty());
 }
 // A delegate failure, even after RAM/durable mutation, inhibits the next
 // ignored-error SMP write and preserves pending provenance.
 for(int kind=1;kind<=3;++kind){
  reset();Backend failed;Backend::instance_=&failed;value=key_value();raw_fail=kind;
  assert(ble_store_write(BLE_STORE_OBJ_TYPE_OUR_SEC,&value)==77);
  assert(read_association(connected).record.state==S::kPending);
  before=disk;raw_fail=0;
  assert(ble_store_write(BLE_STORE_OBJ_TYPE_PEER_SEC,&value)==BLE_HS_ESTORE_FAIL);
  assert(raw_writes==1&&disk==before&&store_failures==1);
 }
 // Same-connection repeat writes after completion cannot replace retained keys.
 reset();Backend closed;both(closed);before=disk;value=key_value();value.sec.synthetic_ltk[0]=99;
 assert(ble_store_write(BLE_STORE_OBJ_TYPE_OUR_SEC,&value)==BLE_HS_ESTORE_FAIL&&disk==before);
 // No valid active connection snapshot: reject before any persistent mutation.
 for(int invalid=0;invalid<3;++invalid){
  reset();Backend missing;Backend::instance_=&missing;
  if(invalid==0)missing.host_identity_valid_=false;
  if(invalid==1)missing.host_connection_identity_.val[0]^=1;
  if(invalid==2)missing.active_host_connection_=0;
  value=key_value();assert(ble_store_write(BLE_STORE_OBJ_TYPE_OUR_SEC,&value)==BLE_HS_ESTORE_FAIL);
  assert(disk.empty()&&ram.empty());
 }
 // Administrative reads and pre-pair absence proof acquire the host lock even
 // when the selected profile is incompatible, without changing SMP filtering.
 reset();Backend inventory;Backend::instance_=&inventory;
 AssociationStore synchronized{read_inventory_security};
 assert(synchronized.prove_security_absent(association_peer(connected))==1);
 assert(!host_locked);
 // No new record may exceed the three-record namespace bound.
 reset();AssociationStore store{raw_read};
 for(unsigned i=1;i<=3;++i){auto address=connected;address.val[0]=i;
  assert(store.write(association_peer(address),{S::kPending,detail::BondClass::kStandaloneMouseJustWorks})==0);}
 auto fourth=connected;fourth.val[0]=9;before=disk;
 assert(store.write(association_peer(fourth),{S::kPending,detail::BondClass::kStandaloneMouseJustWorks})!=0&&disk==before);
 // A completed sidecar without both durable keys fails startup validation.
 reset();disk=complete;disk.erase({kNimbleStoreNamespace,"peer_sec_1"});assert(validate_complete_associations()!=0);
 std::cout<<"PASS: actual association NVS/store wrappers; I/O cuts="<<boundaries<<'\n';
}
'''

def main():
    source = (ROOT / 'firmware/components/ble_transport/ble_transport.cpp').read_text()
    signatures = (
        'ble_addr_t nimble_identity(', 'bool same_identity(const ble_addr_t',
        'bool valid_identity(const ble_addr_t', 'ble_security::StoredSecurityRecord security_record(',
        'void schema_key(', 'bool parse_schema_key(', 'bool security_slot_key(',
        'esp_err_t validate_nimble_security_key_layout(', 'esp_err_t read_schema_revision(',
        'detail::AssociationPeer association_peer(', 'ble_addr_t association_address(',
        'detail::AssociationRead read_association(', 'int validate_association_namespace(',
        'int read_inventory_security(', 'struct AssociationStore {', 'int validate_complete_associations(',
        'int Backend::read_security_raw(', 'bool Backend::compatible_association(',
        'int Backend::store_read(', 'int Backend::store_write(')
    extracted = '\n'.join(body(source, s) + (';' if s.startswith('struct') else '') for s in signatures)
    with tempfile.TemporaryDirectory(prefix='bond-association-nvs-') as directory:
        path = Path(directory)
        (path/'test.cpp').write_text(PRE + EXTRA + extracted + POST)
        command = [os.environ.get('CXX','c++'),'-std=c++20','-Wall','-Wextra','-Werror','-pedantic']
        for include in ('ble_transport','ble_transport/include','hid_capability/include',
                        'ble_fixture_profile/include','ble_security/include','ble_lifecycle/include'):
            command += ['-I'+str(ROOT/'firmware/components'/include)]
        command += [str(path/'test.cpp'),str(ROOT/'firmware/components/ble_security/ble_security.cpp'),'-o',str(path/'test')]
        subprocess.run(command,check=True)
        subprocess.run([str(path/'test')],check=True)

if __name__ == '__main__': main()
