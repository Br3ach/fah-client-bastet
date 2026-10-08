# /******************************************************************************\
#
#                   This file is part of the Folding@home Client.
#
#           The fah-client runs Folding@home protein folding simulations.
#                     Copyright (c) 2001-2026, foldingathome.org
#                                All rights reserved.
#
#        This program is free software; you can redistribute it and/or modify
#        it under the terms of the GNU General Public License as published by
#         the Free Software Foundation; either version 3 of the License, or
#                        (at your option) any later version.
#
#          This program is distributed in the hope that it will be useful,
#           but WITHOUT ANY WARRANTY; without even the implied warranty of
#           MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#                    GNU General Public License for more details.
#
#      You should have received a copy of the GNU General Public License along
#      with this program; if not, write to the Free Software Foundation, Inc.,
#            51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
#
#                   For information regarding this software email:
#                                  Joseph Coffland
#                           joseph@cauldrondevelopment.com
#
# \******************************************************************************/

from pathlib import Path
import sqlite3,json,subprocess,time,socket,base64,os,struct,datetime
import argparse,tempfile
parser=argparse.ArgumentParser(description='Test real client configuration with disposable paused data')
parser.add_argument('--client',required=True,type=Path)
parser.add_argument('--port',type=int,default=17497)
parser.add_argument('--expect-homogeneous',action='store_true',help='Require native General-mode managed allocation on this test machine')
args=parser.parse_args()
# Test-only process termination is safe because this profile has no WUs.
temporary=tempfile.TemporaryDirectory(prefix='fah-api-smoke-')
root=Path(temporary.name)
executable=str(args.client.resolve())
def frame_recv(s):
 def read(n):
  b=b''
  while len(b)<n:
   x=s.recv(n-len(b));assert x;b+=x
  return b
 h=read(2);n=h[1]&127
 if n==126:n=struct.unpack('!H',read(2))[0]
 elif n==127:n=struct.unpack('!Q',read(8))[0]
 return json.loads(read(n))
def send(s,obj):
 b=json.dumps(obj).encode();mask=os.urandom(4);head=b'\x81'+(bytes([128|len(b)]) if len(b)<126 else b'\xfe'+struct.pack('!H',len(b)))
 s.sendall(head+mask+bytes(v^mask[i%4] for i,v in enumerate(b)))
last_change_second=0
def change_time():
 global last_change_second
 # Client configuration requests must have strictly increasing timestamps.
 while int(time.time()) <= last_change_second:time.sleep(.02)
 last_change_second=int(time.time())
 return datetime.datetime.fromtimestamp(last_change_second,datetime.timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ')

def open_websocket(port):
 probe=socket.create_connection(('127.0.0.1',port),timeout=5)
 try:
  key=base64.b64encode(os.urandom(16)).decode()
  probe.sendall(('GET /api/websocket HTTP/1.1\r\nHost: 127.0.0.1:'+str(port)+'\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: '+key+'\r\nSec-WebSocket-Version: 13\r\nOrigin: http://127.0.0.1:'+str(port)+'\r\n\r\n').encode())
  response=b''
  while not response.endswith(b'\r\n\r\n'):response+=probe.recv(1)
  assert b'101' in response,response
  return probe,frame_recv(probe)
 except:
  probe.close();raise

def authoritative_snapshot(port):
 probe,snapshot=open_websocket(port)
 probe.close()
 return snapshot

def configuration_state(snapshot):
 affinity=snapshot['info']['cpu_affinity']
 return {'groups':{name:group['config'] for name,group in snapshot['groups'].items()},
         'allocation':{key:affinity.get(key) for key in
          ['managed','runtime_fallback','runtime_fallback_reason','group_allocations','gpu_helper_blocked_groups']}}

def verify_allocation(snapshot,requested):
 affinity=snapshot['info']['cpu_affinity']
 # Exercise the compiled status builder/serializer through the real API.
 assert set(affinity)=={'capability','available','topology_generation','allocation_generation',
  'class_selection','effective_classes','smt_topology','managed','runtime_fallback',
  'runtime_fallback_reason','gpu_cpu_reservation','allocatable','physical_cpus',
  'performance1_core_threads','performance_levels','group_allocations','unit_allocations',
  'gpu_helper_blocked_groups','gpu_priority_options'},affinity
 for key in ['class_selection','effective_classes','smt_topology','managed','runtime_fallback','gpu_cpu_reservation']:
  assert isinstance(affinity[key],bool),(key,affinity)
 for level in affinity['performance_levels']:
  assert set(level)=={'logical_cpus','available_logical_cpus','physical_cpus'},level
  assert 0 <= level['physical_cpus'] <= level['available_logical_cpus'] <= level['logical_cpus'],level
 assert affinity['unit_allocations']=={},affinity
 assert affinity['gpu_helper_blocked_groups']=={},affinity
 if not affinity['managed']:
  assert not affinity.get('group_allocations'),affinity
  return
 allocation=affinity['group_allocations']['']
 assert allocation['configured_cpus']==requested,allocation
 assert allocation['cpu_mode']=='count' and allocation['cpu_class_counts']==[],allocation
 if snapshot['groups']['']['config']['paused']:
  assert allocation['allocated_workers']==0 and allocation['pool_logical_cpus']==0,allocation
 assert 0 <= allocation['allocated_workers'] <= min(requested,affinity['available']),allocation
 assert allocation['allocated_workers'] <= allocation['pool_logical_cpus'] <= affinity['available'],allocation
 assert allocation['logical_cpus'] == allocation['pool_logical_cpus'],allocation
 assert allocation['physical_cpus'] == allocation['pool_physical_cpus'],allocation
 assert 'full_smt' not in allocation and 'smt_in_use' not in allocation,allocation
 physical=allocation['pool_physical_cpus']
 logical=allocation['pool_logical_cpus']
 has_smt=physical > 0 and logical > physical
 assert allocation['has_smt'] == has_smt,allocation
 assert allocation['potential_full_smt'] == (has_smt and allocation['allocated_workers'] == logical),allocation

def run(exe,folder,port,edit=False):
 proc=subprocess.Popen([exe,'--http-addresses=127.0.0.1:'+str(port),'--open-web-control=false','--verbosity=5','--api-server=http://127.0.0.1:1','--assignment-servers=127.0.0.1:1'],cwd=folder,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0),env=os.environ.copy())
 try:
  for _ in range(100):
   if proc.poll() is not None:raise RuntimeError('Client exited: '+str(proc.returncode))
   try:s=socket.create_connection(('127.0.0.1',port),timeout=1);break
   except OSError:time.sleep(.1)
  else:raise RuntimeError('Client did not listen')
  s.settimeout(5);key=base64.b64encode(os.urandom(16)).decode()
  s.sendall(('GET /api/websocket HTTP/1.1\r\nHost: 127.0.0.1:'+str(port)+'\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: '+key+'\r\nSec-WebSocket-Version: 13\r\nOrigin: http://127.0.0.1:'+str(port)+'\r\n\r\n').encode())
  response=b''
  while not response.endswith(b'\r\n\r\n'):response+=s.recv(1)
  assert b'101' in response,response
  initial=frame_recv(s)
  (folder/'snapshot.json').write_text(json.dumps(initial,indent=2))
  if edit == "reject":
   baseline=configuration_state(initial)
   for invalid in [{'cpus':1.5},{'cpus':-1},{'cpus':4294967296},
                   {'cpu_class_counts':[1.5]},{'cpu_class_counts':[-1]},
                   {'cpu_class_counts':[4294967296]},
                   {'gpu_reserved_cores':1.5}, {'gpu_reserved_cores':-1},
                   {'gpu_reserved_cores':4294967296}, {'gpu_priority':'realtime'},
                    {'gpu_priority':'invalid'}, {'gpu_priority':3}]:
    invalid_socket,_=open_websocket(port)
    send(invalid_socket,{'cmd':'config','time':change_time(),
            'config':{'groups':{'':{**invalid,'paused':True,'cpu_mode':'count'}}}})
    rejected=[];invalid_socket.settimeout(.2)
    try:
     while True:rejected.append(frame_recv(invalid_socket))
    except (socket.timeout,AssertionError,OSError):pass
    finally:invalid_socket.close()
    groups=[x[1] for x in rejected if isinstance(x,list) and x[0]=='groups']
    current=authoritative_snapshot(port)
    assert configuration_state(current)==baseline,(invalid,current)
    actual=current['groups']['']['config']
    assert actual['cpus']==6 and actual['cpu_class_counts']==[],(invalid,actual)
    assert all(g['']['config']['cpus']==6 for g in groups),(invalid,rejected)
   print('PASS: fractional, negative and overflowing CPU and class counts rejected before mutation')
  if edit is True:
   send(s,{'cmd':'config','time':change_time(),'config':{'groups':{'':{'cpus':5,'paused':True,'cpu_mode':'count','pin_to_perf_cores':True,
              'gpu_priority': ('normal' if os.name=='nt' else 'other-normal')}}}})
   for _ in range(100):
    current=authoritative_snapshot(port)
    if current['groups']['']['config']['cpus']==5:
     verify_allocation(current,5)
     (folder/'accepted.json').write_text(json.dumps(current))
     break
    time.sleep(.1)
   else:raise AssertionError('Accepted configuration not published')
   # A real unrelated settings edit must not rebuild the CPU allocation.
   generation=current['info']['cpu_affinity']['allocation_generation']
   awake=not current['groups']['']['config']['keep_awake']
   send(s,{'cmd':'config','time':change_time(),'config':{'groups':{'':{'keep_awake':awake}}}})
   for _ in range(100):
    current=authoritative_snapshot(port)
    if current['groups']['']['config']['keep_awake']==awake:
     assert current['info']['cpu_affinity']['allocation_generation']==generation,current
     verify_allocation(current,5)
     (folder/'accepted.json').write_text(json.dumps(current))
     break
    time.sleep(.1)
   else:raise AssertionError('Unrelated configuration edit not confirmed')
   print('PASS: paused real group owns no CPU pool; unrelated settings keep allocation generation')
   messages=[];s.settimeout(.3)
   try:
    while True:messages.append(frame_recv(s))
   except (socket.timeout,AssertionError):pass
   (folder/'responses.json').write_text(json.dumps(messages,indent=2))
  s.close();time.sleep(.5)
  proc.terminate();proc.wait(timeout=10)
  with sqlite3.connect(folder/'client.db') as db:rows={k:json.loads(v) for k,v in db.execute('select name,value from groups')}
  db.close()
  return rows
 finally:
  proc.terminate();proc.wait(timeout=10)

folder=root/'client';folder.mkdir(exist_ok=True)
with sqlite3.connect(folder/'client.db') as db:
 db.execute('CREATE TABLE IF NOT EXISTS groups(name TEXT PRIMARY KEY,value)')
 db.execute('INSERT OR REPLACE INTO groups VALUES(?,?)',('',json.dumps({'paused':True,'cpus':6,'cpu_mode':'count','pin_to_perf_cores':True,'gpus':{}})))
db.close()
baseline_rows=run(executable,folder,args.port)
baseline_snapshot=json.loads((folder/'snapshot.json').read_text())
verify_allocation(baseline_snapshot,6)
rejected_rows=run(executable,folder,args.port,"reject")
assert rejected_rows==baseline_rows,(baseline_rows,rejected_rows)
print('PASS: rejected updates preserve complete group configuration, allocation and persisted group rows')
rows=run(executable,folder,args.port,True)
snapshot=json.loads((folder/'snapshot.json').read_text())
if args.expect_homogeneous:
 assert snapshot['info']['cpu_affinity']['managed'],snapshot['info']['cpu_affinity']
 assert not snapshot['info']['cpu_affinity']['runtime_fallback']
 assert not snapshot['info']['cpu_affinity']['class_selection']
 assert len(snapshot['info']['cpu_affinity']['performance_levels']) == 1
 print('PASS: native homogeneous General allocation is managed without fallback')
assert rows['']['cpus']==5,rows
assert rows['']['gpu_priority']==('normal' if os.name=='nt' else 'other-normal'),rows
assert 'pin_to_perf_cores' not in snapshot['groups'][''],snapshot['groups']
assert 'pin_to_perf_cores' not in rows[''],rows
print('PASS: removed legacy pin setting ignored on database load and API edit')
assert 'Parent already set' not in (folder/'log.txt').read_text()
messages=json.loads((folder/'responses.json').read_text())
assert all(any(isinstance(x,list) and x[0]==k for x in messages) for k in ['groups','units','info'])
print('PASS: ordinary edit persisted, final trees published, no observable ownership error')
accepted=json.loads((folder/'accepted.json').read_text())
restart_rows=run(executable,folder,args.port)
restarted=json.loads((folder/'snapshot.json').read_text())
assert restart_rows==rows,(rows,restart_rows)
assert configuration_state(restarted)==configuration_state(accepted),(accepted,restarted)
verify_allocation(restarted,5)
assert not restarted.get('units'),restarted.get('units')
print('PASS: accepted configuration and allocation survive a real client restart without work units')
temporary.cleanup()


