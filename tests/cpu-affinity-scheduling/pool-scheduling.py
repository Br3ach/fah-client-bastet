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

"""Compile the production CPU-WU budgeting and whole-core partition block."""
from pathlib import Path
from compile_harness import compile_and_run
root=Path(__file__).resolve().parents[2]
s=(root/'src/fah/client/Group.cpp').read_text()
a=s.index('  // Allocate remaining CPUs to existing CPU WUs')
b=s.index('  // Start and stop WUs',a)
body=s[a:b]
harness=r"""
#include <fah/client/CPUExecutionPlan.h>
#include <fah/client/WUCPUAllocationPlanner.h>
#include <set>
#include <map>
#include <memory>
#include <vector>
#include <string>
#include <cassert>
#include <stdexcept>
#include <iostream>
using FAH::Client::CPUWholeCorePacking;
using namespace std;using FAH::Client::CPUExecutionPlan;using FAH::Client::WUCPUAllocationPlanner;using FAH::Client::CPUAllocationSlices;
#define LOG_DEBUG(a,b) do{}while(false)
const int UNIT_CORE=3, UNIT_RUN=4;
struct Unit {bool isAssigning()const{return false;}string id;bool gpu=false,managed=false;CPUAllocationSlices classes;unsigned minimum=1,maximum=64,cpus=0;set<unsigned>pool;
 bool hasGPUs()const{return gpu;}int getState()const{return UNIT_RUN;}
 unsigned getMinCPUs()const{return minimum;}unsigned getMaxCPUs()const{return maximum;}
 unsigned getScheduledCPUs()const{return cpus;}string getID()const{return id;}void setScheduledCPUs(unsigned n){cpus=n;}
 void setCPUAffinity(bool m,const set<unsigned>&p,const CPUAllocationSlices *s=nullptr,bool classMode=false){managed=m;pool=p;classes=s?*s:CPUAllocationSlices{};}};
struct CPU {CPUAllocationSlices slices;const CPUAllocationSlices &getGroupAllocationSlices(const string&)const{return slices;}vector<set<unsigned>>cores;const auto &getCoreThreads()const{return cores;}};
struct Config {bool classes=false;bool usesCPUClasses()const{return classes;}};
struct Group {string name="A";Config cfg;Config*config=&cfg;CPU cpuResources;vector<shared_ptr<Unit>>wus;auto units(){return wus;}
 void schedule(vector<unsigned> groupCPUs,unsigned remainingCPUs,bool managed=true){
 set<string>enabledWUs;
"""+body+r"""
 }
};
int main(){
 Group g;g.cpuResources.cores={{0,1},{2,3},{4,5},{6,7},{8,9},{10,11},{12,13},{14,15}};
 auto a=make_shared<Unit>();a->id="A";g.wus={a};
 vector<unsigned> order={0,2,4,6,8,10,12,14,1,3,5,7,9,11,13,15};
 for(unsigned n=1;n<=16;++n){g.schedule(order,n);assert(a->cpus==n);assert(a->pool.size()==2*min(n,8u));
  auto plan=CPUExecutionPlan::create(0xa8,n,a->pool,g.cpuResources.cores,order);
  assert(plan.mask.size()==(n<=8?n:16));}
 auto b=make_shared<Unit>();b->id="B";a->maximum=8;b->maximum=8;g.wus={a,b};
 g.schedule(order,16);assert(a->cpus==8&&b->cpus==8);
 for(auto cpu:a->pool)assert(!b->pool.count(cpu));
 for(auto core:g.cpuResources.cores){unsigned owners=0;for(auto u:g.wus)for(auto cpu:core)if(u->pool.count(cpu)){++owners;break;}assert(owners==1);}
 // Group totals do not reveal every WU's full-SMT condition.
 a->maximum=8;b->maximum=6;g.schedule(order,14);
 auto rg=CPUExecutionPlan::create(0xa8,14,set<unsigned>(order.begin(),order.end()),g.cpuResources.cores,order);
 auto wu=CPUExecutionPlan::create(0xa8,b->cpus,b->pool,g.cpuResources.cores,order);
 assert(!rg.fullSMT && wu.fullSMT && b->cpus==6 && wu.physical==3);
 a->maximum=8;b->maximum=8;
 // Recover the fourth worker in a 3+1 request split over two SMT2 cores.
 a->maximum=3;b->maximum=3;g.schedule({0,2,1,3},4);
 assert(a->cpus==2&&b->cpus==2&&a->pool.size()==2&&b->pool.size()==2);
 // Recovery must respect each WU maximum and cannot exceed the RG budget.
 b->maximum=1;g.schedule({0,2,1,3},4);assert(a->cpus==2&&b->cpus==1);
 b->maximum=3;g.schedule({0,2,1,3},3);assert(a->cpus+(b->pool.empty()?0:b->cpus)<=3);
 // Sweep mixed physical widths and budget/maximum boundaries after recovery.
 for(unsigned first=1;first<=5;++first)for(unsigned second=1;second<=5;++second)
  for(unsigned budget=0;budget<=6;++budget){
   a->maximum=first;b->maximum=second;g.schedule({0,2,4,5,1,3},budget);
   unsigned fulfilled=0;
   for(auto u:g.wus)if(!u->pool.empty()){
    assert(u->cpus>=u->minimum&&u->cpus<=u->maximum&&u->cpus<=u->pool.size());
    fulfilled+=u->cpus;
   }
   assert(fulfilled<=budget);
   for(auto cpu:a->pool)assert(!b->pool.count(cpu));
  }
 // Odd simultaneous WU counts cannot split a physical core to match budgets.
 a->maximum=3;b->maximum=3;g.schedule({0,2,4,1,3,5},6);
 assert(a->cpus==3&&b->cpus==2);assert(a->pool.size()==4&&b->pool.size()==2);
 // A CPU WU whose minimum cannot fit gets an empty pool and cannot launch.
 b->minimum=3;g.schedule({0,2,4,1,3,5},6);assert(b->pool.empty());
 // Reject a 3-worker minimum on one SMT2 core, then give B the released core.
 a->minimum=3;a->maximum=3;b->minimum=1;b->maximum=3;
 g.schedule({0,2,1,3},4);
 assert(a->cpus==0&&a->pool.empty()&&b->cpus==3&&b->pool.size()==4);
 a->minimum=1;
 // The real Group block must preserve class slices, including failure-to-wait.
 g.cfg.classes=true;g.cpuResources.cores={{0,1},{2,3}};
 g.cpuResources.slices={{1,{0,1}},{2,{2,3}}};g.wus={a};
 a->minimum=3;a->maximum=3;g.schedule({0,2,1,3},3);
 assert(a->cpus==3&&a->classes.size()==2);
 assert(a->classes[0].workers==1&&a->classes[1].workers==2);
 g.cpuResources.cores={{0}};g.schedule({0,2,1,3},3);
 assert(a->cpus==0&&a->pool.empty());
 g.cfg.classes=false;g.cpuResources.cores={{0,1},{2,3},{4,5},{6,7},{8,9},{10,11},{12,13},{14,15}};
 a->minimum=1;
 // GPU helpers never consume the CPU process pool.
 auto gpu=make_shared<Unit>();gpu->id="GPU";gpu->gpu=true;g.wus={a,gpu};
 g.schedule(order,8);assert(gpu->pool.empty()&&!gpu->managed);
 cout<<"PASS: production CPU-WU worker budgets, expanded pools, simultaneous isolation, minimum constraints, GPU independence\n";
}
"""
compile_and_run(harness, ['WUCPUAllocationPlanner.cpp', 'CPUExecutionPlan.cpp', 'CPUWholeCorePacking.cpp'], cxx='g++')
