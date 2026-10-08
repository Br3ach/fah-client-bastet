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

"""Compile production desired/live affinity, launch exclusion and restart logic."""
from pathlib import Path
from compile_harness import compile_and_run
source = (Path(__file__).resolve().parents[2] / 'src/fah/client/Unit.cpp').read_text()
a = source.index('bool Unit::desiredGPUReservation()')
b = source.index('void Unit::run()', a)
methods = source[a:b]
a = source.index('  // Monitor running core process')
b = source.index('  // Handle pause', a)
monitor = source[a:b]
a = source.index('  // One immutable record drives', source.index('void Unit::run()'))
b = source.index('  auto units = app.getUnits();', a)
launch_gate = source[a:b]
a = source.index('  if (allocation.isManaged()) {', source.index('Unit::createCoreProcess('))
b = source.index('  return process;', a)
affinity_setup = source[a:b].replace('allocation.', 'desired.')
a = source.index('  if (!hasGPUs() && desired.isManaged() && desired.fullSMT)', source.index('void Unit::run()'))
b = source.index('  startLogCopy(logFile);', a)
launch_commit = affinity_setup + source[a:b]
a = source.index('  } else { // CPU', source.index('Unit::buildCoreArgs('))
b = source.index('  return args;', a)
worker_args = source[a:b].split('\n',1)[1].rsplit('  }',1)[0].replace('allocation.', 'desired.')
harness = r'''
#include <fah/client/CPUExecutionPlan.h>
#include <fah/client/CPUExecutionPlan.h>
#include <fah/client/RunningCPUAllocation.h>
#include <fah/client/CPUOwnershipPolicy.h>
using FAH::Client::CPUOwnershipPolicy;
#include <set>
#include <vector>
#include <string>
#include <memory>
#include <cassert>
#include <iostream>
#include <map>
using namespace std;using FAH::Client::CPUAllocationSlices;
using FAH::Client::CPUExecutionPlan;using FAH::Client::RunningCPUAllocation;using FAH::Client::CPUAllocationMode;
#define LOG_DEBUG(a,b) do{}while(false)
#define LOG_WARNING(b) do{++warnings;}while(false)
#define LOG_INFO(a,b) do{}while(false)
const int UNIT_RUN=4;
struct CPU {
 unsigned generation=1;mutable unsigned planReads=0,generationReads=0;
 uint64_t getAllocationGeneration()const{++generationReads;return generation;}
 bool supported=true,hard=true;vector<set<unsigned>> levels={{0,1,2,3},{4,5}};set<unsigned> mask={0,1,2,3};map<string,set<unsigned>> gpuMasks;
 bool supportsGPUAffinity()const{return hard;}bool hasPerformanceClasses()const{return supported;}
 const auto &getPerformanceLevels()const{return levels;}
 const auto &getGPUCPUs(const string &,const string &id)const{auto it=gpuMasks.find(id);return it==gpuMasks.end()?mask:it->second;}
 vector<set<unsigned>> getCoreThreads()const{++planReads;return {{0,1},{2,3},{4,5}};}
 vector<unsigned> getGroupCPUs(const string &)const{return {0,2,4,1,3,5};}
};
struct Unit;
struct Units {vector<shared_ptr<Unit>> values;unsigned size()const{return values.size();}auto getUnit(unsigned i){return values.at(i);}};
struct App {shared_ptr<Units> units=make_shared<Units>();auto getUnits(){return units;}unsigned updates=0;void triggerUpdate(){++updates;}CPU cpu;const CPU &getCPUResources()const{return cpu;}};
struct Config {unsigned reserved=0;unsigned getGPUReservedCores()const{return reserved;}};
struct Group {string name;string getName()const{return name;}};
struct Process {bool live=true,stopping=false;bool isSet()const{return live;}bool isRunning()const{return live;}bool isStopping()const{return stopping;}Process *operator->(){return this;}};
struct Core {bool isSet()const{return true;}const Core *operator->()const{return this;}unsigned getType()const{return 0xa8;}};
string String(unsigned n){return to_string(n);}
struct LaunchProcess {
 bool reject=false;set<unsigned>mask;unsigned executions=0;vector<string>arguments;
 void setRequiredAffinity(const set<unsigned>&m){mask=m;}
 void exec(const vector<string>&args){arguments=args;++executions;if(reject)throw 7;}
};
struct Unit {bool isAssigning()const{return false;}
 App app;Config config;Process process;Core core;shared_ptr<Group> group=make_shared<Group>();
 bool gpu=true,paused=false,affinityManaged=false;bool affinityClassMode=false;CPUAllocationSlices affinitySlices;
 FAH::Client::RunningCPUAllocation runningAllocation;
 uint64_t affinityAllocationGeneration=1;set<unsigned>attemptedAffinityCPUs;
 bool allocationBlockedLogged=false,launchReady=false;unsigned retries=0,warnings=0;
 unsigned cpus=0,delta=10,stops=0;int state=UNIT_RUN;set<unsigned> affinityCPUs;
 set<string> gpuIDs={"gpu"};set<string> getGPUs()const{return gpu?gpuIDs:set<string>{};}
 const Config &getConfig()const{return config;}bool hasGPUs()const{return gpu;}
 unsigned getScheduledCPUs()const{return cpus;}bool isPaused()const{return paused;}int getState()const{return state;}
 unsigned getRunTimeDelta()const{return delta;}void triggerNext(unsigned seconds){assert(seconds==1);++retries;}void finalizeRun(){}int save(){return 0;}
 int stopRun(){++stops;return 1;}int monitorRun(){return 0;}
 bool desiredGPUReservation()const;bool desiredAffinityManaged()const;
 optional<CPUExecutionPlan::SliceExecution> buildCPUExecutionPlan()const;
 RunningCPUAllocation buildDesiredAllocation()const;
 bool blocksCPULaunch(const Unit&,const RunningCPUAllocation&)const;
 bool blocksCPULaunch(const Unit&u)const{return blocksCPULaunch(u,buildDesiredAllocation());}
 set<unsigned>getDesiredAffinity()const{return buildDesiredAllocation().mask;}
 int checkRun();void checkLaunchGate();
 void launchSnapshot(const RunningCPUAllocation&,LaunchProcess*);
 void processStarted(LaunchProcess*){process.live=true;}
 void launched(){runningAllocation=buildDesiredAllocation();}
};
''' + methods + '\nint Unit::checkRun(){\n' + monitor + 'return -1;}\nvoid Unit::checkLaunchGate(){\n' + launch_gate + 'launchReady=true;}\nvoid Unit::launchSnapshot(const RunningCPUAllocation &desired, LaunchProcess *process){vector<string>args;\n' + worker_args + launch_commit + '}\n' + r'''
int main() {
 {Unit unit;unit.gpu=false;unit.affinityManaged=true;unit.affinityClassMode=true;
  unit.cpus=3;unit.affinityCPUs={0,1,2,3};unit.affinitySlices={{1,{0,1}},{2,{2,3}}};
  auto desired=unit.buildDesiredAllocation();
  assert(desired.mask==set<unsigned>({0,2,3}));assert(desired.workers==3);
  unit.launched();auto frozen=unit.runningAllocation;
  unit.affinitySlices={{2,{0,1}},{1,{2,3}}};
  assert(unit.buildDesiredAllocation().mask==set<unsigned>({0,1,2}));
  assert(unit.runningAllocation.mask==frozen.mask&&unit.runningAllocation.resourcePool==frozen.resourcePool);
  unit.affinitySlices.clear();assert(unit.buildDesiredAllocation().mask.empty());
 }

 {Unit first,second;first.config.reserved=second.config.reserved=1;
  first.gpuIDs={"GPU0"};second.gpuIDs={"GPU1"};
  first.app.cpu.gpuMasks={{"GPU0",{0,1}},{"GPU1",{}}};second.app.cpu=first.app.cpu;
  assert(first.buildDesiredAllocation().mask==set<unsigned>({0,1}));
  assert(second.buildDesiredAllocation().mask.empty());
  first.app.cpu.gpuMasks["GPU1"]={2,3};second.app.cpu=first.app.cpu;
  first.launched();second.launched();
  assert(second.runningAllocation.mask==set<unsigned>({2,3}));
  assert(!first.blocksCPULaunch(second)&&!second.blocksCPULaunch(first));
  first.gpuIDs={"GPU0","GPU1"};assert(first.buildDesiredAllocation().mask.empty());
  cout<<"PASS: GPU0 shortage independence, disjoint GPU WU reservations and multi-device ownership rejection\n";
 }

 // GPU helper accounting adopts the current generation without restarting.
 for(unsigned reservedCount:{0u,1u})for(bool supported:{false,true}) {
  if(reservedCount && !supported)continue;
  Unit u;u.config.reserved=reservedCount;u.app.cpu.supported=supported;
  u.cpus=1;u.launched();const auto mask=u.runningAllocation.mask;
  u.cpus=3;u.app.cpu.generation=2;
  assert(u.checkRun()==0 && u.stops==0);
  assert(u.runningAllocation.workers==3 && u.runningAllocation.generation==2);
  assert(u.runningAllocation.mask==mask);
 }
 // Conflict and stopping gates retain frozen GPU accounting as well as pools.
 {
  auto owner=make_shared<Unit>();owner->config.reserved=1;
  owner->cpus=1;owner->launched();
  auto peer=make_shared<Unit>();peer->gpu=false;peer->launched();
  owner->app.units->values={owner,peer};
  owner->cpus=3;owner->app.cpu.generation=2;
  assert(owner->checkRun()==0 && owner->stops==0);
  assert(owner->runningAllocation.workers==1 && owner->runningAllocation.generation==1);
  peer->process.live=false;owner->process.stopping=true;
  assert(owner->checkRun()==1);
  assert(owner->runningAllocation.workers==1 && owner->runningAllocation.generation==1);
  owner->process.stopping=false;
  assert(owner->checkRun()==0 && owner->runningAllocation.workers==3);
  assert(owner->runningAllocation.generation==2);
 }
 // Pool-only changes preserve execution; conflicts retain frozen old ownership.
 {
  auto owner=make_shared<Unit>();owner->gpu=false;owner->affinityManaged=true;
  owner->cpus=1;owner->affinityCPUs={0,1};owner->launched();
  auto peer=make_shared<Unit>();peer->gpu=false;peer->affinityManaged=true;
  peer->cpus=1;peer->affinityCPUs={2,3};peer->launched();
  owner->app.units->values={owner,peer};owner->affinityCPUs={0,1,2,3};
  assert(owner->checkRun()==0 && owner->stops==0 && owner->runningAllocation.resourcePool==set<unsigned>({0,1}));
  peer->process.live=false;
  assert(owner->checkRun()==0 && owner->stops==0 && owner->runningAllocation.resourcePool==set<unsigned>({0,1,2,3}));
  owner->affinityCPUs={0,1};
  assert(owner->checkRun()==0 && owner->stops==0 && owner->runningAllocation.resourcePool==set<unsigned>({0,1}));
  owner->process.stopping=true;owner->affinityCPUs={0,1,2,3};
  assert(owner->checkRun()==1 && owner->runningAllocation.resourcePool==set<unsigned>({0,1}));
 }
 // Build once: the same CPU plan supplies mask, whole-core ownership and SMT.
 Unit planned;planned.gpu=false;planned.affinityManaged=true;
 planned.affinityCPUs={0,1,2,3,4,5};planned.cpus=6;
 const auto snapshot=planned.buildDesiredAllocation();
 assert(planned.app.cpu.planReads==1 && planned.app.cpu.generationReads==1);
 assert(snapshot.mask==planned.affinityCPUs && snapshot.resourcePool==planned.affinityCPUs);
 assert(snapshot.workers==6 && snapshot.physical==3 && snapshot.mask.size()==6 && snapshot.fullSMT);
 assert(snapshot.generation==1 && snapshot.isManaged() && !snapshot.isReservedGPU());
 planned.cpus=1;planned.affinityCPUs={4,5};planned.app.cpu.generation=2;
 Unit occupied;occupied.gpu=false;occupied.runningAllocation.mode=FAH::Client::CPUAllocationMode::ManagedCPU;
 occupied.runningAllocation.resourcePool={0,1};
 assert(planned.blocksCPULaunch(occupied,snapshot)); // Frozen pool, not new desired settings.
 const auto retry=planned.buildDesiredAllocation();
 assert(retry.generation==2 && retry.workers==1 && retry.mask==set<unsigned>{4});
 assert(!retry.fullSMT && retry.physical==1 && retry.resourcePool.size()==2 && retry.mask.size()==1);
 assert(!planned.blocksCPULaunch(occupied,retry));
 // A small process mask still owns all siblings in the physical pool.
 Unit narrow;narrow.gpu=false;narrow.affinityManaged=true;narrow.cpus=1;
 narrow.affinityCPUs={0,1};const auto one=narrow.buildDesiredAllocation();
 assert(one.mask==set<unsigned>{0} && one.resourcePool==set<unsigned>({0,1}));
 occupied.runningAllocation.resourcePool={1};assert(narrow.blocksCPULaunch(occupied,one));
 // An outdated CPU scheduler pool waits, then a fresh retry can proceed.
 planned.process.live=false;planned.app.cpu.planReads=planned.app.cpu.generationReads=0;planned.checkLaunchGate();
 assert(planned.app.cpu.planReads==1 && planned.app.cpu.generationReads==1);
 assert(!planned.launchReady && planned.retries==1 && planned.app.updates==1);
 planned.affinityAllocationGeneration=2;planned.checkLaunchGate();
 assert(planned.launchReady);
 // Execution commits the frozen record only after success. Rejected attempts
 // retain their mask independently and leave live ownership untouched.
 Unit committed;committed.gpu=false;LaunchProcess child;child.reject=true;
 try{committed.launchSnapshot(snapshot,&child);assert(false);}catch(int){}
 assert(committed.attemptedAffinityCPUs==snapshot.mask);
 assert(committed.runningAllocation.mask.empty() && committed.runningAllocation.generation==0);
 child.reject=false;committed.launchSnapshot(snapshot,&child);
 assert(child.arguments==vector<string>({"-np","6"}));
 assert(child.mask==snapshot.mask && committed.runningAllocation.mask==snapshot.mask);
 assert(committed.runningAllocation.resourcePool==snapshot.resourcePool);
 assert(committed.runningAllocation.workers==6 && committed.runningAllocation.generation==1);
 assert(committed.runningAllocation.fullSMT && committed.runningAllocation.physical==3);
 // Snapshot flags stay correct for legacy work and shared/reserved GPUs.
 Unit legacy;legacy.gpu=false;legacy.cpus=4;
 auto unmanaged=legacy.buildDesiredAllocation();
 assert(!unmanaged.isManaged() && unmanaged.mask.empty() && unmanaged.resourcePool.empty());
 Unit gpuSnapshot;gpuSnapshot.cpus=3;
 auto shared=gpuSnapshot.buildDesiredAllocation();
 assert(shared.isManaged() && !shared.isReservedGPU() && shared.mask==shared.resourcePool && !shared.fullSMT);
 gpuSnapshot.config.reserved=1;assert(gpuSnapshot.buildDesiredAllocation().isReservedGPU());
 cout<<"PASS: immutable launch snapshots, single CPU plan, frozen conflict masks, fresh retries, stale-pool wait, success-only ownership and attempted-mask rejection\n";
 // Zero gets a required Performance 1 mask independent of helper count.
 for(unsigned before:{0u,1u,4u})for(unsigned after:{0u,1u,4u}) {
  Unit u;u.cpus=before;u.launched();u.cpus=after;
  assert(u.desiredAffinityManaged());assert(u.getDesiredAffinity()==set<unsigned>({0,1,2,3}));
  assert(u.checkRun()==0 && u.stops==0);
 }
 // Exercise the actual Unit::run affinity gate as well as desired masks.
 // The scheduling suite sets these counts from min_cpus >= 2.
 for(unsigned minimum:{2u,3u,4u})for(unsigned reservedCount:{0u,1u}) {
  Unit small;small.cpus=minimum;small.config.reserved=reservedCount;
  small.app.cpu.mask={0};small.process.live=false;
  small.checkLaunchGate();assert(small.launchReady && small.retries==0);
  assert(small.getDesiredAffinity()==set<unsigned>{0});
  small.launched();assert(small.runningAllocation.workers==minimum);
  assert(small.runningAllocation.mask==set<unsigned>{0});
  small.launchReady=false;small.app.cpu.mask.clear();small.checkLaunchGate();
  assert(!small.launchReady && small.retries==1 && small.warnings==1);
  small.checkLaunchGate();assert(small.retries==2 && small.warnings==1);
  small.app.cpu.mask={0};small.checkLaunchGate();
  assert(small.launchReady && !small.allocationBlockedLogged);
 }
 // Reserved GPU processes stay running after launch (no desired/live mode mismatch).
 Unit reserved;reserved.config.reserved=1;reserved.app.cpu.mask={0,2};reserved.launched();
 assert(reserved.runningAllocation.isReservedGPU() && reserved.checkRun()==0);
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
 cpu.runningAllocation.workers=6;assert(cpu.checkRun()==1);
 Unit stableCPU;stableCPU.gpu=false;stableCPU.cpus=stableCPU.runningAllocation.workers;assert(stableCPU.checkRun()==0);
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
 old.runningAllocation.mode=FAH::Client::CPUAllocationMode::SharedGPU;assert(!next.blocksCPULaunch(old));
 Unit paused;paused.launched();paused.paused=true;assert(paused.checkRun()==1);
 Unit state;state.launched();state.state=UNIT_RUN+1;assert(state.checkRun()==1);
 Unit stopping;stopping.process.stopping=true;assert(stopping.checkRun()==1);
 Unit grace;grace.launched();grace.app.cpu.mask={1,3};grace.delta=4;assert(grace.checkRun()==0 && grace.stops==0);
 cout<<"PASS: zero/shared hard affinity, stable reserved GPU, reservation/mask changes, unmanaged CPU scheduling, CPU/GPU exclusion, separate same-RG GPU reservations, shutdown grace\n";
}
'''
compile_and_run(harness, ['CPUOwnershipPolicy.cpp', 'CPUExecutionPlan.cpp', 'CPUWholeCorePacking.cpp'])
