from pathlib import Path
import os, subprocess, tempfile
root = Path(__file__).resolve().parents[2]
source = (root / 'src/fah/client/Unit.cpp').read_text()
a = source.index('  // Monitor running core process')
b = source.index('  // Handle pause', a)
monitor = source[a:b]
a = source.index('std::set<unsigned> Unit::getDesiredAffinity()')
b = source.index('\n\n\nvoid Unit::run()', a)
affinity = source[a:b]
harness = r"""
#include <set>
#include <vector>
#include <cassert>
#include <iostream>
using namespace std;
#define LOG_DEBUG(a,b) do{}while(false)
const int UNIT_RUN=4;
struct CPU {bool supported=true;vector<set<unsigned>> levels={{0,1,2,3},{4,5}};bool hasPerformanceClasses()const{return supported;}const auto &getPerformanceLevels()const{return levels;}};
struct App {CPU cpu;const CPU &getCPUResources()const{return cpu;}};
struct Config {bool pin=false;bool getPinToPerfCores()const{return pin;}};
struct Process {bool live=true,stopping=false;bool isSet()const{return live;}bool isRunning()const{return live;}bool isStopping()const{return stopping;}Process *operator->(){return this;}};
struct Unit {
 App app;Config config;Process process;bool gpu=true,paused=false,affinityManaged=false,runningAffinityManaged=false;
 unsigned cpus=0,runningCPUs=1,delta=10,stops=0;int state=UNIT_RUN;set<unsigned> affinityCPUs,runningAffinityCPUs;
 const Config &getConfig()const{return config;}bool hasGPUs()const{return gpu;}
 unsigned getCPUs()const{return cpus;}bool isPaused()const{return paused;}int getState()const{return state;}
 unsigned getRunTimeDelta()const{return delta;}void triggerNext(unsigned){}void finalizeRun(){}int save(){return 0;}
 int stopRun(){++stops;return 1;}int monitorRun(){return 0;}
 set<unsigned> getDesiredAffinity()const;int checkRun();
};
""" + affinity + '\nint Unit::checkRun(){\n' + monitor + 'return -1;}\n' + r"""
int main(){
 // GPU count changes in both directions must not stop an otherwise unchanged process.
 for(bool pinned:{false,true}) for(unsigned before:{0u,1u,4u}) for(unsigned after:{0u,1u,4u}) {
   Unit u;u.config.pin=pinned;u.cpus=after;u.runningCPUs=before;u.runningAffinityCPUs=u.getDesiredAffinity();
   assert(u.checkRun()==0 && u.stops==0);
 }
 // CPU WUs still restart when their -np value changes, including legacy mode.
 Unit cpu;cpu.gpu=false;cpu.cpus=4;cpu.runningCPUs=6;assert(cpu.checkRun()==1);
 Unit stableCPU;stableCPU.gpu=false;stableCPU.cpus=stableCPU.runningCPUs;assert(stableCPU.checkRun()==0);
 // GPU pin enable/disable and changed topology must still restart.
 Unit enable;enable.config.pin=true;assert(enable.checkRun()==1);
 Unit disable;disable.runningAffinityCPUs={0,1,2,3};assert(disable.checkRun()==1);
 Unit topology;topology.config.pin=true;topology.runningAffinityCPUs={0,1};assert(topology.checkRun()==1);
 // CPU masks and enforcement-mode changes remain restart reasons.
 Unit mask;mask.gpu=false;mask.runningCPUs=mask.cpus;mask.affinityManaged=true;mask.runningAffinityManaged=true;mask.affinityCPUs={2,3};mask.runningAffinityCPUs={0,1};assert(mask.checkRun()==1);
 Unit mode;mode.gpu=false;mode.runningCPUs=mode.cpus;mode.affinityManaged=true;mode.affinityCPUs={0,1};mode.runningAffinityCPUs={0,1};assert(mode.checkRun()==1);
 // Pausing, leaving RUN and completing an existing shutdown remain effective.
 Unit paused;paused.paused=true;assert(paused.checkRun()==1);
 Unit state;state.state=UNIT_RUN+1;assert(state.checkRun()==1);
 Unit stopping;stopping.process.stopping=true;assert(stopping.checkRun()==1);
 // The five-second startup grace remains in effect for genuine changes.
 Unit grace;grace.config.pin=true;grace.delta=4;assert(grace.checkRun()==0 && grace.stops==0);
 cout<<"PASS: GPU bookkeeping transitions, pin/topology restarts, CPU count/masks, pause/state and shutdown grace\n";
}
"""
with tempfile.TemporaryDirectory(prefix='fah-restart-qa-') as d:
 p=Path(d);(p/'test.cpp').write_text(harness)
 if os.name=='nt':cmd=['cl','/nologo','/EHsc','/std:c++17','test.cpp','/Fetest.exe'];exe=p/'test.exe'
 else:cmd=[os.environ.get('CXX','c++'),'-std=c++17','test.cpp','-o','test'];exe=p/'test'
 subprocess.run(cmd,cwd=p,check=True);subprocess.run([str(exe)],check=True)
