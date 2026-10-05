"""Run the complete production Group::update with a pending/retrying assignment."""
from pathlib import Path
import os, subprocess, tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'src/fah/client/Group.cpp').read_text()
a=s.index('void Group::update() {');b=s.index('void Group::setWait(',a)
body=s[a:b].strip().replace('void Group::update()', 'void update()')
harness=r"""
#include <fah/client/CPUExecutionPlan.h>
#include <algorithm>
#include <cassert>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>
using namespace std; using FAH::Client::CPUExecutionPlan;
#define LOG_DEBUG(a,b) do{}while(false)
#define LOG_INFO(a,b) do{}while(false)
enum UnitState {UNIT_ASSIGN,UNIT_DOWNLOAD,UNIT_CORE,UNIT_RUN,UNIT_UPLOAD,UNIT_DUMP,UNIT_DONE};
struct Time {static double now(){return 10;}};
struct App;
struct Unit {
 string id;UnitState state=UNIT_RUN;unsigned cpus=2,minimum=1,maximum=8;
 bool paused=false,managed=true;set<unsigned> pool={0,1,2,3};set<string> gpus;
 Unit()=default;Unit(App&,const string&,unsigned,unsigned,const set<string>&){}
 string getID()const{return id;}UnitState getState()const{return state;}
 bool hasGPUs()const{return !gpus.empty();}auto getGPUs()const{return gpus;}
 unsigned getMinCPUs()const{return minimum;}unsigned getMaxCPUs()const{return maximum;}
 unsigned getCPUs()const{return cpus;}void setCPUs(unsigned n){cpus=n;}
 void setCPUAffinity(bool m,const set<unsigned>&p){managed=m;pool=p;}
 bool atRunState()const{return state==UNIT_RUN;}void setPause(bool p){paused=p;}
 bool isRunning()const{return state==UNIT_RUN;}void triggerNext(){}void save(){}
};
struct CPU {
 vector<unsigned> pool={4,6,5,7};unsigned workers=2;bool managed=true;
 vector<set<unsigned>> cores={{0,1},{2,3},{4,5},{6,7}};
 bool isManaged()const{return managed;}auto getGroupCPUs(const string&)const{return pool;}
 unsigned getGroupWorkerCount(const string&)const{return workers;}
 auto &getCoreThreads()const{return cores;}
};
struct Config {
 bool paused=false;unsigned cpus=2;
 bool getPaused()const{return paused;}void setPaused(bool p){paused=p;}
 unsigned getCPUs()const{return cpus;}set<string> getGPUs()const{return {"gpu"};}
 bool getFinish()const{return false;}
};
struct Units {vector<shared_ptr<Unit>> wus;unsigned added=0;
 void removeUnit(const string&id){wus.erase(remove_if(wus.begin(),wus.end(),[&](auto u){return u->id==id;}),wus.end());}
 void add(Unit*u){++added;delete u;}
};
struct App {CPU cpu;Units wus;unsigned published=0;
 bool shouldQuit()const{return false;}CPU&getCPUResources(){return cpu;}
 Units*getUnits(){return &wus;}void updateCPUInfo(){++published;}unsigned getNextWUID(){return 1;}
};
struct Event {unsigned waits=0;void add(double){++waits;}};
struct Group {
 App app;Config c;Config*config=&c;Event e;Event*event=&e;string name="A";
 double waitUntil=0;function<void()> shutdownCB;
 auto units(){return app.wus.wus;}bool waitForIdle(){return false;}
 bool waitOnBattery(){return false;}bool waitOnGPU(){return false;}
 bool isAssigning(){for(auto u:units())if(u->state==UNIT_ASSIGN)return true;return false;}
 void triggerUpdate(){}
"""+body+r"""
};
int main(){
 Group g;auto running=make_shared<Unit>();running->id="running";
 auto pending=make_shared<Unit>();pending->id="pending";pending->state=UNIT_ASSIGN;
 pending->gpus={"gpu"};pending->cpus=pending->minimum=pending->maximum=1;
 g.app.wus.wus={running,pending};
 g.update();assert(running->pool==set<unsigned>({4,5,6,7}));assert(running->cpus==2&&!running->paused);
 assert(g.app.published==1&&g.app.wus.added==0&&g.e.waits==1);
 // Remain in assignment/retry state while shrinking the CPU budget to zero.
 g.app.cpu.pool.clear();g.app.cpu.workers=0;g.update();
 assert(running->pool.empty()&&running->paused);assert(g.app.wus.added==0&&g.e.waits==2);
 // Ordinary scheduling resumes after the assignment completes.
 pending->state=UNIT_RUN;g.app.cpu.pool={0,2,1,3};g.app.cpu.workers=2;g.update();
 assert(running->pool==set<unsigned>({0,1,2,3})&&!running->paused);
 assert(g.app.wus.added==0&&g.e.waits==2);
 cout<<"PASS: pending/retrying assignments reconcile changed and empty CPU pools without requesting extra WUs\n";
}
"""
with tempfile.TemporaryDirectory(prefix='fah-assignment-reconciliation-') as d:
 p=Path(d);(p/'test.cpp').write_text(harness)
 source=str(root/'src/fah/client/CPUExecutionPlan.cpp')
 if os.name=='nt':cmd=['cl','/nologo','/EHsc','/std:c++17','/I'+str(root/'src'),'test.cpp',source,'/Fetest.exe'];exe=p/'test.exe'
 else:cmd=[os.environ.get('CXX','c++'),'-std=c++17','-I'+str(root/'src'),'test.cpp',source,'-o','test'];exe=p/'test'
 subprocess.run(cmd,cwd=p,check=True);subprocess.run([str(exe)],check=True)
