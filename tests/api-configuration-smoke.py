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

def run(exe,folder,port,edit=False):
 proc=subprocess.Popen([exe,'--http-addresses=127.0.0.1:'+str(port),'--open-web-control=false','--verbosity=5'],cwd=folder,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,creationflags=subprocess.CREATE_NO_WINDOW,env=os.environ.copy())
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
   for invalid in [{'cpus':1.5},{'cpus':-1},{'cpus':4294967296},
                   {'cpu_class_counts':[1.5]},{'cpu_class_counts':[-1]},
                   {'cpu_class_counts':[4294967296]}]:
    invalid_socket,_=open_websocket(port)
    send(invalid_socket,{'cmd':'config','time':change_time(),
            'config':{'groups':{'':{**invalid,'paused':True,'cpu_mode':'count'}}}})
    rejected=[];invalid_socket.settimeout(.2)
    try:
     while True:rejected.append(frame_recv(invalid_socket))
    except (socket.timeout,AssertionError,OSError):pass
    finally:invalid_socket.close()
    groups=[x[1] for x in rejected if isinstance(x,list) and x[0]=='groups']
    actual=authoritative_snapshot(port)['groups']['']['config']
    assert actual['cpus']==6 and actual['cpu_class_counts']==[],(invalid,actual)
    assert all(g['']['config']['cpus']==6 for g in groups),(invalid,rejected)
   print('PASS: fractional, negative and overflowing CPU and class counts rejected before mutation')
   send(s,{'cmd':'config','time':change_time(),'config':{'groups':{'':{'cpus':5,'paused':True,'cpu_mode':'count','pin_to_perf_cores':True}}}})
   time.sleep(1)
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
rows=run(executable,folder,args.port,True)
snapshot=json.loads((folder/'snapshot.json').read_text())
if args.expect_homogeneous:
 assert snapshot['info']['cpu_affinity']['managed'],snapshot['info']['cpu_affinity']
 assert not snapshot['info']['cpu_affinity']['runtime_fallback']
 assert not snapshot['info']['cpu_affinity']['class_selection']
 assert len(snapshot['info']['cpu_affinity']['performance_levels']) == 1
 print('PASS: native homogeneous General allocation is managed without fallback')
assert rows['']['cpus']==5,rows
assert 'pin_to_perf_cores' not in snapshot['groups'][''],snapshot['groups']
assert 'pin_to_perf_cores' not in rows[''],rows
print('PASS: removed legacy pin setting ignored on database load and API edit')
assert 'Parent already set' not in (folder/'log.txt').read_text()
messages=json.loads((folder/'responses.json').read_text())
assert all(any(isinstance(x,list) and x[0]==k for x in messages) for k in ['groups','units','info'])
print('PASS: ordinary edit persisted, final trees published, no observable ownership error')
temporary.cleanup()


