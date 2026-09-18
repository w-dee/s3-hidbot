"""Frozen subcheck for the official v0.4.0 FNK0099 qualification attempt."""
from pathlib import Path
import dataclasses, hashlib, json, os, re, signal, subprocess, sys, time, traceback
ROOT=Path(__file__).resolve().parent
sys.path[:0]=[str(ROOT/'host/src'),str(ROOT/'tools'),'/usr/lib/python3/dist-packages']
import dbus
from hidbot.client import Client
from hidbot.serial_transport import PySerialTransport
from hidbot.protocol import validate_system_info
from hidbot.provisioning import stage_and_inspect_firmware_bundle
from hidbot.firmware_verification import compare_firmware_identity
from ble_cleanup_rehearsal import _headless,_serial_port

mode,artifact_name,evidence_name=sys.argv[1:4]
evidence=Path(evidence_name)
result={'classification':'OFFICIAL_FNK0099_PHYSICAL_QUALIFICATION_SUBCHECK','qualification_attempt_consumed':True,
        'mode':mode,'result':'RUNNING','events':[],'heap':[],'uart_diagnostics':[]}
client=None;adapter_properties=None;original_power=None

def plain(value):
    if dataclasses.is_dataclass(value):return plain(dataclasses.asdict(value))
    if isinstance(value,dict):return {str(k):plain(v) for k,v in value.items()}
    if isinstance(value,(tuple,list)):return [plain(v) for v in value]
    if hasattr(value,'value'):return value.value
    return value

def save():
    temp=evidence.with_suffix('.tmp');temp.write_text(json.dumps(plain(result),indent=2)+'\n');temp.replace(evidence)

def note(kind,value):
    result['events'].append({'kind':kind,'time_monotonic':time.monotonic(),'value':plain(value)});save()

def logs(data):
    text=data.decode('utf-8','replace').strip()
    if 'heap checkpoint=' in text:result['heap'].append(text)
    elif re.search(r'(panic|assert|error|fail|reject|reset|abort|Guru)',text,re.I):result['uart_diagnostics'].append(text)

def fresh(label):
    global client
    if client is not None:client.close()
    transport=PySerialTransport(_serial_port(None),115200);transport.open()
    client=Client(transport,timeout=1.3,max_attempts=1,log_sink=logs)
    hello=client.connect();note(label,{'boot_id':hello.boot_id})
    return client

def poll(function,predicate,seconds=12):
    end=time.monotonic()+seconds;last=None
    while time.monotonic()<end:
        client.ping()
        last=function()
        if predicate(last):return last
        time.sleep(.08)
    raise RuntimeError('bounded poll failed: '+str(plain(last)))

def hidden():
    client.ble_disable()
    return poll(client.ble_exposure_status,lambda x:x.desired.value=='hidden' and x.observed.value in ('idle','uninitialized') and not x.connected and not x.advertising and not x.recovery_required)

def route_none():
    route=client.hid_route_status()
    return route.desired.value=='none' and route.active.value=='none' and route.transition=='stable'

import threading
import dbus.service
from dbus.mainloop.glib import DBusGMainLoop
from gi.repository import GLib
from hidbot.errors import RemoteError
from hidbot.sequence import SequenceBuilder,MouseButton
from ble_cleanup_rehearsal import _paired_target,Observer,_normalize_address,_ancestor_text,_read,_capability,KEY_F24,BTN_LEFT,REL_X,EV_KEY,EV_REL
DBusGMainLoop(set_as_default=True)
dbus.mainloop.glib.threads_init()
observer=None;loop=None;agent=None;manager=None;target_path=None;discovery=False
result['agent_requests']=[]

class Rejected(dbus.DBusException):
    _dbus_error_name='org.bluez.Error.Rejected'

class Agent(dbus.service.Object):
    def check(self,path,kind):
        result['agent_requests'].append(kind)
        if str(path)!=target_path:raise Rejected('only exact fixture is authorized')
    @dbus.service.method('org.bluez.Agent1',in_signature='',out_signature='')
    def Release(self):pass
    @dbus.service.method('org.bluez.Agent1',in_signature='o',out_signature='s')
    def RequestPinCode(self,device):self.check(device,'RequestPinCode');raise Rejected('unexpected input')
    @dbus.service.method('org.bluez.Agent1',in_signature='o',out_signature='u')
    def RequestPasskey(self,device):self.check(device,'RequestPasskey');raise Rejected('unexpected input')
    @dbus.service.method('org.bluez.Agent1',in_signature='ou',out_signature='')
    def RequestConfirmation(self,device,passkey):self.check(device,'RequestConfirmation');raise Rejected('unexpected input')
    @dbus.service.method('org.bluez.Agent1',in_signature='os',out_signature='')
    def DisplayPinCode(self,device,pincode):self.check(device,'DisplayPinCode');raise Rejected('unexpected input')
    @dbus.service.method('org.bluez.Agent1',in_signature='ouq',out_signature='')
    def DisplayPasskey(self,device,passkey,entered):self.check(device,'DisplayPasskey');raise Rejected('unexpected input')
    @dbus.service.method('org.bluez.Agent1',in_signature='o',out_signature='')
    def RequestAuthorization(self,device):self.check(device,'RequestAuthorization')
    @dbus.service.method('org.bluez.Agent1',in_signature='os',out_signature='')
    def AuthorizeService(self,device,uuid):
        self.check(device,'AuthorizeService')
        if str(uuid)!='00001812-0000-1000-8000-00805f9b34fb':raise Rejected('unexpected service')
    @dbus.service.method('org.bluez.Agent1',in_signature='',out_signature='')
    def Cancel(self):result['agent_requests'].append('Cancel')

def objects():return dbus.Interface(bus.get_object('org.bluez','/'),'org.freedesktop.DBus.ObjectManager').GetManagedObjects()
def device_props():return objects().get(dbus.ObjectPath(target_path),{}).get('org.bluez.Device1',{})
def async_call(proxy,method,seconds=25):
    state={'done':False}
    def good(*args):state['done']=True
    def bad(exc):state.update(done=True,error=str(exc))
    getattr(proxy,method)(reply_handler=good,error_handler=bad,timeout=seconds)
    end=time.monotonic()+seconds+1
    while not state['done'] and time.monotonic()<end:
        client.ping();time.sleep(.1)
    assert state['done'],method+' timed out'
    if state.get('error'):raise RuntimeError(method+': '+state['error'])

def secure(authenticated):
    value=poll(client.ble_pairing_status,lambda x:x.connected and x.encrypted and x.bonded and x.authenticated==authenticated and x.key_size==16,seconds=20)
    note('security',value);assert value.secure_connections
    return value

def inspect_gatt():
    managed=objects();prefix=target_path+'/'
    chars=[(str(path),v['org.bluez.GattCharacteristic1']) for path,v in managed.items() if str(path).startswith(prefix) and 'org.bluez.GattCharacteristic1' in v]
    maps=[path for path,v in chars if str(v['UUID'])=='00002a4b-0000-1000-8000-00805f9b34fb'];assert len(maps)==1
    reportmap=bytes(dbus.Interface(bus.get_object('org.bluez',maps[0]),'org.bluez.GattCharacteristic1').ReadValue(dbus.Dictionary({},signature='sv')))
    refs=[]
    for path,v in managed.items():
        if str(path).startswith(prefix) and 'org.bluez.GattDescriptor1' in v and str(v['org.bluez.GattDescriptor1']['UUID'])=='00002908-0000-1000-8000-00805f9b34fb':
            refs.append(list(dbus.Interface(bus.get_object('org.bluez',path),'org.bluez.GattDescriptor1').ReadValue(dbus.Dictionary({},signature='sv'))))
    inputs=[path for path,v in chars if str(v['UUID'])=='00002a4d-0000-1000-8000-00805f9b34fb']
    note('gatt',{'map_length':len(reportmap),'map_sha256':hashlib.sha256(reportmap).hexdigest(),'report_references':refs,'report_characteristic_count':len(inputs)})
    assert len(reportmap)==69 and hashlib.sha256(reportmap).hexdigest()=='c2fb165ffe3f84fc4160b013e15914dffbdecbc330c3c315051dc6922262e924'
    assert refs==[[2,1]] and len(inputs)==1

def open_mouse(address):
    exact=_normalize_address(address);mice=[];keyboards=[]
    for node in Path('/sys/class/input').glob('event[0-9]*'):
        resolved=node.resolve();uniq=_ancestor_text(resolved,'uniq') or _read(node/'device/uniq')
        if not uniq or _normalize_address(uniq)!=exact:continue
        if _capability(node,'key',KEY_F24):keyboards.append(node.name)
        if _capability(node,'key',BTN_LEFT) and _capability(node,'rel',REL_X):mice.append(Path('/dev/input')/node.name)
    assert len(mice)==1 and not keyboards,{'mice':len(mice),'keyboards':len(keyboards)}
    value=Observer(mice[0],'mouse');value.open();note('evdev',{'mouse_count':1,'keyboard_count':0});return value

def collect(seconds):
    client.ping()
    end=time.monotonic()+seconds;events=[]
    while time.monotonic()<end:
        for event in observer.read(min(.04,max(0,end-time.monotonic()))):
            if event.event_type in (EV_KEY,EV_REL):events.append([event.event_type,event.code,event.value])
    return events

def cleanup_attempt():
    attempt=client.session
    client.ping()
    note('release_all',client.release_all());assert client.session==attempt
    tail=collect(.25);assert tail==[] and observer.held_count()==0
    note('all_up_quiet',{'held':0,'quiet_seconds':.25,'events':tail,'same_attempt_session':client.session==attempt})
    note('intentional_route_none',client.hid_route_set('none'))
    observer.close();fresh('post_retirement_session')
    poll(client.hid_route_status,lambda x:x.active.value=='none' and x.desired.value=='none' and x.transition=='stable',seconds=7)
    note('hidden',hidden())

try:
    save();assert _headless()['safe'];fresh('setup_session');assert route_none()
    with stage_and_inspect_firmware_bundle(Path(artifact_name)) as bundle:
        comparison=compare_firmware_identity(bundle.artifact_identity,client.capabilities,validate_system_info(client.info(),capabilities=client.capabilities))
        note('identity',comparison);assert comparison.match
    before=client.ble_bond_list();note('bonds_before',before);assert before.healthy
    before_ids=tuple(sorted(x.bond_id for x in before.bonds))
    assert client.ble_profile_status().selected.value in ('mouse_simulated_sleep_v1','mouse_host_initiated_security')
    address,retained=_paired_target();note('bluez_before',retained)
    bus=dbus.SystemBus();managed=objects();adapters=[str(p) for p,v in managed.items() if 'org.bluez.Adapter1' in v];assert len(adapters)==1
    target_path=adapters[0]+'/dev_'+address.replace(':','_')
    assert all(str(p)==target_path or not v['org.bluez.Device1'].get('Connected') for p,v in managed.items() if 'org.bluez.Device1' in v)
    adapter_properties=dbus.Interface(bus.get_object('org.bluez',adapters[0]),'org.freedesktop.DBus.Properties')
    adapter=dbus.Interface(bus.get_object('org.bluez',adapters[0]),'org.bluez.Adapter1')
    original_power=bool(adapter_properties.Get('org.bluez.Adapter1','Powered'))
    if not original_power:adapter_properties.Set('org.bluez.Adapter1','Powered',dbus.Boolean(True))
    loop=GLib.MainLoop();thread=threading.Thread(target=loop.run,daemon=True);thread.start()
    agent=Agent(bus,'/s3_hidbot_engineering_agent')
    manager=dbus.Interface(bus.get_object('org.bluez','/org/bluez'),'org.bluez.AgentManager1')
    manager.RegisterAgent('/s3_hidbot_engineering_agent','NoInputNoOutput');manager.RequestDefaultAgent('/s3_hidbot_engineering_agent')
    # The preceding failed capsule already identified and deleted only this
    # exact fixture peer. Do not recreate its old strict bond just for setup.
    assert len(before_ids)==1,'exactly one preflight-created target bond is required'
    target_bond=before_ids[0]
    expected_others=()
    note('setup_release',client.release_all());note('setup_hidden',hidden())
    if target_bond in before_ids:
        note('mutation',{'command':'ble.bond.remove','bond_id':target_bond})
        note('exact_bond_removal',client.ble_bond_remove(target_bond))
    remaining=client.ble_bond_list();note('preserved_others',remaining)
    assert remaining.healthy and tuple(sorted(x.bond_id for x in remaining.bonds))==expected_others
    expected_after=tuple(sorted((*expected_others,target_bond)))
    note('mutation','BlueZ RemoveDevice exact fixture')
    adapter.RemoveDevice(dbus.ObjectPath(target_path))
    note('select_host_security',client.ble_profile_select('mouse_host_initiated_security'))
    status=poll(client.ble_profile_status,lambda x:x.transition!='initializing',seconds=17);note('selected',status)
    assert status.transition=='stable' and status.active.value=='mouse_host_initiated_security'
    exposure=client.ble_exposure_status();assert not exposure.advertising and not exposure.connected and exposure.desired.value=='hidden'
    note('host_security_enable',client.ble_enable())
    adapter.SetDiscoveryFilter(dbus.Dictionary({'Transport':dbus.String('le')},signature='sv'))
    adapter.StartDiscovery();discovery=True
    poll(lambda:target_path in objects(),lambda x:x,seconds=12)
    from evidence_pipeline import package_request
    from q8_capture import capture_pair
    note('mutation','Pair exact fixture (host/central initiated, NoInputNoOutput)')
    device=dbus.Interface(bus.get_object('org.bluez',target_path),'org.bluez.Device1')
    receipt=capture_pair(package_request(), lambda: async_call(device,'Pair',seconds=30))
    note('hci_security_order',{'capture_preceded_host_pair_invocation':True,
        'pairing_request_count':receipt['counts']['pairing_requests'],
        'pairing_response_count':receipt['counts']['pairing_responses'],
        'peripheral_security_request_count':receipt['counts']['security_requests'],
        'capture_id':receipt['request']['capture_id'], 'raw_sha256':receipt['raw']['sha256']})
    adapter.StopDiscovery();discovery=False
    dbus.Interface(bus.get_object('org.bluez',target_path),'org.freedesktop.DBus.Properties').Set('org.bluez.Device1','Trusted',dbus.Boolean(True))
    note('after_pair_pairing',client.ble_pairing_status());note('after_pair_exposure',client.ble_exposure_status())
    assert not client.ble_exposure_status().recovery_required,'firmware fault after Pair'
    if not device_props().get('Connected'):async_call(device,'Connect')
    secure(False)
    poll(lambda:bool(device_props().get('ServicesResolved')),lambda x:x,seconds=15)
    inspect_gatt();client.ping()
    poll(lambda:[node for node in Path('/sys/class/input').glob('event[0-9]*') if _read(node/'device/uniq') and _normalize_address(_read(node/'device/uniq'))==_normalize_address(address)],lambda x:bool(x),seconds=8)
    observer=open_mouse(address)
    assert collect(.2)==[] and observer.held_count()==0
    note('route_ble',client.hid_route_set('ble'));fresh('attempt_session')
    assert client.hid_route_status().ready
    attempt=client.session
    for label,call in [('keyboard',lambda:client.keyboard_report(0,[115])),('mixed_sequence',lambda:client.sequence_start(SequenceBuilder().mouse_press(MouseButton.LEFT).key_press(115).key_release(115).mouse_release(MouseButton.LEFT).build()))]:
        try:call();raise AssertionError('unsupported operation unexpectedly accepted')
        except RemoteError as exc:assert exc.code=='HID_UNSUPPORTED_OPERATION';note('unsupported_'+label,{'code':exc.code})
        assert collect(.15)==[] and observer.held_count()==0 and client.session==attempt
    events=[]
    note('mouse_down',client.mouse_report(1,0,0,0,0));events+=collect(.18)
    note('mouse_up',client.mouse_report(0,0,0,0,0));events+=collect(.18)
    note('mouse_move',client.mouse_report(0,1,0,0,0));events+=collect(.18)
    note('mouse_events',events);assert events==[[EV_KEY,BTN_LEFT,1],[EV_KEY,BTN_LEFT,0],[EV_REL,REL_X,1]]
    assert client.session==attempt
    note('held_mouse_down',client.mouse_report(1,0,0,0,0))
    assert collect(.18)==[[EV_KEY,BTN_LEFT,1]] and observer.held_count()==1
    client.ping();released=client.release_all();note('held_release',released)
    assert released.keyboard=='already_up' and released.mouse=='submitted' and client.session==attempt
    assert collect(.18)==[[EV_KEY,BTN_LEFT,0]] and observer.held_count()==0
    retained=client.hid_route_status();note('route_after_held_release',retained)
    assert retained.active.value=='ble' and retained.transition=='stable' and retained.ready
    note('post_release_mouse_move',client.mouse_report(0,1,0,0,0))
    assert collect(.18)==[[EV_REL,REL_X,1]] and client.session==attempt
    bonds=client.ble_bond_list();note('mouse_bonds',bonds)
    assert bonds.healthy and tuple(sorted(x.bond_id for x in bonds.bonds))==expected_after
    ours=[x for x in bonds.bonds if x.connected];assert len(ours)==1 and ours[0].schema_revision==8 and ours[0].schema_current
    assert not any(x in result['agent_requests'] for x in ('RequestPasskey','DisplayPasskey','RequestPinCode','DisplayPinCode','RequestConfirmation'))
    cleanup_attempt()
    note('retained_enable',client.ble_enable())
    if not device_props().get('Connected'):async_call(device,'Connect')
    secure(False);assert route_none()
    poll(lambda:[node for node in Path('/sys/class/input').glob('event[0-9]*') if _read(node/'device/uniq') and _normalize_address(_read(node/'device/uniq'))==_normalize_address(address)],lambda x:bool(x),seconds=8)
    observer=open_mouse(address);assert collect(.2)==[] and observer.held_count()==0
    note('retained_explicit_route',client.hid_route_set('ble'));fresh('retained_attempt_session');assert client.hid_route_status().ready
    note('retained_mouse',client.mouse_report(0,1,0,0,0));assert collect(.18)==[[EV_REL,REL_X,1]]
    cleanup_attempt()
    note('final_bonds',client.ble_bond_list());note('final_route',client.hid_route_status());note('final_ble',client.ble_exposure_status());note('final_profile',client.ble_profile_status())
    result.update(result='PASS',final_held_state='ALL_UP',sequence_active=False)
except BaseException as exc:
    result.update(result='FAIL',failure={'type':type(exc).__name__,'message':str(exc),'traceback':traceback.format_exc()})
    try:
        fresh('emergency_cleanup_session');note('emergency_release',client.release_all())
        note('emergency_route',client.hid_route_set('none'));fresh('emergency_after_route')
        note('emergency_disable',client.ble_disable());note('emergency_state',client.ble_exposure_status())
    except BaseException as cleanup:result['cleanup_failure']={'type':type(cleanup).__name__,'message':str(cleanup)}
finally:
    if observer is not None:observer.close()
    if client is not None:client.close()
    if discovery:
        try:adapter.StopDiscovery()
        except Exception:pass
    if manager is not None and agent is not None:
        try:manager.UnregisterAgent('/s3_hidbot_engineering_agent')
        except Exception:pass
    if loop is not None:loop.quit()
    if adapter_properties is not None and original_power is not None:
        try:adapter_properties.Set('org.bluez.Adapter1','Powered',dbus.Boolean(original_power));note('adapter_power_restored',original_power)
        except BaseException as exc:result['adapter_restore_failure']=str(exc);result['result']='FAIL'
    save()
print(json.dumps({'result':result['result'],'classification':result['classification'],'failure':result.get('failure'),'heap':result['heap']}),flush=True)
raise SystemExit(0 if result['result']=='PASS' else 1)
