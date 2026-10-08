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
from compile_harness import compile_and_run
root=Path(__file__).resolve().parents[2]
s=(root/'src/fah/client/Unit.cpp').read_text()
a=s.index('  } catch (const AffinityRejected &e) {');b=s.index(' CATCH_ERROR;',a)
catch=s[a:b]
a=s.index('  affinityRejections = 0;', s.index('void Unit::processStarted('))
b=s.index('  auto pid =',a)
reset=s[a:b]
a=s.index('bool Unit::isActive() const {');b=s.index('\n\nvoid Unit::setScheduledCPUs',a)
active=s[a:b].replace('bool Unit::isActive()', 'bool isActive()')
header=(root/'src/fah/client/Unit.h').read_text()
a=header.index('      bool hasLaunchFailure() const {');b=header.index('\n      }',a)+8
failure=header[a:b]

a=s.index('void Unit::stopRun() {');b=s.index('\n}',a)+2
stop_run=s[a:b].replace('void Unit::stopRun()', 'void stopRun()')
core=(root/'src/fah/client/CoreProcess.cpp').read_text()
a=core.index('void CoreProcess::stop() {');b=core.index('\n}',a)+2
stop_core=core[a:b].replace('void CoreProcess::stop()', 'void stop()')

a=s.index('void Unit::abortPendingAssignment() {');b=s.index('\n}',a)+2
abort_assignment=s[a:b].replace('void Unit::abortPendingAssignment()', 'void abortPendingAssignment()')
a=s.index('void Unit::response(HTTP::Request &req) {')
a=s.index('\n',a)+1;b=s.index('  try {',a)
response_guard=s[a:b]

cpp=r"""
#include <fah/client/RunningCPUAllocation.h>
#include <cassert>
#include <stdexcept>
#include <string>
#include <set>
#include <algorithm>
#include <cstdint>
#include <memory>
using namespace std;
const int UNIT_RUN=4;
#define TRY_CATCH_ERROR(x) try {x;} catch (...) {}
#define LOG_WARNING(x) (++warnings)
unsigned warnings=0;
struct Time {inline static uint64_t clock=100;static uint64_t now(){return clock;}};
struct StopProcess {
 bool killedByClient=false,failKill=false;uint64_t interruptTime=0,lastStop=0;
 unsigned interrupts=0,kills=0;
 void interrupt(){++interrupts;throw runtime_error("interrupt failed");}
 void kill(){++kills;if(failKill){failKill=false;throw runtime_error("kill failed");}}
"""+stop_core+r"""
};
struct StopUnit {
 StopProcess child;StopProcess *process=&child;unsigned armed=0;
 void triggerNext(unsigned seconds){armed=seconds;}
"""+stop_run+r"""
};

namespace HTTP {struct Request {};}
struct Pending {shared_ptr<HTTP::Request> request;auto &getRequest()const{return request;}};
struct RequestRef {shared_ptr<Pending> value;unsigned releases=0;bool isSet()const{return bool(value);}auto operator->()const{return value.get();}void release(){++releases;value.reset();}};
struct AssignmentUnit {
 RequestRef pr;bool assigning=true,failCleanup=false;unsigned armed=0,accepted=0,cleanups=0;
 bool isAssigning()const{return assigning;}void triggerNext(unsigned delay){armed=delay;}
 void cancelRequest(){pr.release();}void clean(const string&why){assert(why=="aborted");++cleanups;if(failCleanup)throw runtime_error("cleanup");assigning=false;}
"""+abort_assignment+r"""
 void response(HTTP::Request &req){
"""+response_guard+r"""
 ++accepted;}
};

struct CPUResources{static string formatCPUs(const set<unsigned>&){return "[0,2]";}};
struct SchedulingRejected:runtime_error{SchedulingRejected():runtime_error("scheduling rejected") {}};
struct AffinityRejected:runtime_error{AffinityRejected():runtime_error("mask rejected") {}};
struct Resources{bool fail=false;unsigned probes=0;uint64_t generation=1;uint64_t getTopologyGeneration(){return generation;}void refreshTopology(const string &why){assert(why=="affinity-rejected");probes++;if(fail)throw runtime_error("refresh");}};
struct App{Resources resources;bool fail=false;unsigned updates=0;Resources &getCPUResources(){return resources;}void triggerUpdate(){updates++;if(fail)throw runtime_error("update");}};
struct Unit{bool paused=false,running=false,waiting=true;int state=UNIT_RUN;FAH::Client::RunningCPUAllocation desired;bool isPaused()const{return paused;}bool isRunning()const{return running;}bool isWaiting()const{return waiting;}int getState()const{return state;}auto buildDesiredAllocation()const{return desired;}App app;uint64_t rejectedTopologyGeneration=0;unsigned affinityRejections=0;bool affinityRejectionLogged=false;bool persistent=false,environmentWarning=false;set<unsigned> rejectedAffinityCPUs;set<unsigned> attemptedAffinityCPUs{0,2};FAH::Client::RunningCPUAllocation runningAllocation;template<class T>void insert(const char *key,const T &){if(string(key)=="affinity_warning")persistent=true;if(string(key)=="launch_environment_warning")environmentWarning=true;}void erase(const char *key){if(string(key)=="affinity_warning")persistent=false;if(string(key)=="launch_environment_warning")environmentWarning=false;}bool has(const char *key)const{return string(key)=="launch_environment_warning"&&environmentWarning;}unsigned retries=7, delay=0, armed=0;void setWait(unsigned seconds){delay=seconds;}void triggerNext(unsigned seconds){armed=seconds;}void launch(){try{throw AffinityRejected();
"""+catch+r"""
}
"""+active+failure+r"""
void started(){
"""+reset+r"""
}
};
int main(){
 {
  auto old=make_shared<HTTP::Request>(),fresh=make_shared<HTTP::Request>();
  AssignmentUnit cancelled;cancelled.pr.value=make_shared<Pending>(Pending{old});
  cancelled.abortPendingAssignment();assert(!cancelled.pr.isSet()&&!cancelled.assigning&&cancelled.armed==1);
  cancelled.response(*old);assert(cancelled.accepted==0); // Late cancellation callback.
  cancelled.pr.value=make_shared<Pending>(Pending{fresh});const auto releases=cancelled.pr.releases;
  cancelled.response(*old);assert(cancelled.pr.isSet()&&cancelled.pr.releases==releases&&cancelled.accepted==0);
  cancelled.response(*fresh);assert(!cancelled.pr.isSet()&&cancelled.accepted==1);
  AssignmentUnit assigned;assigned.assigning=false;assigned.pr.value=make_shared<Pending>(Pending{old});
  assigned.abortPendingAssignment();assert(assigned.pr.isSet()&&!assigned.cleanups&&!assigned.armed);
  AssignmentUnit failed;failed.pr.value=make_shared<Pending>(Pending{old});failed.failCleanup=true;
  try{failed.abortPendingAssignment();assert(false);}catch(const runtime_error&){}
  assert(failed.armed==1&&!failed.pr.isSet());failed.response(*old);assert(!failed.accepted);
 }

 // Exercise real stop methods: interrupt failure and a later kill failure
 // must both leave the next one-second callback armed.
 for(bool killFailure:{false,true}) {
  Time::clock=100;StopUnit stop;stop.child.failKill=killFailure;
  bool interrupted=false;try {stop.stopRun();}catch(const runtime_error&){interrupted=true;}
  assert(interrupted&&stop.armed==1&&stop.child.interruptTime==100);
  for(unsigned tick=1;tick<=62;++tick) {
   assert(stop.armed==1);stop.armed=0;Time::clock=100+tick;
   try {stop.stopRun();}catch(const runtime_error&){}
   assert(stop.armed==1);
  }
  assert(stop.child.killedByClient&&stop.child.interrupts==1);
  assert(stop.child.kills==(killFailure?2u:1u));
 }
 for(bool refreshFailure:{false,true}){Unit failure;failure.app.resources.fail=refreshFailure;failure.app.fail=!refreshFailure;failure.launch();assert(failure.armed==5&&failure.delay==5&&failure.retries==7&&failure.app.updates==1);}
 warnings=0;Unit activity;
 assert(activity.isActive());activity.desired.mode=FAH::Client::CPUAllocationMode::ManagedCPU;assert(!activity.isActive());
 activity.desired.mask={0};assert(activity.isActive());activity.affinityRejections=1;
 assert(!activity.isActive()&&activity.hasLaunchFailure());activity.running=true;assert(activity.isActive()&&!activity.hasLaunchFailure());
 activity.paused=true;assert(!activity.isActive());activity.paused=false;activity.running=false;
 activity.state=2;assert(activity.isActive());activity.state=5;assert(activity.isActive());
 activity.state=UNIT_RUN;activity.waiting=false;assert(activity.isActive());
 Unit u;assert(u.runningAllocation.mask.empty());u.launch();assert(u.app.resources.probes==1&&u.app.updates==1&&u.delay==5&&u.retries==7);
 u.launch();assert(u.delay==5&&!u.persistent&&warnings==1);u.launch();assert(u.delay==30&&u.persistent&&warnings==2);
 u.launch();assert(u.delay==60);for(unsigned i=0;i<10;++i)u.launch();assert(u.delay==300&&u.retries==7&&u.persistent&&warnings==3);
 u.app.resources.generation++;u.launch();assert(u.delay==5&&!u.persistent&&warnings==4);
 u.attemptedAffinityCPUs={0,4};u.launch();assert(u.delay==5&&u.affinityRejections==1);assert(u.runningAllocation.mask.empty());
 u.launch();u.launch();assert(u.persistent);u.started();assert(!u.persistent&&u.affinityRejections==0&&!u.affinityRejectionLogged&&u.rejectedAffinityCPUs.empty());
}
"""
compile_and_run(cpp, cxx='g++')
print('PASS: cancelled/stale callbacks preserve active requests; assignment cleanup, stop/kill failures and affinity recovery retain polling')
