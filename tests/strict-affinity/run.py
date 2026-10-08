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
import os,subprocess,tempfile,argparse
parser=argparse.ArgumentParser();parser.add_argument('--openssl',type=Path,default=Path(os.environ.get('OPENSSL_HOME','C:/OpenSSL-3.5')));parser.add_argument('--cbang',type=Path,required=True);args=parser.parse_args()
here=Path(__file__).resolve().parent;client=here.parents[1];root=args.cbang.resolve()
with tempfile.TemporaryDirectory(prefix='cbang-affinity-qa-') as d:
 build=Path(d);(build/'work').mkdir()
 if os.name=='nt':
  exe=build/'test.exe'
  libs=['cbang','cbang-boost','setupapi','winmm','ws2_32','crypt32','user32','gdi32','advapi32','libssl','libcrypto','re2','event','yaml','sqlite3','expat','lz4','bz2','z']
  cmd=['cl','/nologo','/EHsc','/std:c++17','/MT','/DNOMINMAX','/DUSING_CBANG','/DFAH_STRICT_LAUNCH_TEST','/DBOOST_ALL_NO_LIB','/I'+str(root/'src'),'/I'+str(root/'include'),'/I'+str(root/'src/boost'),'/I'+str(client/'src'),str(here/'test.cpp'),str(client/'src/fah/client/CoreProcess.cpp'),str(client/'src/fah/client/win/CoreProcessLaunch.cpp'),'/Fe'+str(exe),'/link','/LIBPATH:'+str(root/'lib'),'/LIBPATH:'+os.environ.get('OPENSSL_LIBPATH',str(args.openssl/'lib/VC/x64/MT'))]+[l+'.lib' for l in libs]
 else:
  raise RuntimeError('Use linux-native.py for the Linux native launch checks')
 subprocess.run(cmd,cwd=build,check=True)
 hidden=subprocess.STARTUPINFO()
 hidden.dwFlags=subprocess.STARTF_USESHOWWINDOW
 hidden.wShowWindow=0
 # Kill the entire native test tree on timeout, including held child modes.
 def run_native(**options):
  process=subprocess.Popen([str(exe),str(build/'marker')],cwd=build,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,**options)
  try:
   output,_=process.communicate(timeout=60)
   code=process.returncode
   print(output.decode(errors="replace"),end="")
  except subprocess.TimeoutExpired:
   subprocess.run(['taskkill','/PID',str(process.pid),'/T','/F'],check=False)
   process.wait(timeout=10)
   raise
  if code:raise subprocess.CalledProcessError(code,process.args)
 run_native(creationflags=subprocess.CREATE_NEW_CONSOLE,startupinfo=hidden)
 run_native(creationflags=subprocess.CREATE_NO_WINDOW)
 print('PASS: strict graceful stop with inherited and absent parent consoles')

 # Compile the production destructor against fault-injected operations using
 # cbang's actual catch macros, in both debug and release preprocessing modes.
 source=(client/'src/fah/client/CoreProcess.cpp').read_text()
 start=source.index('CoreProcess::~CoreProcess()')
 destructor=source[start:source.index('void CoreProcess::setRequiredAffinity',start)]
 cleanup=r"""
#include <cbang/Exception.h>
#include <cbang/Catch.h>
#include <cassert>
#include <memory>
#include <stdexcept>
static int fault=0, logs=0, kills=0;
static void logFault(){++logs;if(fault==4||fault==5)throw std::runtime_error("logging");}
#undef CBANG_LOG_LEVEL
#undef CBANG_LOG_LEVEL_LOCATION
#undef LOG_ERROR
#define CBANG_LOG_LEVEL(...) logFault()
#define CBANG_LOG_LEVEL_LOCATION(...) logFault()
#define LOG_ERROR(...) logFault()
struct CoreProcess {
  std::unique_ptr<int> strictProcess;
  bool running=true;
  ~CoreProcess();
  unsigned getPID() const{return 123;}
  bool kill(){++kills;if(fault==1||fault==5)throw std::runtime_error("kill");
    if(fault==2)throw 42;if(fault==3)throw cb::Exception("kill");
    return fault!=4;}
};
"""+destructor+r"""
int main(){
 for(fault=0;fault<=5;++fault){
  kills=logs=0;
  {CoreProcess p;p.strictProcess.reset(new int);}
  assert(kills==1);
  assert(logs==(fault==4?2:(fault?1:0)));
 }
 kills=logs=0;{CoreProcess p;}assert(!kills&&!logs);
 {CoreProcess p;p.strictProcess.reset(new int);p.running=false;}
 assert(!kills&&!logs);
}
"""
 (build/'cleanup.cpp').write_text(cleanup)
 for debug in (False,True):
  cleanupExe=build/('cleanup-debug.exe' if debug else 'cleanup-release.exe')
  cleanupCmd=[x for x in cmd if not x.endswith('.cpp') and not x.startswith('/Fe')]
  if debug:cleanupCmd.insert(1,'/DDEBUG')
  index=cleanupCmd.index('/link')
  cleanupCmd[index:index]=[str(build/'cleanup.cpp'),'/Fe'+str(cleanupExe)]
  subprocess.run(cleanupCmd,cwd=build,check=True)
  subprocess.run([str(cleanupExe)],cwd=build,check=True,timeout=10)
 print('PASS: production destructor contains cleanup and logging exceptions in debug/release')
