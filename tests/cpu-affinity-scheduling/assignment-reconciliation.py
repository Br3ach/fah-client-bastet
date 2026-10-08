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

"""Run the complete production Group::update with a pending/retrying assignment."""
from pathlib import Path
from compile_harness import compile_and_run
root=Path(__file__).resolve().parents[2]
s=(root/'src/fah/client/Group.cpp').read_text()
a=s.index('void Group::update() {');b=s.index('void Group::setWait(',a)
body=s[a:b].strip().replace('void Group::update()', 'void update()').replace('void Group::updateAssignmentOffer(', 'void updateAssignmentOffer(')
unit_source=(root/'src/fah/client/Unit.cpp').read_text()
reset_start=unit_source.index('if (getState() == UNIT_DOWNLOAD)',unit_source.index('case HTTP_SERVICE_UNAVAILABLE:'))
reset=unit_source[reset_start:unit_source.index('retry();',reset_start)]
request_cpu=unit_source[unit_source.index('sink.insert("cpus",',unit_source.index('void Unit::writeRequest')):].splitlines()[0]
harness=r"""
#include <fah/client/CPUExecutionPlan.h>
#include <fah/client/CPUAllocationPlanner.h>
#include <fah/client/WUCPUAllocationPlanner.h>
#include <algorithm>
#include <cassert>
#include <stdexcept>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>
using FAH::Client::CPUWholeCorePacking;
using namespace std; using FAH::Client::CPUExecutionPlan;using FAH::Client::WUCPUAllocationPlanner;using FAH::Client::CPUAllocationSlices;
#define LOG_DEBUG(a,b) do{}while(false)
#define LOG_INFO(a,b) do{}while(false)
enum UnitState {UNIT_ASSIGN,UNIT_DOWNLOAD,UNIT_CORE,UNIT_RUN,UNIT_UPLOAD,UNIT_DUMP,UNIT_DONE};
struct Time {static double now(){return 10;}};
struct App;
struct Unit {
 string id;UnitState state=UNIT_RUN;unsigned cpus=2,minimum=1,maximum=8;
 bool paused=false,managed=true;set<unsigned> pool={0,1,2,3};set<string> gpus;
 Unit()=default;Unit(App&,const string&,unsigned,unsigned n,const set<string>&ids):cpus(n),gpus(ids){}
 string getID()const{return id;}UnitState getState()const{return state;}
 bool hasGPUs()const{return !gpus.empty();}auto getGPUs()const{return gpus;}
 unsigned getMinCPUs()const{return minimum;}unsigned getMaxCPUs()const{return maximum;}
 unsigned getScheduledCPUs()const{return cpus;}void setScheduledCPUs(unsigned n){cpus=n;}
 void setCPUAffinity(bool m,const set<unsigned>&p,const CPUAllocationSlices *s=nullptr,bool classMode=false){managed=m;pool=p;}
 bool atRunState()const{return state==UNIT_RUN;}void setPause(bool p){paused=p;}
 bool isAssigning()const{return state==UNIT_ASSIGN;}
 void setState(UnitState n){state=n;}
 struct Data {void release(){}} data;
 void resetDownload(){
// DOWNLOAD_REASSIGNMENT_IMPLEMENTATION
 }
 unsigned requestCPUs()const{
  struct Sink {unsigned cpus=999;void insert(const string&,unsigned n){cpus=n;}} sink;
// REQUEST_CPU_IMPLEMENTATION
  return sink.cpus;
 }
 bool matchesAssignmentOffer(unsigned n,const set<string>&ids)const{return cpus==n&&gpus==ids;}
 void abortPendingAssignment(){state=UNIT_DONE;}
 bool isRunning()const{return state==UNIT_RUN;}void triggerNext(){}void save(){}
};
struct CPU {CPUAllocationSlices slices;const CPUAllocationSlices &getGroupAllocationSlices(const string&)const{return slices;}
 vector<unsigned> pool={4,6,5,7};unsigned workers=2;bool managed=true;
 bool gpuAffinity=false;map<string,set<unsigned>> gpuMasks;map<string,string> shortages;
 bool supportsGPUAffinity()const{return gpuAffinity;}
 const set<unsigned>&getGPUCPUs(const string&,const string&id)const{
  static const set<unsigned> empty;auto it=gpuMasks.find(id);return it==gpuMasks.end()?empty:it->second;}
 const string&getGPUAllocationShortage(const string&,const string&id)const{
  static const string empty;auto it=shortages.find(id);return it==shortages.end()?empty:it->second;}
 vector<set<unsigned>> cores={{0,1},{2,3},{4,5},{6,7}};
 bool isManaged()const{return managed;}auto getGroupCPUs(const string&)const{return pool;}
 unsigned getGroupWorkerCount(const string&)const{return workers;}
 auto &getCoreThreads()const{return cores;}
};
struct Config {
 bool paused=false,classes=false;unsigned cpus=2,reserved=0;
 unsigned getGPUReservedCores()const{return reserved;}
 bool usesCPUClasses()const{return classes;}bool getPaused()const{return paused;}void setPaused(bool p){paused=p;}
 set<string> gpus={"gpu"};
 unsigned getCPUs()const{return cpus;}set<string> getGPUs()const{return gpus;}
 bool getFinish()const{return false;}
};
struct Units {vector<shared_ptr<Unit>> wus;unsigned added=0;vector<set<string>> offered;vector<unsigned> offeredCPUs;
 void removeUnit(const string&id){wus.erase(remove_if(wus.begin(),wus.end(),[&](auto u){return u->id==id;}),wus.end());}
 void add(Unit*u){++added;offeredCPUs.push_back(u->cpus);offered.push_back(u->gpus);delete u;}
};
struct App {CPU cpu;Units wus;unsigned published=0;
 bool shouldQuit()const{return false;}CPU&getCPUResources(){return cpu;}
 void triggerUpdate(){}Units*getUnits(){return &wus;}void updateCPUInfo(){++published;}unsigned getNextWUID(){return 1;}
};
struct Event {unsigned waits=0;void add(double){++waits;}};
struct Group {
 App app;Config c;Config*config=&c;Event e;Event*event=&e;string name="A";
 double waitUntil=0;bool lastResourceDemand=true;function<void()> shutdownCB;
 auto units(){return app.wus.wus;}bool waitForIdle(){return false;}
 bool waitOnBattery(){return false;}bool waitOnGPU(){return false;}
 bool waitForRetry(){return Time::now()<waitUntil;}bool wantsResources(){return !config->getPaused()&&!waitForRetry();}
 bool isAssigning(){for(auto u:units())if(u->state==UNIT_ASSIGN)return true;return false;}
 void triggerUpdate(){}
"""+body+r"""
};
int main(){
 // GPU-only offers must not acquire CPU workers through helper accounting.
 for(bool managed:{false,true}) {
  Group gpu;gpu.c.cpus=0;gpu.c.reserved=managed?1:0;
  gpu.app.cpu.managed=managed;gpu.app.cpu.workers=0;gpu.app.cpu.pool.clear();
  gpu.app.cpu.gpuAffinity=managed;gpu.app.cpu.gpuMasks["gpu"]={0,1};
  auto pending=make_shared<Unit>();pending->id="GPU-offer";pending->state=UNIT_ASSIGN;
  pending->cpus=pending->minimum=0;pending->maximum=1;pending->gpus={"gpu"};
  gpu.app.wus.wus={pending};
  for(int n=0;n<3;++n) {
   gpu.update();assert(pending->requestCPUs()==0&&pending->state==UNIT_ASSIGN);
   assert(gpu.app.wus.added==0);
  }
  // Accepted GPU preparation may account for helpers. A 503 must cause
  // that stale CPU offer to be replaced before another assignment is sent.
  pending->state=UNIT_DOWNLOAD;pending->minimum=1;pending->cpus=1;
  gpu.update();assert(gpu.app.wus.added==0&&pending->gpus==set<string>{"gpu"});
  pending->resetDownload();assert(pending->state==UNIT_ASSIGN);
  gpu.update();assert(pending->state==UNIT_DONE&&gpu.app.wus.added==1);
  assert(gpu.app.wus.offeredCPUs.back()==0&&gpu.app.wus.offered.back()==set<string>{"gpu"});
  auto rebuilt=make_shared<Unit>();rebuilt->id="rebuilt";rebuilt->state=UNIT_ASSIGN;
  rebuilt->cpus=gpu.app.wus.offeredCPUs.back();rebuilt->gpus=gpu.app.wus.offered.back();
  gpu.app.wus.wus={rebuilt};gpu.update();assert(rebuilt->requestCPUs()==0);
  assert(gpu.app.wus.added==1&&rebuilt->state==UNIT_ASSIGN);
  rebuilt->state=UNIT_RUN;rebuilt->minimum=rebuilt->maximum=1;
  gpu.update();assert(rebuilt->cpus==1&&!rebuilt->paused&&gpu.app.wus.added==1);
 }
 cout<<"PASS: GPU-only serialized offers stay zero; production download-503 reset rebuilds helper-contaminated offers before reassignment\n";

 {
  using Planner=FAH::Client::CPUAllocationPlanner;
  Planner::Topology topology;topology.hardAffinity=topology.effectiveClasses=true;
  topology.available={0,1,2,3};topology.performanceLevels={{0,1},{2,3}};
  topology.coreThreads={{0},{1},{2},{3}};topology.fastPhysicalCores={{0},{1}};
  Planner::Request zero;zero.name="Zero";zero.classes=true;zero.classCounts={0,0};
  Planner::Request gpu;gpu.name="A";gpu.gpus={"gpu"};gpu.reservedCores=1;
  auto result=Planner::plan(topology,{zero,gpu});
  assert(!result.runtimeFallback);
  Group group;group.c.cpus=0;group.c.reserved=1;
  group.app.cpu.managed=result.managed;group.app.cpu.workers=result.workerBudgets.at("A");
  group.app.cpu.pool=result.allocations.count("A")?result.allocations.at("A"):vector<unsigned>{};
  group.app.cpu.cores=topology.coreThreads;group.app.cpu.gpuAffinity=true;
  group.app.cpu.gpuMasks=result.gpuDeviceAllocations.at("A");
  group.update();
  assert(group.app.wus.added==1&&group.app.wus.offered.back()==set<string>{"gpu"});
  cout<<"PASS: real planner zero-class publication preserves production Group GPU assignment eligibility\n";
 }
 // Test acquisition with required shared affinity and exclusive reservations,
 // including reservation failure when global managed affinity is unavailable.
 for(bool exclusive:{false,true}) {
  Group g;g.c.gpus={"GPU0","GPU1"};g.c.cpus=0;g.app.cpu.workers=0;
  g.app.cpu.gpuAffinity=!exclusive;g.c.reserved=exclusive?1:0;
  g.app.cpu.managed=!exclusive;g.app.cpu.gpuMasks["GPU0"]={0,1};
  g.update();assert(g.app.wus.added==1&&g.app.wus.offered.back()==set<string>{"GPU0"});
  g.app.cpu.gpuMasks["GPU1"]={2,3};g.app.cpu.shortages["GPU1"]="shortage";
  g.update();assert(g.app.wus.offered.back()==set<string>{"GPU0"});
  auto pending=make_shared<Unit>();pending->state=UNIT_ASSIGN;pending->cpus=0;
  pending->gpus={"GPU0","GPU1"};g.app.wus.wus={pending};g.update();
  assert(pending->state==UNIT_DONE&&g.app.wus.offered.back()==set<string>{"GPU0"});
  auto existing=make_shared<Unit>();existing->id="accepted";existing->gpus={"GPU1"};
  g.app.wus.wus={existing};g.update();assert(existing->gpus==set<string>{"GPU1"});
  assert(g.app.wus.offered.back()==set<string>{"GPU0"});
  g.app.wus.wus.clear();g.app.cpu.gpuMasks.clear();
  auto before=g.app.wus.added;g.update();assert(g.app.wus.added==before);
  g.app.cpu.gpuMasks["GPU1"]={2,3};g.app.cpu.shortages.clear();g.update();
  assert(g.app.wus.added==before+1&&g.app.wus.offered.back()==set<string>{"GPU1"});
 }
 {Group legacy;legacy.c.gpus={"GPU0","GPU1"};legacy.c.cpus=0;
  legacy.app.cpu.workers=0;legacy.app.cpu.managed=false;legacy.update();
  assert(legacy.app.wus.added==1&&legacy.app.wus.offered.back()==legacy.c.gpus);}
 cout<<"PASS: blocked GPU offers filtered, shortages/recovery, existing assignments, pending rebuild and legacy eligibility\n";

 {Group one;one.c.gpus={"GPU0","GPU1"};one.app.cpu.workers=0;
  auto ready=make_shared<Unit>();ready->id="GPU0-WU";ready->gpus={"GPU0"};
  one.app.wus.wus={ready};one.update();assert(!ready->paused);
  assert(one.app.wus.added==1&&one.app.wus.offered[0]==set<string>{"GPU1"});
  Group two;two.c.gpus={"GPU0","GPU1"};two.app.cpu.workers=0;
  auto other=make_shared<Unit>();other->id="GPU1-WU";other->gpus={"GPU1"};
  two.app.wus.wus={ready,other};two.update();
  assert(!ready->paused&&!other->paused&&two.app.wus.added==0);
  Group waiting;waiting.c.gpus={"GPU0","GPU1"};waiting.app.cpu.workers=0;
  auto pending=make_shared<Unit>();pending->id="download";pending->state=UNIT_DOWNLOAD;
  pending->gpus={"GPU0"};waiting.app.wus.wus={pending};waiting.update();
  assert(waiting.app.wus.added==1&&waiting.app.wus.offered[0]==set<string>{"GPU1"});
  pending->state=UNIT_CORE;waiting.app.wus.added=0;waiting.update();assert(waiting.app.wus.added==1);
  cout<<"PASS: production Group scheduling releases unassigned GPU1 after resolution, allows two ready GPU WUs, and releases unselected GPUs during download/core preparation\n";
 }


 // Changed pending offers are rebuilt from residual capacity in both modes.
 for(bool managed:{false,true}) for(unsigned old:{4u,8u}) for(unsigned budget:{0u,4u,8u}) {
  Group resized;resized.c.gpus.clear();resized.c.cpus=budget;
  resized.app.cpu.managed=managed;resized.app.cpu.workers=budget;
  resized.app.cpu.pool={0,1,2,3,4,5,6,7};
  auto pending=make_shared<Unit>();pending->state=UNIT_ASSIGN;
  pending->cpus=pending->minimum=pending->maximum=old;pending->pool.clear();
  resized.app.wus.wus={pending};resized.update();
  assert(pending->cpus==old&&pending->pool.empty());
  if(old==budget) assert(pending->state==UNIT_ASSIGN&&resized.app.wus.added==0&&resized.e.waits==1);
  else {assert(pending->state==UNIT_DONE);assert(resized.app.wus.added==(budget?1u:0u));
   if(budget)assert(resized.app.wus.offeredCPUs[0]==budget);}
 }
 // Accepted CPU work below its minimum suppresses further CPU acquisition.
 for(bool managed:{false,true}) for(auto state:{UNIT_DOWNLOAD,UNIT_CORE,UNIT_RUN}) {
  Group blocked;blocked.c.gpus.clear();blocked.c.cpus=2;
  blocked.app.cpu.managed=managed;blocked.app.cpu.workers=2;
  blocked.app.cpu.pool={0,1,2,3,4,5};
  auto wu=make_shared<Unit>();wu->id="blocked";wu->state=state;
  wu->minimum=wu->maximum=4;blocked.app.wus.wus={wu};
  for(unsigned i=0;i<5;++i)blocked.update();
  assert(blocked.app.wus.added==0&&wu->paused);
  auto pending=make_shared<Unit>();pending->id="pending";pending->state=UNIT_ASSIGN;
  pending->cpus=2;blocked.app.wus.wus.push_back(pending);blocked.update();
  assert(pending->state==UNIT_DONE&&blocked.app.wus.added==0);
  blocked.c.gpus={"GPU0"};blocked.update();
  assert(blocked.app.wus.added==1&&blocked.app.wus.offeredCPUs.back()==0&&
    blocked.app.wus.offered.back()==set<string>{"GPU0"});
  blocked.c.gpus.clear();blocked.c.cpus=6;blocked.app.cpu.workers=6;
  blocked.update();assert(!wu->paused&&wu->cpus==4&&blocked.app.wus.offeredCPUs.back()==2);
  wu->state=UNIT_UPLOAD;blocked.c.cpus=2;blocked.app.cpu.workers=2;
  const auto before=blocked.app.wus.added;blocked.update();
  assert(blocked.app.wus.added==before+1&&blocked.app.wus.offeredCPUs.back()==2);
 }
 cout<<"PASS: blocked accepted CPU work prevents accumulation, cancels CPU offers, preserves GPUs and resumes acquisition on recovery/completion\n";
 // Zero-budget CPU preparation pauses, including an in-flight completion
 // moving DOWNLOAD to CORE. Recovery resumes work without claiming cores early.
 for(bool managed:{false,true}) {
  Group preparation;preparation.c.gpus.clear();preparation.c.cpus=0;
  preparation.app.cpu.managed=managed;preparation.app.cpu.workers=0;
  auto cpu=make_shared<Unit>();cpu->id="cpu";cpu->state=UNIT_DOWNLOAD;
  preparation.app.wus.wus={cpu};preparation.update();assert(cpu->paused);
  cpu->state=UNIT_CORE;preparation.update();assert(cpu->paused);
  preparation.c.cpus=2;preparation.app.cpu.workers=2;
  preparation.update();assert(!cpu->paused&&cpu->cpus==2);
  if(managed)assert(cpu->pool.empty());
  for(auto state:{UNIT_UPLOAD,UNIT_DUMP,UNIT_DONE}) {
   preparation.c.cpus=0;preparation.app.cpu.workers=0;
   cpu->state=state;preparation.update();assert(!cpu->paused);
  }
  for(auto state:{UNIT_DOWNLOAD,UNIT_CORE}) {
   Group gpu;gpu.c.gpus={"GPU0"};gpu.c.cpus=0;
   gpu.app.cpu.managed=managed;gpu.app.cpu.workers=0;
   auto unit=make_shared<Unit>();unit->state=state;unit->gpus={"GPU0"};
   unit->minimum=unit->maximum=4;gpu.app.wus.wus={unit};gpu.update();
   assert(!unit->paused);
  }
 }
 cout<<"PASS: unallocated CPU preparation pauses and recovers; GPU preparation and upload/dump/completion remain unaffected\n";
 // Unsupported class topology never turns saved class work into legacy work.
 Group unsupported;auto blocked=make_shared<Unit>();
 unsupported.c.classes=true;unsupported.c.gpus.clear();unsupported.app.cpu.managed=false;
 unsupported.app.cpu.pool.clear();unsupported.app.cpu.workers=0;
 unsupported.app.wus.wus={blocked};unsupported.update();
 assert(blocked->paused&&blocked->pool.empty()&&blocked->cpus==0);
 // A topology-rejected minimum releases cores to the surviving WU, not a
 // redundant new assignment. Genuine unused budget still requests work.
 Group reduced;auto a=make_shared<Unit>(),b=make_shared<Unit>();
 a->id="Z";a->minimum=a->maximum=3;b->id="A";b->minimum=1;b->maximum=3;
 reduced.c.gpus.clear();reduced.app.wus.wus={a,b};reduced.app.cpu.pool={0,2,1,3};
 reduced.app.cpu.cores={{0,1},{2,3}};reduced.app.cpu.workers=4;
 reduced.update();assert(a->paused&&a->pool.empty()&&b->cpus==3&&b->pool.size()==4);
 assert(reduced.app.wus.added==0);
 Group spare;auto only=make_shared<Unit>();only->id="only";only->maximum=2;
 spare.c.gpus.clear();spare.app.wus.wus={only};spare.app.cpu.pool={0,2,1,3};
 spare.app.cpu.cores={{0,1},{2,3}};spare.app.cpu.workers=4;
 spare.update();assert(only->cpus==2&&spare.app.wus.added==1);
 Group deferred;auto live=make_shared<Unit>(),waiting=make_shared<Unit>();
 live->id="live";live->maximum=2;waiting->id="waiting";
 waiting->minimum=waiting->maximum=2;waiting->state=UNIT_ASSIGN;waiting->pool.clear();
 deferred.c.gpus.clear();deferred.app.wus.wus={live,waiting};
 deferred.app.cpu.pool={0,2,1,3};deferred.app.cpu.cores={{0,1},{2,3}};
 deferred.app.cpu.workers=4;
 deferred.update();assert(live->pool.size()==4&&waiting->pool.empty()&&waiting->cpus==2);
 waiting->state=UNIT_DOWNLOAD;deferred.update();assert(live->pool.size()==4&&waiting->pool.empty());
 waiting->state=UNIT_CORE;deferred.update();assert(live->pool.size()==4&&waiting->pool.empty());
 waiting->state=UNIT_RUN;deferred.update();assert(live->pool.size()==2&&waiting->pool.size()==2);
 // Unused cores are not live ownership: another ready WU need not restart its peer.
 Group stable;auto first=make_shared<Unit>(),second=make_shared<Unit>();
 first->id="first";first->minimum=first->maximum=1;
 second->id="second";second->minimum=second->maximum=1;second->cpus=7;second->state=UNIT_ASSIGN;second->pool.clear();
 stable.c.gpus.clear();stable.app.cpu.pool={0,2,4,6,1,3,5,7};
 stable.app.cpu.cores={{0,1},{2,3},{4,5},{6,7}};stable.app.cpu.workers=8;
 stable.app.wus.wus={first};stable.update();const auto originalPool=first->pool;
 assert(originalPool==set<unsigned>({0,1}));
 stable.app.wus.wus.push_back(second);stable.update();assert(first->pool==originalPool&&second->pool.empty());
 second->state=UNIT_CORE;stable.update();assert(first->pool==originalPool&&second->pool.empty());
 second->state=UNIT_RUN;stable.update();assert(first->pool==originalPool&&second->pool==set<unsigned>({2,3}));
 cout<<"PASS: pending/retrying assignments reconcile changed and empty CPU pools without requesting extra WUs\n";
}
"""
harness=harness.replace('// DOWNLOAD_REASSIGNMENT_IMPLEMENTATION',reset).replace('// REQUEST_CPU_IMPLEMENTATION',request_cpu)
compile_and_run(harness, ['WUCPUAllocationPlanner.cpp', 'CPUAllocationPlanner.cpp', 'CPUWholeCorePacking.cpp', 'CPUExecutionPlan.cpp'])
