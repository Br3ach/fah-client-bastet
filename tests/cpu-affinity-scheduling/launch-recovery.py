from pathlib import Path
import os, subprocess, tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'src/fah/client/Unit.cpp').read_text()
a=s.index('  } catch (const AffinityRejected &e) {');b=s.index(' CATCH_ERROR;',a)
catch=s[a:b]
cpp=r"""
#include <cassert>
#include <stdexcept>
#include <string>
#include <set>
#include <algorithm>
#include <cstdint>
using namespace std;
#define LOG_WARNING(x) ((void)0)
struct AffinityRejected:runtime_error{AffinityRejected():runtime_error("mask rejected") {}};
struct Resources{unsigned probes=0;uint64_t generation=1;uint64_t getTopologyGeneration(){return generation;}void refreshTopology(const string &why){assert(why=="affinity-rejected");probes++;}};
struct App{Resources resources;unsigned updates=0;Resources &getCPUResources(){return resources;}void triggerUpdate(){updates++;}};
struct Unit{App app;uint64_t rejectedTopologyGeneration=0;unsigned affinityRejections=0;set<unsigned> rejectedAffinityCPUs,runningAffinityCPUs{0,2};template<class T>void insert(const char *,const T &){}unsigned retries=7, delay=0;void setWait(unsigned seconds){delay=seconds;}void triggerNext(unsigned seconds){assert(delay==seconds);}void launch(){try{throw AffinityRejected();
"""+catch+r"""
}
};
int main(){Unit u;u.launch();assert(u.app.resources.probes==1&&u.app.updates==1&&u.delay==5&&u.retries==7);
 u.launch();assert(u.delay==5);u.launch();assert(u.delay==30);
 u.launch();assert(u.delay==60);for(unsigned i=0;i<10;++i)u.launch();assert(u.delay==300&&u.retries==7);
 u.app.resources.generation++;u.launch();assert(u.delay==5);
 u.runningAffinityCPUs={0,4};u.launch();assert(u.delay==5&&u.affinityRejections==1);
}
"""
with tempfile.TemporaryDirectory() as directory:
 p=Path(directory);(p/'test.cpp').write_text(cpp)
 if os.name=='nt':
  subprocess.run(['cl','/nologo','/EHsc','/std:c++17','test.cpp','/Fetest.exe'],cwd=p,check=True);exe=p/'test.exe'
 else:
  subprocess.run(['g++','-std=c++17','test.cpp','-o','test'],cwd=p,check=True);exe=p/'test'
 subprocess.run([str(exe)],check=True)
print('PASS: affinity rejection refreshes topology and reschedules without WU retry')
