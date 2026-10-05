from pathlib import Path
import os,subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'src/fah/client/App.cpp').read_text()
a=s.index('  cpuRefreshEvent = base.newEvent([this, topologyRefreshInterval] {')
b=s.index('\n  }, 0);',a)
body=s[a:b].split('{',1)[1]
harness=r"""
#include <functional>
#include <memory>
#include <stdexcept>
#include <iostream>
#include <cassert>
#define LOG_DEBUG(a,b) do {} while(0)
struct CPU {bool fail=true;bool refreshTopology(const char*) {if(fail){fail=false;throw std::runtime_error("probe");}return true;} unsigned getTopologyGeneration(){return 0;}};
struct Timer{bool pending=false;void add(unsigned seconds){assert(seconds==300);pending=true;}};
struct Groups{bool fail=true;unsigned updates=0;void triggerUpdate(){if(fail){fail=false;throw std::runtime_error("reconcile");}++updates;}};
struct App{std::unique_ptr<CPU> cpuResources{new CPU};std::unique_ptr<Timer> cpuRefreshEvent{new Timer};Groups groups;Groups* getGroups(){return &groups;}
void fire(){const unsigned topologyRefreshInterval=300;cpuRefreshEvent->pending=false;
 auto callback=[this,topologyRefreshInterval]{BODY};
 try{callback();}catch(...){/* Event::call logs and catches */}
}};
int main(){App a;for(unsigned i=0;i<3;++i){a.fire();assert(a.cpuRefreshEvent->pending);}assert(a.groups.updates==1);std::cout<<"PASS: topology watcher survives probe and reconciliation failures, then recovers\n";}
""".replace('BODY',body)
with tempfile.TemporaryDirectory(prefix='fah-watcher-') as d:
 p=Path(d);(p/'test.cpp').write_text(harness)
 if os.name=='nt':cmd=['cl','/nologo','/EHsc','/std:c++17','test.cpp','/Fetest.exe'];exe=p/'test.exe'
 else:cmd=[os.environ.get('CXX','c++'),'-std=c++17','test.cpp','-o','test'];exe=p/'test'
 subprocess.run(cmd,cwd=p,check=True);subprocess.run([str(exe)],check=True)
