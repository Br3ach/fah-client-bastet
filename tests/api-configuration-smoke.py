from pathlib import Path
import sqlite3,json,subprocess,time,socket,base64,os,struct,datetime
import argparse,tempfile
parser=argparse.ArgumentParser(description='Test real client configuration with disposable paused data')
parser.add_argument('--client',required=True,type=Path)
parser.add_argument('--port',type=int,default=17497)
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
   send(s,{'cmd':'config','time':datetime.datetime.now(datetime.timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ'),'config':{'groups':{'':{'cpus':5,'paused':True,'cpu_mode':'count','pin_to_perf_cores':pin_requested}}}})
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
 db.execute('INSERT OR REPLACE INTO groups VALUES(?,?)',('',json.dumps({'paused':True,'cpus':6,'pin_to_perf_cores':False,'cpu_mode':'count','gpus':{}})))
db.close()
pin_requested=True
rows=run(executable,folder,args.port,True)
snapshot=json.loads((folder/'snapshot.json').read_text())
if snapshot['info']['cpu_affinity']['class_selection']:
 print('SKIP: unsupported-pinning check requires a machine without performance classes')
 temporary.cleanup()
 raise SystemExit(0)
assert rows['']['pin_to_perf_cores'] is False and rows['']['cpus']==6,rows
print('\\n'.join(line for line in (folder/'log.txt').read_text().splitlines() if any(x in line for x in ['validation','Parent','Remote','msg:','requires'])))
time.sleep(1.1)
pin_requested=False
rows=run(executable,folder,args.port,True)
assert rows['']['cpus']==5 and rows['']['pin_to_perf_cores'] is False,rows
assert 'Parent already set' not in (folder/'log.txt').read_text()
messages=json.loads((folder/'responses.json').read_text())
assert all(any(isinstance(x,list) and x[0]==k for x in messages) for k in ['groups','units','info'])
print('PASS: unsupported pin rejected, ordinary edit persisted, final trees published, no observable ownership error')
temporary.cleanup()


