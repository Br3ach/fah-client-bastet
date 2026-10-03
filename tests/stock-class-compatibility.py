from pathlib import Path
import sqlite3,json,subprocess,time,socket,base64,os,struct,datetime
import argparse,tempfile
parser=argparse.ArgumentParser(description='Verify stock-client compatibility using disposable paused data')
parser.add_argument('--client',required=True,type=Path)
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
def run(exe,folder,port,edit=False):
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
   send(s,{'cmd':'config','time':datetime.datetime.now(datetime.timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ'),'config':{'groups':{'':{'cpus':5,'paused':True,'cpu_mode':'classes','cpu_class_counts':[3,2]},'test':{'cpus':3,'paused':True,'cpu_mode':'classes','cpu_class_counts':[2,1]}}}})
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
  db.execute('INSERT OR REPLACE INTO groups VALUES(?,?)',(name,json.dumps({'paused':True,'cpus':sum(counts),'cpu_mode':'classes','cpu_class_counts':counts,'gpus':{}})))
db.close()
stock=str(args.client.resolve())
rows=run(stock,folder,args.port)
for name,count in [('',6),('test',4)]:
 assert rows[name]['cpus']==count,rows
 assert rows[name]['paused'] is True
 assert 'cpu_mode' not in rows[name] and 'cpu_class_counts' not in rows[name],rows
print('PASS: actual backed-up stock executable loads class-enabled database, retains cpus totals, removes new fields on startup save')
rows=run(stock,folder,args.port,True)
for name,count in [('',5),('test',3)]:
 assert rows[name]['cpus']==count,rows
 assert 'cpu_mode' not in rows[name] and 'cpu_class_counts' not in rows[name],rows
print('PASS: stock API accepts supported cpus edit, ignores incoming class fields and saves stock-compatible state')
(root/'RESULTS.json').write_text(json.dumps(rows,indent=2))
