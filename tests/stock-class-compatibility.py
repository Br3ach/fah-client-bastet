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
parser=argparse.ArgumentParser(description='Verify stock-client compatibility using disposable paused data')
parser.add_argument('--client',required=True,type=Path)
parser.add_argument('--affinity-client',required=True,type=Path,help='Current client for stock-to-affinity database round-trip')
parser.add_argument('--port',type=int,default=17496)
args=parser.parse_args()
temporary=tempfile.TemporaryDirectory(prefix='fah-stock-compatibility-')
root=Path(temporary.name)
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
def run(exe,folder,port,edit=False,classes=True,counts=(5,3)):
 proc=subprocess.Popen([exe,'--http-addresses=127.0.0.1:'+str(port),'--open-web-control=false'],cwd=folder,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
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
  if edit:
   groups={'':{'cpus':counts[0],'paused':True},'test':{'cpus':counts[1],'paused':True}}
   if classes:
    for name,counts in [('',[3,2]),('test',[2,1])]:
     groups[name].update(cpu_mode='classes',cpu_class_counts=counts,
       gpu_reserved_cores=1,gpu_priority='normal')
   send(s,{'cmd':'config','time':datetime.datetime.now(datetime.timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ'),'config':{'groups':groups}})
   time.sleep(2)
  s.close();time.sleep(.5)
  proc.terminate();proc.wait(timeout=10)
  with sqlite3.connect(folder/'client.db') as db:rows={k:json.loads(v) for k,v in db.execute('select name,value from groups')}
  db.close()
  return rows
 finally:
  proc.terminate();proc.wait(timeout=10)
folder=root/'stock';folder.mkdir(exist_ok=True)
with sqlite3.connect(folder/'client.db') as db:
 db.execute('CREATE TABLE IF NOT EXISTS groups(name TEXT PRIMARY KEY,value)')
 for name,counts in [('',[4,2]),('test',[2,2])]:
  db.execute('INSERT OR REPLACE INTO groups VALUES(?,?)',(name,json.dumps({'paused':True,'cpus':sum(counts),'cpu_mode':'classes','cpu_class_counts':counts,'gpu_reserved_cores':1,'gpu_priority':'normal',
    'gpus':{'compatibility-gpu':{'enabled':False}}})))
db.close()
stock=str(args.client.resolve())
rows=run(stock,folder,args.port)
for name,count in [('',6),('test',4)]:
 assert rows[name]['cpus']==count,rows
 assert rows[name]['paused'] is True
 assert all(key not in rows[name] for key in
  ('cpu_mode','cpu_class_counts','gpu_reserved_cores','gpu_priority')),rows
 assert rows[name]['gpus']=={'compatibility-gpu':{'enabled':False}},rows
print('PASS: actual backed-up stock executable loads class-enabled database, retains cpus totals, removes new fields on startup save')
rows=run(stock,folder,args.port,True)
for name,count in [('',5),('test',3)]:
 assert rows[name]['cpus']==count,rows
 assert all(key not in rows[name] for key in
  ('cpu_mode','cpu_class_counts','gpu_reserved_cores','gpu_priority')),rows
 assert rows[name]['gpus']=={'compatibility-gpu':{'enabled':False}},rows
print('PASS: stock API accepts supported cpus edit, ignores incoming class/reservation/priority fields and saves stock-compatible state')
affinity=str(args.affinity_client.resolve())
previous=(5,3)
for edit_config,counts in [(False,(5,3)),(True,(4,2)),(False,(4,2))]:
 rows=run(affinity,folder,args.port,edit_config,classes=False,counts=counts)
 snapshot=json.loads((folder/'snapshot.json').read_text())
 for i,name in enumerate(['','test']):
  count=counts[i]
  assert rows[name]['cpus']==count and rows[name]['paused'] is True,rows
  assert rows[name].get('cpu_mode','count')=='count',rows
  assert not rows[name].get('cpu_class_counts'),rows
  assert not rows[name].get('gpu_reserved_cores') and not rows[name].get('gpu_priority'),rows
  assert rows[name]['gpus']=={'compatibility-gpu':{'enabled':False}},rows
  config=snapshot['groups'][name]['config']
  assert config['cpus']==previous[i] and config.get('cpu_mode','count')=='count',snapshot
 previous=counts
print('PASS: affinity client reopens stock-edited database, accepts General configuration and retains it across restart')
(root/'RESULTS.json').write_text(json.dumps(rows,indent=2))
