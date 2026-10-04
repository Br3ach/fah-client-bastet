from pathlib import Path
import os,subprocess,tempfile,sys
here=Path(__file__).resolve().parent
root=here.parents[1]
u=(root/'src/fah/client/Unit.cpp').read_text();g=(root/'src/fah/client/Group.cpp').read_text()
a=u.index('bool Unit::blocksCPULaunch(');b=u.index('\n\n\nstd::set<unsigned> Unit::getDesiredAffinity()',a)
method=u[a:b]
a=g.index('  // Allocate GPUs with minimum CPU requirements');b=g.index('  // Allocate remaining CPUs to existing CPU WUs',a)
s=(here/'harness.cpp.in').read_text().replace('// RESERVATION_IMPLEMENTATION',method).replace('// GPU_SCHEDULING_IMPLEMENTATION',g[a:b])
groups=(root/'src/fah/client/Groups.cpp').read_text()
a=groups.index('static void validatePerformancePin(');b=groups.index('\n\n\nvoid Groups::validateCPUConfiguration(',a)
s=s.replace('// PIN_VALIDATION_IMPLEMENTATION',groups[a:b])
a=groups.index('    bool pinUnchanged = ');b=groups.index(';',a)
expression=groups[a:b].split(' = ',1)[1]
expression=expression.replace('has(name)', 'exists').replace('getGroup(name).getConfig().getPinToPerfCores()', 'oldPin').replace('getGroup(name).getConfig().getCPUMode()', 'oldMode').replace('getGroup(name).getConfig().getConfiguredCPUTotal()', 'oldCPUs').replace('config->getU32("cpus", 0)', 'cpus')
s=s.replace('// PIN_UNCHANGED_IMPLEMENTATION', 'bool pinUnchanged(bool oldAnyClass,bool anyClass,bool exists,bool oldPin,bool pin,string oldMode,string mode,unsigned oldCPUs,unsigned cpus) {return '+expression+';}')
with tempfile.TemporaryDirectory(prefix='fah-scheduling-qa-') as d:
 p=Path(d);(p/'test.cpp').write_bytes(s.encode())
 if os.name=='nt':
  subprocess.run(['cl','/nologo','/EHsc','/std:c++17','test.cpp','/Fetest.exe'],cwd=p,check=True);exe=p/'test.exe'
 else:
  subprocess.run([os.environ.get('CXX','c++'),'-std=c++17','test.cpp','-o','test'],cwd=p,check=True);exe=p/'test'
 subprocess.run([str(exe)],check=True)

subprocess.run([sys.executable,str(here/'restart.py')],check=True)
subprocess.run([sys.executable,str(here/"config-update.py")],check=True)
