#!/usr/bin/env python3
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

"""Exercise the production App::configure body with configuration adapters."""
from pathlib import Path
import os
from compile_harness import compile_and_run
root = Path(__file__).resolve().parents[2]
source = (root / 'src/fah/client/App.cpp').read_text()
start = source.index('void App::configure(')
body = source[start:source.index('\n\n\nstring App::getURL', start)]
reconcile_start=source.index("void App::reconcileSavedConfiguration()")
reconcile=source[reconcile_start:source.index("\nbool App::isActive",reconcile_start)]
group_source = (root / "src/fah/client/Group.cpp").read_text()
notify_start = group_source.index("void Group::notify(")
notify = group_source[notify_start:group_source.index("void Group::update()", notify_start)]
harness = r"""
#include <cassert>
#include <functional>
#include <iterator>
#include <list>
#include <memory>
#include <string>
#include <stdexcept>
using namespace std;
#define LOG_DEBUG(level,x) do{}while(false)
#define LOG_WARNING(x) do{}while(false)
#define THROW(x) throw runtime_error(x)
struct CbangError:runtime_error {using runtime_error::runtime_error;};
#ifdef DEBUG
#define TRY_CATCH_ERROR(x) try {x;} catch (const CbangError &) {}
#else
#define TRY_CATCH_ERROR(x) try {x;} catch (...) {}
#endif
bool failLogging=false;
#define CBANG_CATCH_ALL(level,msg) catch (...) {if(failLogging)throw runtime_error("logging");}
namespace JSON { struct Value {
 string getString()const{return "config";}
 bool config=false, groups=false, change=false;
 bool smtPresent=false,smtBoolean=true;
 bool has(const string &) const {return smtPresent;}
 bool isBoolean() const {return smtBoolean;}
 bool hasDict(const string &key) const {return key=="config"?config:groups;}
 shared_ptr<Value> get(const string &) const {return make_shared<Value>(*this);}
}; using ValuePtr=shared_ptr<Value>; }
struct Config {string state="old"; string toString(){return state;}
 void configure(const JSON::Value &v){if(v.change)state="new";}};
struct Groups {int updates=0;bool fail=false,configuring=false;
 bool isConfiguring()const{return configuring;}
 void configure(const JSON::Value &){if(fail)throw runtime_error("reject");updates++;}};
struct Timer {
 function<void()> callback;bool pending=false,failAdd=false;unsigned delay=0;
 bool isPending()const{return pending;}void add(unsigned n){if(failAdd)throw runtime_error("timer add");pending=true;delay=n;}
 void del(){pending=false;}void fire(){assert(pending);pending=false;callback();}
};
struct Base {bool fail=false;shared_ptr<Timer> newEvent(function<void()> f,int){if(fail)throw runtime_error("timer creation");auto t=make_shared<Timer>();t->callback=f;return t;}};
struct App {Base base;shared_ptr<Timer> cpuReconciliationEvent;unsigned cpuReconciliationRetries=0;Config config; Groups groups; int updates=0; bool allowed=true;
 bool groupReconciliationDeferred=false,topologyReconciliationPending=false,failReconcile=false; int batches=0, publications=0;
 void beginGroupConfigNotifications(){batches++;}
 void endGroupConfigNotifications(bool){assert(batches>0); if(!--batches)publications++;}
 bool validateChange(const JSON::Value &){return allowed;}
 Config *getConfig(){return &config;} Groups *getGroups(){return &groups;}
 void triggerUpdate(){assert(!groupReconciliationDeferred);updates++;if(failReconcile)throw runtime_error("reconcile");} void configure(const JSON::Value &);
 void reconcileSavedConfiguration() noexcept;
};
""" + body + "\n" + reconcile + r"""
struct Group {
 App &app;Config *config;int saves=0;bool failSave=false;
 explicit Group(App &app):app(app),config(&app.config){}
 void save(){if(failSave)throw runtime_error("save");++saves;}
 void notify(const list<JSON::ValuePtr> &);
};
""" + notify + r"""
int main(){
 list<JSON::ValuePtr> change(3,make_shared<JSON::Value>());
 App notification;Group group(notification);notification.failReconcile=true;
 group.notify(change);
 assert(group.saves==1&&notification.topologyReconciliationPending);
 assert(notification.cpuReconciliationEvent->delay==5);
 notification.failReconcile=false;notification.cpuReconciliationEvent->fire();
 assert(!notification.topologyReconciliationPending&&notification.updates==2);
 notification.groups.configuring=true;group.notify(change);
 assert(group.saves==1&&notification.updates==2);
 notification.groups.configuring=false;group.failSave=true;
 try{group.notify(change);assert(false);}catch(const runtime_error &e){assert(string(e.what())=="save");}
 assert(notification.updates==2);

 for(bool hasConfig:{false,true}) for(bool hasGroups:{false,true})
 for(bool globalChange:{false,true}) {
  App app; JSON::Value msg{hasConfig,hasGroups,globalChange}; app.configure(msg);
  bool groupSave=hasConfig&&hasGroups;
  assert(app.groups.updates==int(groupSave));
  assert(app.updates==1 && app.publications==1 && !app.groupReconciliationDeferred);
 }
 App rejected; rejected.groups.fail=true; JSON::Value both{true,true,true};
 try{rejected.configure(both);assert(false);}catch(const runtime_error &){}
 assert(rejected.config.state=="old"&&rejected.updates==1);
 // Rejected requests use the same retry path without replacing the rejection.
 App rejectionRecovery;rejectionRecovery.groups.fail=true;rejectionRecovery.failReconcile=true;
 try{rejectionRecovery.configure(both);assert(false);}catch(const runtime_error &e){assert(string(e.what())=="reject");}
 assert(rejectionRecovery.config.state=="old"&&rejectionRecovery.publications==1&&rejectionRecovery.batches==0);
 assert(!rejectionRecovery.groupReconciliationDeferred&&rejectionRecovery.topologyReconciliationPending);
 assert(rejectionRecovery.cpuReconciliationEvent->delay==5);
 rejectionRecovery.failReconcile=false;rejectionRecovery.cpuReconciliationEvent->fire();
 assert(!rejectionRecovery.topologyReconciliationPending&&!rejectionRecovery.cpuReconciliationEvent->pending);
 // Scheduling and diagnostic failures cannot escape noexcept, including DEBUG.
 App noTimer;noTimer.failReconcile=true;noTimer.base.fail=true;noTimer.configure(both);
 assert(noTimer.topologyReconciliationPending&&!noTimer.cpuReconciliationEvent&&noTimer.publications==1);
 noTimer.base.fail=false;noTimer.reconcileSavedConfiguration();assert(noTimer.cpuReconciliationEvent->delay==5);
 App noAdd;noAdd.failReconcile=true;noAdd.cpuReconciliationEvent=make_shared<Timer>();noAdd.cpuReconciliationEvent->failAdd=true;
 noAdd.configure(both);assert(noAdd.topologyReconciliationPending&&!noAdd.cpuReconciliationEvent->pending);
 App noLog;noLog.failReconcile=true;noLog.base.fail=true;failLogging=true;noLog.configure(both);failLogging=false;
 assert(noLog.topologyReconciliationPending&&noLog.publications==1);
 // Removed SMT option is no longer a special pre-validation path.
 App valid;JSON::Value good{true,false,true};valid.configure(good);
 assert(valid.updates==1&&valid.config.state=="new");
 App committed;committed.failReconcile=true;committed.configure(both);
 assert(committed.config.state=="new"&&committed.topologyReconciliationPending);
 assert(committed.cpuReconciliationEvent->delay==5);
 committed.reconcileSavedConfiguration(); // Coalesce without postponing or consuming retries.
 assert(committed.cpuReconciliationRetries==0&&committed.cpuReconciliationEvent->delay==5);
 committed.cpuReconciliationEvent->fire();assert(committed.cpuReconciliationEvent->delay==15);
 committed.cpuReconciliationEvent->fire();assert(committed.cpuReconciliationEvent->delay==30);
 committed.cpuReconciliationEvent->fire();
 assert(committed.topologyReconciliationPending&&!committed.cpuReconciliationEvent->pending);
 committed.failReconcile=false;committed.reconcileSavedConfiguration();
 assert(!committed.topologyReconciliationPending&&committed.cpuReconciliationRetries==0);
 App transient;transient.failReconcile=true;transient.configure(both);
 transient.failReconcile=false;transient.cpuReconciliationEvent->fire();
 assert(!transient.topologyReconciliationPending&&!transient.cpuReconciliationEvent->pending);
 App cancelled;cancelled.failReconcile=true;cancelled.configure(both);
 cancelled.failReconcile=false;cancelled.reconcileSavedConfiguration();
 assert(!cancelled.cpuReconciliationEvent->pending);
 App denied;denied.allowed=false;denied.configure(both);
 assert(denied.groups.updates==0&&denied.updates==0);
}
"""
for debug in (False, True):
    compile_and_run(harness, includes=(), defines=('DEBUG',) if debug else (),
                    cxx=os.environ.get('CXX', 'g++'))
print('PASS: accepted/rejected configuration recovery, bounded retries and noexcept scheduling/logging failures in release and debug')
