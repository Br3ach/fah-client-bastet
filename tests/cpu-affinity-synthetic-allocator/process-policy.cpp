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

#include "CPUExecutionPlan.h"
#include "CPUWholeCorePacking.h"
#include <algorithm>
#include <cassert>
#include <iostream>
using P=FAH::Client::CPUExecutionPlan;
// Exhaust small mixed-width topologies and three competing worker requests.
// Under-allocation can be necessary at whole-core granularity. Check safety
// and execution-policy invariants without requiring the greedy repair to be optimal.
static unsigned partitionProperties() {
 unsigned cases=0;
 for(unsigned x:{1u,2u,3u,6u})for(unsigned y:{1u,2u,3u,6u})for(unsigned z:{1u,2u,3u,6u}) {
  std::vector<P::CPUSet> cores;unsigned next=0;
  for(unsigned width:{x,y,z}) {P::CPUSet core;while(width--)core.insert(next++);cores.push_back(core);}
  // Include an unavailable/reserved whole core and different physical-core orders.
  for(unsigned removed:{0u,1u})for(unsigned rotation=0;rotation<3;++rotation) {
   std::vector<unsigned> order;
   for(unsigned i=0;i<3;++i) {
    unsigned index=(i+rotation)%3;
    if(removed && index==0)continue;
    order.insert(order.end(),cores[index].begin(),cores[index].end());
   }
   P::CPUSet available(order.begin(),order.end());unsigned capacity=available.size();
   for(unsigned a=0;a<=capacity;++a)for(unsigned b=0;b<=capacity-a;++b)for(unsigned c=0;c<=capacity-a-b;++c)
    for(bool fill:{false,true}) {
     std::map<std::string,unsigned> requests={{"A",a},{"B",b},{"C",c}};
     auto pools=FAH::Client::CPUWholeCorePacking::partition(requests,order,cores,fill);P::CPUSet occupied;
     for(const auto &entry:pools) {
      assert(requests.at(entry.first)>0 || entry.second.empty());
      for(auto cpu:entry.second)assert(available.count(cpu)&&occupied.insert(cpu).second);
      for(const auto &core:cores) {
       unsigned owned=0;for(auto cpu:core)owned+=entry.second.count(cpu);
       assert(owned==0||owned==core.size());
      }
      unsigned workers=std::min<unsigned>(requests.at(entry.first),entry.second.size());
      assert(workers<=entry.second.size());
      for(unsigned type:{0xa8u,0xa9u,0x99u}) {
       auto plan=P::create(type,workers,entry.second,cores,order);
       for(auto cpu:plan.mask)assert(entry.second.count(cpu));
       if(!workers){assert(plan.mask.empty());continue;}
       assert(plan.mask.size()==((type==0xa8||type==0xa9)&&workers>plan.physical?entry.second.size():workers));
       if(workers<=plan.physical)for(const auto &core:cores) {
        unsigned used=0;for(auto cpu:core)used+=plan.mask.count(cpu);assert(used<=1);
       }
      }
     }
     ++cases;
    }
  }
 }
 return cases;
}
int main(){
 // Partial optimum: one-worker groups must not consume both SMT cores.
 {
  FAH::Client::CPUWholeCorePacking::RebalanceReport report;
  auto pools=FAH::Client::CPUWholeCorePacking::partition({{"A",1},{"B",1},{"C",2}},
    {0,1,2,3},{{0,1},{2,3}},false,&report);
  assert(pools["A"].size()==2&&pools["B"].empty()&&pools["C"].size()==2);
  assert(report.improved&&report.outcome==FAH::Client::CPUWholeCorePacking::RebalanceReport::Outcome::Improved);
 }
 // Independent exhaustive oracle includes infeasible three-group requests.
 for(unsigned w0:{1u,2u,3u})for(unsigned w1:{1u,2u,3u})for(unsigned w2:{1u,2u}) {
  std::vector<P::CPUSet> cores;std::vector<unsigned> order;unsigned lp=0;
  for(auto width:{w0,w1,w2}){P::CPUSet core;while(width--){core.insert(lp);order.push_back(lp++);}cores.push_back(core);}
  for(unsigned a=0;a<=4;++a)for(unsigned b=0;b<=4;++b)for(unsigned c=0;c<=4;++c){
   std::vector<unsigned> demand{a,b,c}, best(3);unsigned optimum=0;
   auto fairer=[](auto x,auto y){auto sx=x,sy=y;std::sort(sx.begin(),sx.end());std::sort(sy.begin(),sy.end());return sx!=sy?sx>sy:x>y;};
   for(unsigned code=0;code<27;++code){unsigned n=code;std::vector<unsigned> caps(3);
    for(const auto &core:cores){caps[n%3]+=core.size();n/=3;}
    unsigned total=0;std::vector<unsigned> candidate(3);
    for(unsigned i=0;i<3;++i){candidate[i]=std::min(caps[i],demand[i]);total+=candidate[i];}
    if(total>optimum||(total==optimum&&fairer(candidate,best))){optimum=total;best=candidate;}
   }
   auto pools=FAH::Client::CPUWholeCorePacking::partition({{"A",a},{"B",b},{"C",c}},order,cores,false);
   unsigned fulfilled=0;for(auto entry:std::map<std::string,unsigned>{{"A",a},{"B",b},{"C",c}})
    fulfilled+=std::min<unsigned>(entry.second,pools[entry.first].size());
   assert(fulfilled==optimum);
   assert(std::vector<unsigned>({std::min<unsigned>(a,pools["A"].size()),
     std::min<unsigned>(b,pools["B"].size()),std::min<unsigned>(c,pools["C"].size())})==best);
  }
 }
 // Four groups require a multi-core rebalance rather than a single swap.
 for (bool fill: {false,true}) {
  const std::map<std::string,unsigned> requests{{"A",1},{"B",1},{"C",2},{"D",3}};
  const std::vector<P::CPUSet> cores{{0,1},{2,3},{4},{5},{6}};
  FAH::Client::CPUWholeCorePacking::RebalanceReport report;
  auto pools=FAH::Client::CPUWholeCorePacking::partition(requests,{0,1,2,3,4,5,6},cores,fill,&report);
  assert(report.outcome==FAH::Client::CPUWholeCorePacking::RebalanceReport::Outcome::Improved&&report.visits>0);
  assert(pools==FAH::Client::CPUWholeCorePacking::partition(requests,{0,1,2,3,4,5,6},cores,fill));
  P::CPUSet used;
  for(const auto &request:requests) {
   assert(pools.at(request.first).size()==request.second);
   for(auto cpu:pools.at(request.first))assert(used.insert(cpu).second);
   for(const auto &core:cores) {
    unsigned owned=0;for(auto cpu:core)owned+=pools.at(request.first).count(cpu);
    assert(owned==0 || owned==core.size());
   }
  }
 }
 // Beyond the search depth bound, retain deterministic safe under-allocation.
 {
  std::vector<P::CPUSet> cores;std::vector<unsigned> order;
  for(unsigned i=0;i<129;++i){cores.push_back({2*i,2*i+1});order.push_back(2*i);order.push_back(2*i+1);}
  const std::map<std::string,unsigned> requests{{"A",257},{"B",1}};
  FAH::Client::CPUWholeCorePacking::RebalanceReport report;
  auto pools=FAH::Client::CPUWholeCorePacking::partition(requests,order,cores,false,&report);
  assert(report.outcome==FAH::Client::CPUWholeCorePacking::RebalanceReport::Outcome::CoreLimit&&report.cores==129&&report.visits==0);
  // No repair can remove a two-LP core from the satisfied one-worker group.
  assert(pools["B"]==P::CPUSet({2,3})&&pools["A"].size()==256);
  assert(pools==FAH::Client::CPUWholeCorePacking::partition(requests,order,cores,false));
  for(auto lp:pools["A"])assert(!pools["B"].count(lp));
  assert(pools["A"].size()<257);
 }
 // Distinguish exhausted search from an exhaustive infeasibility result.
 {
  FAH::Client::CPUWholeCorePacking::RebalanceReport report;
  const std::map<std::string,unsigned> requests{{"A",3},{"B",3}};
  const auto pools=FAH::Client::CPUWholeCorePacking::partition(requests,{0,1,2,3,4,5},{{0,1},{2,3},{4,5}},false,&report);
  assert(report.outcome==FAH::Client::CPUWholeCorePacking::RebalanceReport::Outcome::Infeasible&&report.visits>0);
  assert(pools.at("A")==P::CPUSet({0,1,4,5})&&pools.at("B")==P::CPUSet({2,3}));
 }
 {
  std::vector<P::CPUSet> cores;std::vector<unsigned> order;
  for(unsigned i=0;i<72;++i){cores.push_back({2*i,2*i+1});order.push_back(2*i);order.push_back(2*i+1);}
  std::map<std::string,unsigned> requests;
  for(unsigned i=0;i<48;++i)requests[std::string(1,char('A'+i))]=3;
  FAH::Client::CPUWholeCorePacking::RebalanceReport report;
  const auto pools=FAH::Client::CPUWholeCorePacking::partition(requests,order,cores,false,&report);
  assert(report.outcome==FAH::Client::CPUWholeCorePacking::RebalanceReport::Outcome::StateLimit&&report.visits==report.MaxStates);
  // Search exhaustion may improve packing but must never reduce throughput.
  unsigned total=0;P::CPUSet used;
  for(const auto &entry:requests) {
   auto found=pools.find(entry.first);if(found==pools.end())continue;
   total+=std::min<unsigned>(entry.second,found->second.size());
   for(auto cpu:found->second)assert(used.insert(cpu).second);
   for(const auto &core:cores)if(found->second.count(*core.begin()))
    for(auto cpu:core)assert(found->second.count(cpu));
  }
  assert(total>=120);
 }
 {
  FAH::Client::CPUWholeCorePacking::RebalanceReport report;report.outcome=FAH::Client::CPUWholeCorePacking::RebalanceReport::Outcome::StateLimit;
  FAH::Client::CPUWholeCorePacking::partition({{"A",1}},{0,1},{{0,1}},false,&report);
  assert(report.outcome==FAH::Client::CPUWholeCorePacking::RebalanceReport::Outcome::NotNeeded&&report.visits==0);
 }
 // Independent exhaustive ownership oracle for four-group mixed-width cases.
 // Enumerate every assignment, then check all small fitting request vectors.
 for (unsigned first: {2u,3u,6u}) {
  std::vector<P::CPUSet> cores;std::vector<unsigned> order;unsigned cpu=0;
  for(unsigned width: {first,2u,1u,1u,1u}) {
   P::CPUSet core;while(width--){core.insert(cpu);order.push_back(cpu++);}cores.push_back(core);
  }
  std::vector<std::vector<unsigned>> feasible;
  for(unsigned encoded=0;encoded<1024;++encoded) {
   std::vector<unsigned> capacity(4);unsigned value=encoded;
   for(const auto &core:cores){capacity[value%4]+=core.size();value/=4;}
   feasible.push_back(capacity);
  }
  for(unsigned a=1;a<=3;++a)for(unsigned b=1;b<=3;++b)
   for(unsigned c=1;c<=3;++c)for(unsigned d=1;d<=3;++d) {
    if(a+b+c+d>cpu)continue;
    bool fits=false;
    for(const auto &cap:feasible)fits|=cap[0]>=a&&cap[1]>=b&&cap[2]>=c&&cap[3]>=d;
    const std::map<std::string,unsigned> requests{{"A",a},{"B",b},{"C",c},{"D",d}};
    for(bool fill: {false,true}) {
     auto pools=FAH::Client::CPUWholeCorePacking::partition(requests,order,cores,fill);P::CPUSet used;
     for(const auto &request:requests) {
      if(fits)assert(pools[request.first].size()>=request.second);
      for(auto lp:pools[request.first])assert(used.insert(lp).second);
      for(const auto &core:cores){unsigned n=0;for(auto lp:core)n+=pools[request.first].count(lp);assert(!n||n==core.size());}
     }
    }
   }
 }
 auto properties=partitionProperties();
 for(unsigned physical:{7u,8u}) {
  P::CPUSet pool;std::vector<P::CPUSet> cores;
  for(unsigned i=0;i<physical;++i){cores.push_back({2*i,2*i+1});pool.insert(2*i);pool.insert(2*i+1);}
  for(unsigned type:{0xa8u,0xa9u})for(unsigned n=1;n<=2*physical;++n){
   auto p=P::create(type,n,pool,cores);assert(p.physical==physical);
   assert(p.mask.size()==(n<=physical?n:2*physical));assert(p.fullSMT==(n==2*physical));
   if(n<=physical)for(auto cpu:p.mask)assert(cpu%2==0);
  }
  assert(P::create(0xa8,2*physical+1,pool,cores).mask.empty());
  assert(P::create(0x99,physical+1,pool,cores).mask.size()==physical+1);
  auto fallback=P::create(0xa8,physical+1,pool,{});assert(fallback.mask.size()==physical+1 && !fallback.fullSMT);
  std::vector<unsigned> order(pool.begin(),pool.end());
  auto split=FAH::Client::CPUWholeCorePacking::partition({{"A",physical},{"B",physical}},order,cores);
  for(auto cpu:split["A"])assert(!split["B"].count(cpu));
  for(auto core:cores){unsigned owners=0;for(auto entry:split)for(auto cpu:core)if(entry.second.count(cpu)){++owners;break;}assert(owners<=1);}
 }
 auto mixed=P::create(0xa8,7,{0,1,2,3,4,5,6,7},{{0,1,2,3,4,5},{6,7}});
 assert(mixed.mask.size()==8 && mixed.physical==2 && !mixed.fullSMT);
 auto partial=P::create(0xa8,3,{0,1,2,3},{{0,1}});assert(partial.mask.size()==3 && !partial.hasSMT);
 auto overlap=P::create(0xa8,3,{0,1,2},{{0,1},{1,2}});assert(overlap.mask.size()==3 && !overlap.hasSMT);
 auto preferred=P::create(0xa8,1,{0,1,2,3},{{0,1},{2,3}},{2,0,3,1});assert(preferred.mask==P::CPUSet({2}));
 // High-numbered fast CPUs must retain priority even without a usable core map.
 for(unsigned type:{0xa8u,0xa9u,0x99u}) {
  assert(P::create(type,1,{0,1,8,9},{},{8,9,0,1}).mask==P::CPUSet({8}));
  assert(P::create(type,2,{0,1,8,9},{{8,9}},{8,9,0,1}).mask==P::CPUSet({8,9}));
 }
 // Other core types also preserve preference when filling additional siblings.
 assert(P::create(0x99,3,{0,1,8,9},{{8,9},{0,1}},{8,0,9,1}).mask==P::CPUSet({0,8,9}));
 std::vector<P::CPUSet> hybrid;std::vector<unsigned> ordered;
 for(unsigned i=0;i<7;++i)hybrid.push_back({2*i,2*i+1});
 for(unsigned i=14;i<22;++i)hybrid.push_back({i});
 for(const auto &core:hybrid)for(auto cpu:core)ordered.push_back(cpu);
 for(unsigned a=1;a<22;++a)for(unsigned b=1;a+b<=22;++b){
  auto pools=FAH::Client::CPUWholeCorePacking::partition({{"A",a},{"B",b}},ordered,hybrid,false);
  assert(pools["A"].size()>=a && pools["B"].size()>=b);
  for(auto cpu:pools["A"])assert(!pools["B"].count(cpu));
  for(const auto &core:hybrid)for(const auto &entry:pools){
   unsigned n=0;for(auto cpu:core)n+=entry.second.count(cpu);
   assert(n==0||n==core.size());
  }
 }
 std::cout<<"PASS: "<<properties<<" mixed-width partition/property cases\n";
 std::cout<<"PASS: a8/a9 physical-first/full-pool policy, full-SMT warning, mixed widths, fallback, per-WU isolation\n";
}
