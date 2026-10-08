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

"""Compile the shipped per-WU metadata method with small JSON/lifecycle adapters."""
from pathlib import Path
from compile_harness import compile_and_run
root=Path(__file__).resolve().parents[2]
s=(root/'src/fah/client/Unit.cpp').read_text()
a=s.index('std::optional<UnitCPUStatus> Unit::getDesiredCPUStatus() const {')
b=s.index('\n\n\nuint32_t Unit::getMinCPUs()',a)
method=s[a:b]
a=s.index('std::optional<CPUExecutionPlan::SliceExecution> Unit::buildCPUExecutionPlan() const {')
b=s.index('\n\n\nRunningCPUAllocation Unit::buildDesiredAllocation() const {',a)
method += '\n' + s[a:b]
harness=r"""
#include <fah/client/CPUExecutionPlan.h>
#include <fah/client/CPUExecutionPlan.h>
#include <fah/client/CPUStatus.h>
#include <optional>
using FAH::Client::UnitCPUStatus;
using FAH::Client::CPUClassAllocationStatus;
#include <memory>
#include <map>
#include <string>
#include <vector>
#include <cassert>
#include <algorithm>
#include <iostream>
using namespace std;using FAH::Client::CPUAllocationSlices;
// Forward to the real planner while retaining its selected mask for comparison.
struct CPUExecutionPlan : FAH::Client::CPUExecutionPlan {
 using FAH::Client::CPUExecutionPlan::operator=;
 inline static CPUSet lastMask;
 static auto createSlices(unsigned type,const FAH::Client::CPUAllocationSlices &slices,
     const vector<CPUSet> &cores,const vector<unsigned> &order={}) {
  auto result=FAH::Client::CPUExecutionPlan::createSlices(type,slices,cores,order);
  lastMask=result?result->mask:CPUSet{};return result;
 }
 static FAH::Client::CPUExecutionPlan create(unsigned type,unsigned workers,
     const CPUSet &pool,const vector<CPUSet> &cores,const vector<unsigned> &order={}) {
  auto plan=FAH::Client::CPUExecutionPlan::create(type,workers,pool,cores,order);
  lastMask=plan.mask;return plan;
 }
};
struct Config{unsigned getConfiguredCPUTotal()const{return 14;}string getCPUMode()const{return "count";}vector<unsigned>getCPUClassCounts()const{return {};}};
struct Group{string getName()const{return "A";}};
struct Core{unsigned type=0xa8;unsigned getType()const{return type;}};
struct CorePtr{Core value;bool isSet()const{return true;}const Core*operator->()const{return &value;}};
struct Resources{vector<set<unsigned>>cores={{0,1},{2,3},{4,5}};unsigned generation=2;
 vector<unsigned> order={4,2,0,5,3,1};
 const auto&getGroupCPUs(const string &name)const{assert(name=="A");return order;}
 const auto&getCoreThreads()const{return cores;}unsigned getAllocationGeneration()const{return generation;}};
struct App{Resources resources;const Resources&getCPUResources()const{return resources;}};
struct Unit{bool affinityClassMode=false;CPUAllocationSlices affinitySlices;bool affinityManaged=true,gpu=false,paused=false;unsigned affinityAllocationGeneration=2;
 set<unsigned>affinityCPUs={0,1,2,3,4,5};CorePtr core;App app;shared_ptr<Group>group=make_shared<Group>();Config config;unsigned workers=6;
 bool hasGPUs()const{return gpu;}bool atRunState()const{return true;}bool isPaused()const{return paused;}
 unsigned getScheduledCPUs()const{return workers;}uint64_t getU64(const string&)const{return 42;}
 const Config&getConfig()const{return config;}
 std::optional<CPUExecutionPlan::SliceExecution> buildCPUExecutionPlan()const;
 std::optional<UnitCPUStatus> getDesiredCPUStatus()const;
};
METHOD
int main(){Unit u;auto info=u.getDesiredCPUStatus();assert(info&&info->fullSMT&&info->physical==3&&info->workers==6);
 u.workers=2;info=u.getDesiredCPUStatus();assert(info->physical==2&&info->logical==2&&!info->fullSMT);
 assert(CPUExecutionPlan::lastMask==set<unsigned>({2,4}));
 // Missing topology must keep the same preferred order as the launch plan.
 auto saved=u.app.resources.cores;u.app.resources.cores.clear();u.workers=1;
 info=u.getDesiredCPUStatus();assert(CPUExecutionPlan::lastMask==set<unsigned>({4}));
 assert(info->logical==1 && !info->fullSMT);
 u.app.resources.cores=saved;
 u.workers=6;u.core.value.type=0xa7;assert(!u.getDesiredCPUStatus()->fullSMT);
 u.affinityAllocationGeneration=1;assert(!u.getDesiredCPUStatus());u.affinityAllocationGeneration=2;
 u.paused=true;assert(!u.getDesiredCPUStatus());u.paused=false;u.gpu=true;assert(!u.getDesiredCPUStatus());
 u.gpu=false;u.affinityClassMode=true;u.core.value.type=0x27;
 u.workers=3;u.affinityCPUs={0,1,2,3};
 u.affinitySlices={{1,{0,1}},{2,{2,3}}};
 info=u.getDesiredCPUStatus();assert(info&&info->workers==3&&info->physical==2&&info->logical==3&&!info->fullSMT);
 assert(info->allocationSlices.size()==2);
 assert(info->allocationSlices[0].workers==1 && info->allocationSlices[0].logical==1);
 assert(info->allocationSlices[1].workers==2 && info->allocationSlices[1].logical==2);
 assert(!info->allocationSlices[0].fullSMT && !info->allocationSlices[1].fullSMT);
 u.core.value.type=0xa8;info=u.getDesiredCPUStatus();
 assert(info->fullSMT && !info->allocationSlices[0].fullSMT && info->allocationSlices[1].fullSMT);
 u.affinitySlices.clear();assert(!u.getDesiredCPUStatus());
 cout<<"PASS: actual WU core policy, mask physical counts, stale-generation exclusion, paused/GPU exclusion\n";}
""".replace('METHOD',method)
compile_and_run(harness, ['CPUWholeCorePacking.cpp', 'CPUExecutionPlan.cpp'])
