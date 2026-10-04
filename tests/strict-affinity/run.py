from pathlib import Path
import os,subprocess,tempfile,argparse
parser=argparse.ArgumentParser();parser.add_argument('--openssl',type=Path,default=Path('C:/OpenSSL-3.5'));parser.add_argument('--cbang',type=Path,required=True);args=parser.parse_args()
here=Path(__file__).resolve().parent;client=here.parents[1];root=args.cbang.resolve()
with tempfile.TemporaryDirectory(prefix='cbang-affinity-qa-') as d:
 build=Path(d);(build/'work').mkdir()
 if os.name=='nt':
  exe=build/'test.exe'
  libs=['cbang','cbang-boost','setupapi','winmm','ws2_32','crypt32','user32','gdi32','advapi32','libssl','libcrypto','re2','event','yaml','sqlite3','expat','lz4','bz2','z']
  cmd=['cl','/nologo','/EHsc','/std:c++17','/MT','/DNOMINMAX','/DUSING_CBANG','/DBOOST_ALL_NO_LIB','/I'+str(root/'src'),'/I'+str(root/'include'),'/I'+str(root/'src/boost'),'/I'+str(client/'src'),str(here/'test.cpp'),str(client/'src/fah/client/CoreProcess.cpp'),'/Fe'+str(exe),'/link','/LIBPATH:'+str(root/'lib'),'/LIBPATH:'+str(args.openssl/'lib/VC/x64/MT')]+[l+'.lib' for l in libs]
 else:
  raise RuntimeError('Use linux-native.py for the Linux native launch checks')
 subprocess.run(cmd,cwd=build,check=True)
 subprocess.run([str(exe),str(build/'marker')],cwd=build,check=True)
