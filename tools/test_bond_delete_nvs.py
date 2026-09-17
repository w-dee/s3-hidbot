#!/usr/bin/env python3
"""Compile the actual product NVS adapter with a synthetic faulting NVS API.

No SDK/hardware/network dependency; secret fields in fixtures are synthetic.
Extraction deliberately fails if the production seam disappears or changes.
"""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]

def body(source, signature):
    start = source.index(signature)
    opening = source.index('{', start)
    level, end = 1, opening + 1
    while level:
        level += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

PRE = r'''
#include "bond_delete_transaction.hpp"
#include <cassert>
#include <map>
#include <string>
#include <vector>
#include <iostream>
namespace detail = ble_transport::detail;
using esp_err_t = int;
using nvs_handle_t = int;
enum { ESP_OK=0, ESP_ERR_NVS_NOT_FOUND=101, ESP_ERR_NVS_INVALID_LENGTH=102,
       ESP_ERR_INVALID_STATE=103, NVS_READONLY=0, NVS_READWRITE=1,
       NVS_TYPE_ANY=255, NVS_TYPE_BLOB=66, NVS_TYPE_U8=1 };
struct ble_addr_t { uint8_t type; uint8_t val[6]; };
constexpr char kSchemaKeyHex[]="0123456789abcdef";
constexpr char kSchemaNamespace[]="hid_schema";
constexpr char kNimbleStoreNamespace[]="nimble_bond";
namespace secure_memory { void zero(void*p,size_t n){std::memset(p,0,n);} }
using Key=std::pair<std::string,std::string>;
struct Value { int type;std::vector<uint8_t> bytes;bool operator==(const Value&)const=default;};
using Disk=std::map<Key,Value>;
Disk disk;std::map<int,std::string> handles;int next_handle=1;
int calls=0,cut=-1;bool after=false;
bool fail(){return calls++==cut;}
int nvs_open(const char*ns,int mode,int*h){
 if(fail())return 77;
 bool exists=false;for(auto&[key,v]:disk){(void)v;exists=exists||key.first==ns;}
 if(!exists&&mode==NVS_READONLY)return ESP_ERR_NVS_NOT_FOUND;
 *h=next_handle++;handles[*h]=ns;return 0;
}
void nvs_close(int h){assert(handles.erase(h)==1);}
int nvs_get_blob(int h,const char*k,void*p,size_t*n){
 if(fail())return 77;
 auto it=disk.find({handles.at(h),k});if(it==disk.end())return ESP_ERR_NVS_NOT_FOUND;
 if(it->second.type!=NVS_TYPE_BLOB)return ESP_ERR_INVALID_STATE;
 if(*n<it->second.bytes.size()){*n=it->second.bytes.size();return ESP_ERR_NVS_INVALID_LENGTH;}
 *n=it->second.bytes.size();std::memcpy(p,it->second.bytes.data(),*n);return 0;
}
int nvs_get_u8(int h,const char*k,uint8_t*p){
 if(fail())return 77;
 auto it=disk.find({handles.at(h),k});if(it==disk.end())return ESP_ERR_NVS_NOT_FOUND;
 if(it->second.type!=NVS_TYPE_U8||it->second.bytes.size()!=1)return ESP_ERR_INVALID_STATE;
 *p=it->second.bytes[0];return 0;
}
int nvs_set_blob(int h,const char*k,const void*p,size_t n){
 bool error=fail();if(error&&!after)return 77;
 auto b=static_cast<const uint8_t*>(p);disk[{handles.at(h),k}]={NVS_TYPE_BLOB,{b,b+n}};return error?77:0;
}
int nvs_commit(int){return fail()?77:0;}
int nvs_erase_key(int h,const char*k){
 bool error=fail();if(error&&!after)return 77;
 bool erased=disk.erase({handles.at(h),k});return error?77:erased?0:ESP_ERR_NVS_NOT_FOUND;
}
struct Iterator {std::vector<Key>keys;size_t index=0;};
using nvs_iterator_t=Iterator*;
int live_iterators=0;
struct nvs_entry_info_t {char key[16]{};int type=0;};
int nvs_entry_find_in_handle(int h,int,nvs_iterator_t*out){
 if(fail())return 77;
 std::vector<Key>keys;for(auto&[key,value]:disk){(void)value;if(key.first==handles.at(h))keys.push_back(key);}
 if(keys.empty())return ESP_ERR_NVS_NOT_FOUND;
 *out=new Iterator{keys};++live_iterators;return 0;
}
void nvs_release_iterator(nvs_iterator_t i){if(i){delete i;--live_iterators;}}
int nvs_entry_next(nvs_iterator_t*i){
 if(fail())return 77;
 if(++(*i)->index==(*i)->keys.size()){nvs_release_iterator(*i);*i=nullptr;return ESP_ERR_NVS_NOT_FOUND;}return 0;
}
int nvs_entry_info(nvs_iterator_t i,nvs_entry_info_t*out){
 if(fail())return 77;
 auto key=i->keys[i->index];std::strncpy(out->key,key.second.c_str(),15);out->type=disk.at(key).type;return 0;
}
'''

POST = r'''
detail::StoreIdentity id(unsigned p){detail::StoreIdentity i{.type=1};i.value[0]=p;return i;}
std::string sk(unsigned peer){char k[16];schema_key(nimble_identity(id(peer)),k);return k;}
Disk fixture(){Disk result;
 for(unsigned peer=1;peer<=3;peer++){
  for(auto&c:detail::kDeleteCategories){std::vector<uint8_t>b(c.size);b[c.identity_offset]=1;b[c.identity_offset+1]=peer;
   result[{kNimbleStoreNamespace,std::string(c.prefix)+std::to_string(peer)}]={NVS_TYPE_BLOB,b};}
  result[{kSchemaNamespace,sk(peer)}]={NVS_TYPE_U8,{1}};
 }
 result[{"other","keep"}]={NVS_TYPE_U8,{42}};return result;
}
bool target(const Key&k){return k==Key{kSchemaNamespace,sk(3)}||(k.first==kNimbleStoreNamespace&&k.second.back()=='3');}
Disk keep(const Disk&d){Disk result;for(auto&[k,v]:d)if(!target(k)&&k.first!=detail::kDeleteNamespace)result[k]=v;return result;}
bool absent(){for(auto&[k,v]:disk){(void)v;if(target(k))return false;}return true;}
int run(){PersistentDeleteStore store;return detail::run_journaled_removal(store,id(3),[]{return 0;},[]{return 0;});}
void reset(const Disk&d){assert(handles.empty()&&live_iterators==0);disk=d;calls=0;cut=-1;after=false;}
int main(){const auto initial=fixture();const auto preserved=keep(initial);
 reset(initial);assert(run()==0&&absent()&&keep(disk)==preserved);const int boundaries=calls;int cases=1;
 for(bool side:{false,true})for(int point=0;point<boundaries;point++){
  reset(initial);cut=point;after=side;assert(run()!=0);assert(keep(disk)==preserved);
  assert(handles.empty()&&live_iterators==0);
  bool pending=disk.count({detail::kDeleteNamespace,detail::kDeleteKey});
  if(!pending)assert(disk==initial||absent());
  cut=-1;calls=0;PersistentDeleteStore store;assert(detail::resume_exact_deletion(store)==0);
  if(pending){assert(absent());}assert(keep(disk)==preserved);++cases;
 }
 for(int malformed=0;malformed<3;malformed++){
  reset(initial);auto key=Key{kNimbleStoreNamespace,"cccd_sec_3"};
  if(malformed==0)disk[key].type=NVS_TYPE_U8;
  if(malformed==1)disk[key].bytes.resize(99);
  if(malformed==2)disk[{kNimbleStoreNamespace,"cccd_sec_16"}]=disk[key];
  auto before=disk;assert(run()!=0&&disk==before);++cases;
 }
 reset(initial);PersistentDeleteStore store;assert(store.verify_schema_absent(id(3))!=0);++cases;
 std::cout<<"PASS: actual NVS adapter cases="<<cases<<" I/O boundaries="<<boundaries<<'\n';
}
'''

def main():
    source = (ROOT / 'firmware/components/ble_transport/ble_transport.cpp').read_text()
    signatures = ('ble_addr_t nimble_identity(', 'void schema_key(',
                  'esp_err_t read_schema_revision(', 'esp_err_t delete_schema_revision(',
                  'esp_err_t delete_schema_revision_verified(', 'struct PersistentDeleteStore {')
    extracted = '\n'.join(body(source, s) + (';' if s.startswith('struct') else '') for s in signatures)
    with tempfile.TemporaryDirectory(prefix='bond-delete-nvs-test-') as directory:
        path = Path(directory)
        (path/'test.cpp').write_text(PRE + extracted + POST)
        command = [os.environ.get('CXX','c++'),'-std=c++20','-Wall','-Wextra','-Werror','-pedantic']
        for include in ('ble_transport', 'hid_capability/include',
                        'ble_fixture_profile/include',
                        'ble_security/include', 'ble_lifecycle/include'):
            command += ['-I'+str(ROOT/'firmware/components'/include)]
        subprocess.run(command+[str(path/'test.cpp'),'-o',str(path/'test')],check=True)
        subprocess.run([str(path/'test')],check=True)

if __name__=='__main__':main()
