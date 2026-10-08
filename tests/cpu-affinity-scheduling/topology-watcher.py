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
s=(root/'src/fah/client/App.cpp').read_text()
a=s.index('  cpuRefreshEvent = base.newEvent([this, topologyRefreshInterval] {')
b=s.index('\n  }, 0);',a)
body=s[a:b].split('{',1)[1]
a=s.index('void App::reconcileSavedConfiguration()')
reconcile=s[a:s.index('\nbool App::isActive',a)]
harness=r"""
#include <functional>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <iostream>
#include <cassert>
#define LOG_DEBUG(a,b) do {} while(0)
#define TRY_CATCH_ERROR(x) try {x;} catch (...) {}
#define CBANG_CATCH_ALL(level,msg) catch (...) {}
struct CPU {bool fail=true,changed=true;bool refreshTopology(const char*) {if(fail){fail=false;throw std::runtime_error("probe");}bool result=changed;changed=false;return result;} unsigned getTopologyGeneration(){return 0;}};
struct Timer{std::function<void()> callback;unsigned delay=0;bool pending=false;bool isPending()const{return pending;}void del(){pending=false;}void fire(){assert(pending);pending=false;callback();}void add(unsigned seconds){delay=seconds;pending=true;}};
struct Base{std::unique_ptr<Timer> newEvent(std::function<void()> cb,int){auto t=std::make_unique<Timer>();t->callback=cb;return t;}};
struct Groups{unsigned failures=2,calls=0,updates=0;void triggerUpdate(){++calls;if(failures){--failures;throw std::runtime_error("reconcile");}++updates;}};
struct App{Base base;bool groupReconciliationDeferred=false;std::unique_ptr<CPU> cpuResources{new CPU};std::unique_ptr<Timer> cpuRefreshEvent{new Timer};std::unique_ptr<Timer> cpuReconciliationEvent;unsigned cpuReconciliationRetries=0;Groups groups;bool topologyReconciliationPending=false;Groups* getGroups(){return &groups;}void triggerUpdate(){groups.triggerUpdate();}void reconcileSavedConfiguration() noexcept;
void fire(){const unsigned topologyRefreshInterval=300;cpuRefreshEvent->pending=false;
 auto callback=[this,topologyRefreshInterval]{BODY};
 try{callback();}catch(...){/* Event::call logs and catches */}
}};
RECONCILE
int main(){App a;
 a.fire();assert(a.cpuRefreshEvent->pending && a.groups.calls==0);
 a.fire();assert(a.cpuRefreshEvent->pending && a.topologyReconciliationPending && a.groups.calls==1);
 assert(a.cpuReconciliationEvent->pending&&a.cpuReconciliationEvent->delay==5);
 a.cpuReconciliationEvent->fire();assert(a.topologyReconciliationPending&&a.cpuReconciliationEvent->delay==15);
 a.cpuReconciliationEvent->fire();assert(!a.topologyReconciliationPending&&a.groups.updates==1&&!a.cpuReconciliationEvent->pending);
 a.fire();assert(a.cpuRefreshEvent->pending && !a.topologyReconciliationPending && a.groups.updates==1);
 a.fire();assert(a.cpuRefreshEvent->pending && a.groups.calls==3 && a.groups.updates==1);
 App bounded;bounded.cpuResources->fail=false;bounded.groups.failures=10;
 bounded.fire();assert(bounded.cpuReconciliationEvent->delay==5);
 bounded.cpuReconciliationEvent->fire();assert(bounded.cpuReconciliationEvent->delay==15);
 bounded.cpuReconciliationEvent->fire();assert(bounded.cpuReconciliationEvent->delay==30);
 bounded.cpuReconciliationEvent->fire();assert(!bounded.cpuReconciliationEvent->pending&&bounded.topologyReconciliationPending);
 auto calls=bounded.groups.calls;bounded.fire();assert(bounded.groups.calls==calls+1&&bounded.topologyReconciliationPending&&!bounded.cpuReconciliationEvent->pending);
 bounded.groups.failures=0;bounded.fire();assert(!bounded.topologyReconciliationPending&&bounded.cpuReconciliationRetries==0);
 App unchanged;unchanged.cpuResources->fail=false;unchanged.cpuResources->changed=false;
 unchanged.fire();assert(unchanged.cpuRefreshEvent->pending && unchanged.groups.calls==0);
 App saved;saved.cpuResources->fail=false;saved.cpuResources->changed=false;
 saved.topologyReconciliationPending=true;
 saved.fire();assert(saved.topologyReconciliationPending&&saved.groups.calls==1);
 saved.fire();assert(saved.topologyReconciliationPending&&saved.groups.calls==2);
 saved.fire();assert(!saved.topologyReconciliationPending&&saved.groups.updates==1);
 saved.fire();assert(saved.groups.calls==3);
 std::cout<<"PASS: topology watcher retries failed reconciliation on stable probes, recovers, then stops retrying\n";}
""".replace('BODY',body).replace('RECONCILE',reconcile)
compile_and_run(harness, includes=())
