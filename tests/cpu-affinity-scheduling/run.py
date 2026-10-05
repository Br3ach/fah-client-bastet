from pathlib import Path
import os,subprocess,tempfile,sys
here=Path(__file__).resolve().parent
root=here.parents[1]
u=(root/'src/fah/client/Unit.cpp').read_text();g=(root/'src/fah/client/Group.cpp').read_text()
a=u.index('bool Unit::blocksCPULaunch(');b=u.index('\n\n\nstd::set<unsigned> Unit::getDesiredAffinity()',a)
method=u[a:b]
a=g.index('  // Allocate GPUs with minimum CPU requirements');b=g.index('  // Allocate remaining CPUs to existing CPU WUs',a)
s=(here/'harness.cpp.in').read_text().replace('// RESERVATION_IMPLEMENTATION',method).replace('// GPU_SCHEDULING_IMPLEMENTATION',g[a:b])
with tempfile.TemporaryDirectory(prefix='fah-scheduling-qa-') as d:
 p=Path(d);(p/'test.cpp').write_bytes(s.encode())
 if os.name=='nt':
  subprocess.run(['cl','/nologo','/EHsc','/std:c++17','test.cpp','/Fetest.exe'],cwd=p,check=True);exe=p/'test.exe'
 else:
  subprocess.run([os.environ.get('CXX','c++'),'-std=c++17','test.cpp','-o','test'],cwd=p,check=True);exe=p/'test'
 subprocess.run([str(exe)],check=True)

subprocess.run([sys.executable,str(here/'gpu-affinity.py')],check=True)
subprocess.run([sys.executable,str(here/"config-update.py")],check=True)

subprocess.run([sys.executable,str(here/"gpu-validation.py")],check=True)

subprocess.run([sys.executable,str(here/'launch-recovery.py')],check=True)

subprocess.run([sys.executable,str(here/'pool-scheduling.py')],check=True)

subprocess.run([sys.executable,str(here/'topology-watcher.py')],check=True)

subprocess.run([sys.executable,str(here/'wu-metadata.py')],check=True)

subprocess.run([sys.executable,str(here/'assignment-reconciliation.py')],check=True)
