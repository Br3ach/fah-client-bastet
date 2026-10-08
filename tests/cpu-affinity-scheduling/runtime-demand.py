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
g=(root/'src/fah/client/Group.cpp').read_text()
start=g.index('bool Group::wantsResources() const {')
method=g[start:g.index('\n\n\nbool Group::waitForRetry()',start)]
start=g.index('  // Local timers also drive')
transition=g[start:g.index('  // Remove completed units',start)]
groups=(root/'src/fah/client/Groups.cpp').read_text()
start=groups.index('void Groups::delGroup(')
delete=groups[start:groups.index('\n\n\nJSON::ValuePtr Groups::proposeConfiguration',start)]
u=(root/'src/fah/client/Unit.cpp').read_text()
start=u.index('const char *Unit::getPauseReason() const {')
pause=u[start:u.index('\n\n\nbool Unit::isRunning()',start)]
start=u.index('void Unit::stopRun() {')
stop=u[start:u.index('\n}',start)+2]
stop_condition=u[u.index('      if (isPaused() || getState() != UNIT_RUN'):]
stop_condition=stop_condition[:stop_condition.index(' {')].strip().replace('if (','',1)[:-1]
harness=r"""
#include <fah/client/CPUOwnershipPolicy.h>
#include <fah/client/CPUAllocationPlanner.h>
#include <functional>
#include <cassert>
#include <memory>
#include <vector>
#include <map>
#include <string>
#include <stdexcept>
#include <iostream>
using namespace std;using FAH::Client::RunningCPUAllocation;using FAH::Client::CPUOwnershipPolicy;
#define LOG_INFO(a,b) do{}while(0)
#define THROW(a) throw runtime_error(a)
const int UNIT_RUN=4;
struct Time{inline static unsigned clock=100;static unsigned now(){return clock;}};
struct Group;
struct Unit;
struct Config{bool paused=false,finish=false;bool getPaused()const{return paused;}bool getFinish()const{return finish;}};
struct App{bool quit=false,fail=false;unsigned updates=0;function<void()> reconcile;bool shouldQuit()const{return quit;}void triggerUpdate(){++updates;if(fail)throw runtime_error("reconcile");if(reconcile)reconcile();}};
struct Unit{App app;int state=UNIT_RUN;Group *group=nullptr;RunningCPUAllocation runningAllocation;bool process=true;
 int getState()const{return state;}void setGroup(Group*g){group=g;}const Config &getConfig()const;
 struct Child {unsigned stops=0;void stop(){++stops;}} child;
 Child *processHandle=&child;unsigned timer=0;
 void triggerNext(unsigned delay=0){timer=delay;if(delay)return;
  bool cpuCountChanged=false,affinityChanged=false;
  if(STOP_CONDITION) stopRun();}
 bool isPaused()const{return getPauseReason()!=nullptr;}
 void stopRun();
 bool getBoolean(const char*,bool)const{return false;}const char *getPauseReason()const;};
struct Event{unsigned polls=0;void add(double){++polls;}};
struct Group{App app;shared_ptr<Config>config=make_shared<Config>();shared_ptr<Event>event=make_shared<Event>();
 bool idle=false,battery=false,gpu=false,lastResourceDemand=true;unsigned waitUntil=0;vector<shared_ptr<Unit>> wus;
 bool waitForRetry()const{return Time::now()<waitUntil;}bool waitForIdle()const{return idle;}bool waitOnBattery()const{return battery;}bool waitOnGPU()const{return gpu;}
 auto units()const{return wus;}void remove(){}bool wantsResources()const;
 void poll(){TRANSITION}
};
METHOD
const Config &Unit::getConfig()const{return *group->config;}
PAUSE
STOP
struct Groups{map<string,shared_ptr<Group>> groups;bool has(const string&n){return groups.count(n);}Group&getGroup(const string&n){return *groups.at(n);}void erase(const string&n){groups.erase(n);}void delGroup(const string&name);};
DELETE
int main(){
 Group g;assert(g.wantsResources());
 for(unsigned reason=0;reason<5;++reason){
  g.config->paused=reason==0;g.idle=reason==1;g.battery=reason==2;g.gpu=reason==3;g.waitUntil=reason==4?101:0;
  assert(!g.wantsResources());g.lastResourceDemand=true;unsigned before=g.app.updates;g.poll();assert(g.app.updates==before+1&&!g.lastResourceDemand);
  g.poll();assert(g.app.updates==before+1);
  g.config->paused=g.idle=g.battery=g.gpu=false;g.waitUntil=100;
  assert(g.wantsResources());g.poll();assert(g.app.updates==before+2&&g.lastResourceDemand);
 }
 g.config->finish=true;assert(!g.wantsResources());auto u=make_shared<Unit>();u->group=&g;g.wus={u};assert(g.wantsResources());
 u->state=5;assert(!g.wantsResources());g.config->finish=false;g.config->paused=true;g.app.fail=true;
 bool failed=false;try{g.poll();}catch(...){failed=true;}assert(failed&&g.lastResourceDemand&&g.event->polls);g.app.fail=false;g.poll();assert(!g.lastResourceDemand);
 g.config->paused=false;g.waitUntil=101;Unit stopped;stopped.group=&g;
 assert(string(stopped.getPauseReason())=="Waiting after failed work units");
 stopped.state=5;assert(!stopped.getPauseReason()); // Upload may proceed during group backoff.
 // Two live WUs enter backoff as another RG receives their desired cores.
 // Use the actual planner and ownership policy; child exit is controlled here.
 {
  using Planner=FAH::Client::CPUAllocationPlanner;
  Group a,b;auto first=make_shared<Unit>(),second=make_shared<Unit>();
  first->group=second->group=&a;a.wus={first,second};
  Planner::Topology topology;topology.hardAffinity=topology.homogeneous=true;
  topology.available={0,1,2,3};topology.coreThreads={{0,1},{2,3}};
  topology.performanceLevels={topology.available};
  Planner::Request ra,rb;ra.name="A";ra.workers=4;rb.name="B";rb.workers=4;
  b.config->paused=true;Planner::Result allocation;
  auto replan=[&]{ra.wantsResources=a.wantsResources();rb.wantsResources=b.wantsResources();
   allocation=Planner::plan(topology,{ra,rb});};
  a.app.reconcile=b.app.reconcile=replan;replan();
  assert(allocation.workerBudgets.at("A")==4);
  first->runningAllocation.mode=second->runningAllocation.mode=FAH::Client::CPUAllocationMode::ManagedCPU;
  first->runningAllocation.mask={0};first->runningAllocation.resourcePool={0,1};
  second->runningAllocation.mask={2};second->runningAllocation.resourcePool={2,3};
  const auto frozenFirst=first->runningAllocation,frozenSecond=second->runningAllocation;
  a.waitUntil=Time::clock+1024;b.config->paused=false;
  a.poll();assert(!a.lastResourceDemand&&first->child.stops==1&&second->child.stops==1);
  assert(first->timer==1&&second->timer==1);
  assert(allocation.workerBudgets.at("B")==4&&allocation.workerBudgets.at("A")==0);
  RunningCPUAllocation desired;desired.mode=FAH::Client::CPUAllocationMode::ManagedCPU;desired.mask={0,2};
  desired.resourcePool={0,1,2,3};
  auto blocked=[&]{for(auto unit:a.wus)if(unit->process&&
    CPUOwnershipPolicy::blocksLaunch(desired,false,unit->runningAllocation,false))return true;
    return false;};
  assert(blocked());first->process=false;assert(blocked());
  second->process=false;assert(!blocked()&&Time::now()<a.waitUntil);
  assert(first->runningAllocation.resourcePool==frozenFirst.resourcePool&&
    second->runningAllocation.resourcePool==frozenSecond.resourcePool);
  // Expiry replans from the current reduced topology, preserving saved demand.
  topology.available={0,1};topology.coreThreads={{0,1}};
  topology.performanceLevels={topology.available};
  b.config->paused=true;Time::clock=a.waitUntil;a.poll();
  assert(a.lastResourceDemand&&allocation.workerBudgets.at("A")==2&&ra.workers==4);
  Time::clock=100;
  cout<<"PASS: two live WUs stop on backoff, new RG waits for both exits, recovery uses current topology and saved demand\n";
 }
 // Delete an RG while its old process is still alive: migration must not rewrite ownership.
 Groups all;all.groups[""]=make_shared<Group>();all.groups["old"]=make_shared<Group>();
 auto old=make_shared<Unit>();old->group=all.groups["old"].get();old->runningAllocation.mode=FAH::Client::CPUAllocationMode::ManagedCPU;old->runningAllocation.mask={0};old->runningAllocation.resourcePool={0,1};
 all.groups["old"]->wus={old};all.delGroup("old");assert(old->group==all.groups[""].get()&&!all.has("old"));
 RunningCPUAllocation desired;desired.mode=FAH::Client::CPUAllocationMode::ManagedCPU;desired.mask={1};desired.resourcePool={0,1};
 assert(old->process&&CPUOwnershipPolicy::blocksLaunch(desired,false,old->runningAllocation,false));old->process=false;
 assert(!(old->process&&CPUOwnershipPolicy::blocksLaunch(desired,false,old->runningAllocation,false)));
 cout<<"PASS: runtime pause/wait/finish demand, global transition recovery and deleted-RG frozen ownership\n";
}
""".replace('PAUSE',pause).replace('METHOD',method).replace('TRANSITION',transition).replace('DELETE',delete).replace('STOP_CONDITION',stop_condition).replace('STOP',stop.replace('process->stop()', 'processHandle->stop()'))
compile_and_run(harness, ['CPUOwnershipPolicy.cpp', 'CPUAllocationPlanner.cpp', 'CPUExecutionPlan.cpp', 'CPUWholeCorePacking.cpp'])
