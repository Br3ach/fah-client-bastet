"""Compile production desired/live affinity, launch exclusion and restart logic."""
from pathlib import Path
import os, subprocess, tempfile
source = (Path(__file__).resolve().parents[2] / 'src/fah/client/Unit.cpp').read_text()
a = source.index('bool Unit::desiredGPUReservation()')
b = source.index('void Unit::run()', a)
methods = source[a:b]
a = source.index('  // Monitor running core process')
b = source.index('  // Handle pause', a)
monitor = source[a:b]
harness = r'''
#include <fah/client/CPUExecutionPlan.h>
#include <set>
#include <vector>
#include <string>
#include <memory>
#include <cassert>
#include <iostream>
using namespace std;
using FAH::Client::CPUExecutionPlan;
#define LOG_DEBUG(a,b) do{}while(false)
const int UNIT_RUN=4;
struct CPU {
 bool supported=true,hard=true;vector<set<unsigned>> levels={{0,1,2,3},{4,5}};set<unsigned> mask={0,1,2,3};
 bool supportsGPUAffinity()const{return hard;}bool hasPerformanceClasses()const{return supported;}
 const auto &getPerformanceLevels()const{return levels;}
 const auto &getGPUCPUs(const string &,const string &)const{return mask;}
 vector<set<unsigned>> getCoreThreads()const{return {{0,1},{2,3},{4,5}};}
 vector<unsigned> getGroupCPUs(const string &)const{return {0,2,4,1,3,5};}
};
struct App {CPU cpu;const CPU &getCPUResources()const{return cpu;}};
struct Config {unsigned reserved=0;unsigned getGPUReservedCores()const{return reserved;}};
struct Group {string name;string getName()const{return name;}};
struct Process {bool live=true,stopping=false;bool isSet()const{return live;}bool isRunning()const{return live;}bool isStopping()const{return stopping;}Process *operator->(){return this;}};
struct Core {bool isSet()const{return true;}const Core *operator->()const{return this;}unsigned getType()const{return 0xa8;}};
struct Unit {
 App app;Config config;Process process;Core core;shared_ptr<Group> group=make_shared<Group>();
 bool gpu=true,paused=false,affinityManaged=false,runningAffinityManaged=false,runningGPUReservation=false;
 unsigned cpus=0,runningCPUs=1,delta=10,stops=0;int state=UNIT_RUN;set<unsigned> affinityCPUs,runningAffinityCPUs,runningResourceCPUs;
 set<string> getGPUs()const{return gpu?set<string>{"gpu"}:set<string>{};}
 const Config &getConfig()const{return config;}bool hasGPUs()const{return gpu;}
 unsigned getCPUs()const{return cpus;}bool isPaused()const{return paused;}int getState()const{return state;}
 unsigned getRunTimeDelta()const{return delta;}void triggerNext(unsigned){}void finalizeRun(){}int save(){return 0;}
 int stopRun(){++stops;return 1;}int monitorRun(){return 0;}
 bool desiredGPUReservation()const;bool desiredAffinityManaged()const;
 bool blocksCPULaunch(const Unit&)const;set<unsigned> getDesiredAffinity()const;set<unsigned> getDesiredResourceCPUs()const;int checkRun();
 void launched(){runningCPUs=cpus;runningAffinityManaged=desiredAffinityManaged();runningGPUReservation=desiredGPUReservation();runningAffinityCPUs=getDesiredAffinity();runningResourceCPUs=getDesiredResourceCPUs();}
};
''' + methods + '\nint Unit::checkRun(){\n' + monitor + 'return -1;}\n' + r'''
int main() {
 // Zero gets a required Performance 1 mask independent of helper count.
 for(unsigned before:{0u,1u,4u})for(unsigned after:{0u,1u,4u}) {
  Unit u;u.cpus=before;u.launched();u.cpus=after;
  assert(u.desiredAffinityManaged());assert(u.getDesiredAffinity()==set<unsigned>({0,1,2,3}));
  assert(u.checkRun()==0 && u.stops==0);
 }
 // Reserved GPU processes stay running after launch (no desired/live mode mismatch).
 Unit reserved;reserved.config.reserved=1;reserved.app.cpu.mask={0,2};reserved.launched();
 assert(reserved.runningGPUReservation && reserved.checkRun()==0);
 reserved.config.reserved=0;assert(reserved.checkRun()==1);reserved.launched();assert(reserved.checkRun()==0);
 reserved.config.reserved=1;reserved.app.cpu.mask={1,3};assert(reserved.checkRun()==1);
 reserved.launched();assert(reserved.checkRun()==0);
 reserved.app.cpu.mask={0,2};assert(reserved.checkRun()==1);
 // An empty exclusive allocation waits instead of acquiring unrestricted affinity.
 Unit unavailable;unavailable.config.reserved=1;unavailable.app.cpu.hard=false;unavailable.app.cpu.mask.clear();
 assert(unavailable.desiredAffinityManaged() && unavailable.getDesiredAffinity().empty());
 // Unmanaged CPU work has no external mask. Managed pools still apply.
 Unit cpu;cpu.gpu=false;cpu.cpus=4;assert(cpu.getDesiredAffinity().empty());
 cpu.affinityManaged=true;cpu.cpus=1;cpu.affinityCPUs={4};assert(cpu.getDesiredAffinity()==set<unsigned>({4}));
 cpu.runningCPUs=6;assert(cpu.checkRun()==1);
 Unit stableCPU;stableCPU.gpu=false;stableCPU.cpus=stableCPU.runningCPUs;assert(stableCPU.checkRun()==0);
 // GPU transitions must wait for old CPU or GPU users of newly reserved cores.
 Unit next,old;next.config.reserved=1;next.app.cpu.mask={0,2};
 old.gpu=false;old.affinityManaged=true;old.affinityCPUs={0,1};old.launched();assert(next.blocksCPULaunch(old));
 old.affinityCPUs={1,3};old.launched();assert(!next.blocksCPULaunch(old));
 old.affinityManaged=false;old.launched();assert(next.blocksCPULaunch(old));
 old.gpu=true;old.app.cpu.mask={0,1,2,3};old.launched();assert(next.blocksCPULaunch(old));
 old.process.live=false;assert(!next.blocksCPULaunch(old));old.process.live=true;
 // Separate GPU reservations cannot overlap, including within one RG.
 old.config.reserved=1;old.app.cpu.mask={0,2};old.launched();assert(next.blocksCPULaunch(old));
 old.group->name="other";assert(next.blocksCPULaunch(old));
 // Releasing a GPU reservation does not let CPU work start before the old process exits.
 next.gpu=false;next.affinityManaged=true;next.affinityCPUs={0,1};assert(next.blocksCPULaunch(old));
 old.runningGPUReservation=false;assert(!next.blocksCPULaunch(old));
 Unit paused;paused.launched();paused.paused=true;assert(paused.checkRun()==1);
 Unit state;state.launched();state.state=UNIT_RUN+1;assert(state.checkRun()==1);
 Unit stopping;stopping.process.stopping=true;assert(stopping.checkRun()==1);
 Unit grace;grace.launched();grace.app.cpu.mask={1,3};grace.delta=4;assert(grace.checkRun()==0 && grace.stops==0);
 cout<<"PASS: zero/shared hard affinity, stable reserved GPU, reservation/mask changes, unmanaged CPU scheduling, CPU/GPU exclusion, separate same-RG GPU reservations, shutdown grace\n";
}
'''
with tempfile.TemporaryDirectory(prefix='fah-gpu-affinity-qa-') as d:
 p=Path(d);(p/'test.cpp').write_text(harness)
 if os.name=='nt':cmd=['cl','/nologo','/EHsc','/std:c++17','/I'+str(Path(__file__).resolve().parents[2]/'src'),'test.cpp','/Fetest.exe'];exe=p/'test.exe'
 else:cmd=[os.environ.get('CXX','c++'),'-std=c++17','-I'+str(Path(__file__).resolve().parents[2]/'src'),'test.cpp','-o','test'];exe=p/'test'
 cmd.insert(cmd.index("test.cpp")+1,str(Path(__file__).resolve().parents[2]/"src/fah/client/CPUExecutionPlan.cpp"))
 subprocess.run(cmd,cwd=p,check=True);subprocess.run([str(exe)],check=True)
