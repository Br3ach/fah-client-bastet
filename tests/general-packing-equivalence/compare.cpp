/******************************************************************************\

                  This file is part of the Folding@home Client.

          The fah-client runs Folding@home protein folding simulations.
                    Copyright (c) 2001-2026, foldingathome.org
                               All rights reserved.

       This program is free software; you can redistribute it and/or modify
       it under the terms of the GNU General Public License as published by
        the Free Software Foundation; either version 3 of the License, or
                       (at your option) any later version.

         This program is distributed in the hope that it will be useful,
          but WITHOUT ANY WARRANTY; without even the implied warranty of
          MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
                   GNU General Public License for more details.

     You should have received a copy of the GNU General Public License along
     with this program; if not, write to the Free Software Foundation, Inc.,
           51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

                  For information regarding this software email:
                                 Joseph Coffland
                          joseph@cauldrondevelopment.com

\******************************************************************************/

#include "ReferenceExecutionPlan.h"
#include "ReferenceWUPlanner.h"
#include "ReferenceRGPlanner.h"
#include <fah/client/CPUAllocationPlanner.h>
#include <fah/client/CPUExecutionPlan.h>
#include <fah/client/CPUWholeCorePacking.h>
#include <fah/client/WUCPUAllocationPlanner.h>
#include <cassert>
#include <iostream>
#include <algorithm>
using namespace FAH::Client;
using Set=CPUExecutionPlan::CPUSet;
template<class A,class B> void report(const A&a,const B&b) {
 assert(int(a.outcome)==int(b.outcome)&&a.cores==b.cores&&a.groups==b.groups&&
 a.visits==b.visits&&a.improved==b.improved);
}
void check(std::vector<Set> cores,std::vector<unsigned> order,
 const std::map<std::string,unsigned>& demand) {
 for(bool fill:{false,true}) {
  ReferenceExecutionPlan::RebalanceReport oldReport;
  CPUWholeCorePacking::RebalanceReport newReport;
  auto a=ReferenceExecutionPlan::partition(demand,order,cores,fill,&oldReport);
  auto b=CPUWholeCorePacking::partition(demand,order,cores,fill,&newReport);
  assert(a==b);report(oldReport,newReport);
  for(const auto&entry:b)for(unsigned type:{0xa8u,0xa9u,0x27u}) {
   auto n=std::min<unsigned>(demand.at(entry.first),entry.second.size());
   auto x=ReferenceExecutionPlan::create(type,n,entry.second,cores,order);
   auto y=CPUExecutionPlan::create(type,n,entry.second,cores,order);
   assert(x.mask==y.mask&&x.physical==y.physical&&x.logical==y.logical&&
     x.hasSMT==y.hasSMT&&x.fullSMT==y.fullSMT);
   auto slices=CPUExecutionPlan::createSlices(type,{{n,entry.second}},cores,order);
   if(!n) assert(!slices);
   else {
    assert(slices);auto summary=slices->summary();
    assert(slices->mask==x.mask && summary.logical==x.logical && summary.physical==x.physical &&
      summary.fullSMT==x.fullSMT && summary.hasSMT==x.hasSMT);
    assert(slices->maskPhysical()==std::min<unsigned>(x.physical,x.mask.size()));
   }
  }
 }
}
unsigned compareRG() {
 unsigned cases=0;
 for(unsigned shape=0;shape<27;++shape) for(unsigned mode=0;mode<4;++mode) {
  CPUAllocationPlanner::Topology topology; ReferenceRGPlanner::Topology oldTopology;
  unsigned code=shape,id=0;
  for(unsigned c=0;c<3;++c) {
   Set core;for(unsigned j=0;j<1+code%3;++j){core.insert(id);topology.available.insert(id++);}code/=3;
   topology.coreThreads.push_back(core);
  }
  topology.hardAffinity=mode!=0; topology.homogeneous=mode!=1;
  topology.performanceLevels={topology.available};
  topology.fastPhysicalCores=topology.coreThreads;
  if(mode==3)topology.coreThreads.pop_back();
  oldTopology.hardAffinity=topology.hardAffinity;oldTopology.homogeneous=topology.homogeneous;
  oldTopology.available=topology.available;oldTopology.coreThreads=topology.coreThreads;
  oldTopology.rawPerformanceLevels=topology.performanceLevels;oldTopology.performanceLevels=topology.performanceLevels;
  oldTopology.fastPhysicalCores=topology.fastPhysicalCores;
  for(unsigned a=0;a<=4;++a)for(unsigned b=0;b<=4;++b)for(unsigned c=0;c<=3;++c) {
   std::vector<CPUAllocationPlanner::Request> requests(3);
   requests[0].name="";requests[0].workers=a;requests[1].name="A";requests[1].workers=b;
   requests[2].name="z";requests[2].workers=c;
   if(c==3){requests[2].gpus={"GPU"};requests[2].reservedCores=1;}
   if(a==4)requests[0].wantsResources=false;
   std::vector<ReferenceRGPlanner::Request> previous;
   for(const auto &request:requests) {
    ReferenceRGPlanner::Request old;old.name=request.name;old.workers=request.workers;
    old.gpus=request.gpus;old.reservedCores=request.reservedCores;old.wantsResources=request.wantsResources;
    previous.push_back(old);
   }
   auto x=ReferenceRGPlanner::plan(oldTopology,previous);auto y=CPUAllocationPlanner::plan(topology,requests);
   assert(x.managed==y.managed&&x.runtimeFallback==y.runtimeFallback&&x.runtimeFallbackReason==y.runtimeFallbackReason&&
     x.allocations==y.allocations&&x.workerBudgets==y.workerBudgets&&x.allocatable==y.allocatable&&
     x.gpuReservedCPUs==y.gpuReservedCPUs&&x.gpuDeviceAllocations==y.gpuDeviceAllocations&&
     x.gpuAllocationShortages==y.gpuAllocationShortages&&x.allocatablePerformanceLevels==y.allocatablePerformanceLevels);
   assert(x.rebalanceReports.size()==y.rebalanceReports.size());
   for(unsigned i=0;i<x.rebalanceReports.size();++i)report(x.rebalanceReports[i],y.rebalanceReports[i]);
   ++cases;
  }
 }
 return cases;
}
int main() {
 unsigned cases=0;
 for(unsigned shape=0;shape<81;shape++) {
  auto code=shape;unsigned id=0;std::vector<Set> cores;std::vector<unsigned> order;
  for(unsigned c=0;c<4;c++) {Set core;for(unsigned j=0;j<1+code%3;j++){core.insert(id);order.push_back(id++);}cores.push_back(core);code/=3;}
  if(shape%2)std::reverse(order.begin(),order.end());
  for(unsigned a=0;a<=5;a++)for(unsigned b=0;b<=5;b++)for(unsigned c=0;c<=3;c++) {
   std::map<std::string,unsigned> requests={{"",a},{"A",b},{"z",c}};
   check(cores,order,requests);++cases;
  }
  for(unsigned budget=0;budget<=id;budget++)for(unsigned variant=0;variant<8;variant++) {
   std::vector<WUCPUAllocationPlanner::Request> requests={{"z",1,3,bool(variant&1)},{"a",2,4,bool(variant&2)},{"m",1,2,bool(variant&4)}};
   std::vector<ReferenceWUPlanner::Request> oldRequests;for(auto&q:requests)oldRequests.push_back({q.id,q.minimum,q.maximum,q.ownsCores});
   auto x=ReferenceWUPlanner::plan(oldRequests,budget,order,cores);
   auto y=WUCPUAllocationPlanner::plan(requests,budget,order,cores);
   assert(x.pools==y.pools&&x.workers==y.workers&&x.remaining==y.remaining);report(x.report,y.report);++cases;
  }
  check({},order,{{"A",3},{"B",7}});
  cores.pop_back();check(cores,order,{{"A",3},{"B",7}});
 }
 std::vector<Set> large;std::vector<unsigned> order;
 for(unsigned i=0;i<130;i++){large.push_back({2*i,2*i+1});order.push_back(2*i);order.push_back(2*i+1);}
 check(large,order,{{"A",1},{"B",259}});
 std::map<std::string,unsigned> many;large.resize(8);order.resize(16);
 for(unsigned i=0;i<16;i++)many[std::to_string(i)]=1;
 check(large,order,many);
 auto rgCases=compareRG();
 std::cout<<"PASS: "<<rgCases<<" exact RG comparisons and "<<cases<<" exact General comparisons, pools/workers/masks/ties/reports and limits\n";
}
