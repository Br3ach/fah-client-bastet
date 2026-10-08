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

#include "WUCPUAllocationPlanner.h"
#include "CPUExecutionPlan.h"
#include <cassert>
#include <algorithm>
#include <iostream>
#include <stdexcept>
using namespace FAH::Client;
int main() {
  using P = WUCPUAllocationPlanner;
  P::Request request;
  assert(request.id.empty() && request.minimum == 0 &&
         request.maximum == 0 && request.ownsCores);
  auto result=P::plan({{"A",3,3},{"B",1,3}},4,{0,2,1,3},{{0,1},{2,3}});
  assert(!result.workers.count("A") && result.workers.at("B")==3);
  assert(result.pools.at("B")==CPUExecutionPlan::CPUSet({0,1,2,3}));
  assert(result.remaining==0);
  // Provisional packing must still reject WUs below their actual minima.
  auto minima=P::plan({{"A",3,3},{"B",3,3}},6,{0,1,2,3,4,5},{{0,1},{2,3},{4,5}});
  assert(minima.workers.size()==1&&minima.workers.at("A")==3);
  // Count allocation and ownership share request priority, even with reversed IDs.
  auto regular=P::plan({{"A",1,3},{"B",1,3}},4,{0,2,1,3},{{0,1},{2,3}});
  auto renamed=P::plan({{"Z",1,3},{"A",1,3}},4,{0,2,1,3},{{0,1},{2,3}});
  assert(regular.pools.at("A")==renamed.pools.at("Z"));
  assert(regular.pools.at("B")==renamed.pools.at("A"));
  assert(renamed.workers.at("Z")==2&&renamed.workers.at("A")==2);
  for(const auto &requests:std::vector<std::vector<P::Request>>{
      {{"Z",3,3},{"A",1,3}},{{"A",1,3},{"Z",3,3}}}){
    auto allocation=P::plan(requests,4,{0,2,1,3},{{0,1},{2,3}});
    unsigned total=0;for(const auto &worker:allocation.workers)total+=worker.second;
    assert(total==3);
  }
  // Genuine unassigned budget stays available; a failed minimum does not
  // manufacture a new assignment after survivor recovery consumed its pool.
  auto spare=P::plan({{"A",1,2}},4,{0,2,1,3},{{0,1},{2,3}});
  assert(spare.remaining==2&&spare.workers.at("A")==2);
  // More than ten candidates keeps the same ordinal priority across digit widths.
  std::vector<P::Request> many;std::vector<unsigned> cpus;
  for(unsigned i=0;i<12;++i){many.push_back({"WU"+std::to_string(12-i),1,1});cpus.push_back(i);}
  auto ordered=P::plan(many,12,cpus,{});
  for(unsigned i=0;i<12;++i)assert(ordered.pools.at(many[i].id)==CPUExecutionPlan::CPUSet({i}));
  // A new runnable WU must not reclaim unused cores from a one-worker peer.
  const std::vector<unsigned> fourCoreOrder={0,2,4,6,1,3,5,7};
  const std::vector<CPUExecutionPlan::CPUSet> fourCores={{0,1},{2,3},{4,5},{6,7}};
  auto single=P::plan({{"A",1,1}},8,fourCoreOrder,fourCores);
  auto waiting=P::plan({{"A",1,1},{"B",1,1,false}},8,fourCoreOrder,fourCores);
  auto concurrent=P::plan({{"A",1,1},{"B",1,1}},8,fourCoreOrder,fourCores);
  assert(single.pools.at("A")==CPUExecutionPlan::CPUSet({0,1}));
  assert(waiting.pools.at("A")==single.pools.at("A"));
  assert(concurrent.pools.at("A")==single.pools.at("A"));
  assert(concurrent.pools.at("B")==CPUExecutionPlan::CPUSet({2,3}));
  for(unsigned workers=1;workers<=8;++workers){
    auto planned=P::plan({{"A",workers,workers}},8,fourCoreOrder,fourCores);
    assert(planned.workers.at("A")==workers);
    assert(planned.pools.at("A").size()==2*std::min(workers,4u));
  }
  unsigned cases=0;
  for(const auto &cores:std::vector<std::vector<CPUExecutionPlan::CPUSet>>{
      {{0,1},{2,3}},{{0,1},{2},{3}}, {}})
    for(unsigned minA=1;minA<=3;++minA)for(unsigned maxA=minA;maxA<=4;++maxA)
      for(unsigned minB=1;minB<=3;++minB)for(unsigned maxB=minB;maxB<=4;++maxB)
        for(unsigned budget=0;budget<=4;++budget){
          const std::vector<P::Request> requests={{"A",minA,maxA},{"B",minB,maxB}};
          auto planned=P::plan(requests,budget,{0,2,1,3},cores);
          auto repeat=P::plan(requests,budget,{0,2,1,3},cores);
          assert(planned.pools==repeat.pools&&planned.workers==repeat.workers);
          auto renamed=P::plan({{"Z",minA,maxA},{"A",minB,maxB}},budget,{0,2,1,3},cores);
          for(const auto &ids:std::vector<std::pair<std::string,std::string>>{{"A","Z"},{"B","A"}}){
            auto a=planned.workers.find(ids.first),b=renamed.workers.find(ids.second);
            assert((a==planned.workers.end())==(b==renamed.workers.end()));
            if(a!=planned.workers.end())assert(a->second==b->second&&
              planned.pools.at(ids.first)==renamed.pools.at(ids.second));
          }
          unsigned total=0;std::set<unsigned> owned;
          for(const auto &request:requests){
            auto found=planned.workers.find(request.id);
            if(found==planned.workers.end())continue;
            assert(found->second>=request.minimum&&found->second<=request.maximum);
            assert(found->second<=planned.pools.at(request.id).size());
            total+=found->second;
            for(auto cpu:planned.pools.at(request.id))assert(cpu<4&&owned.insert(cpu).second);
          }
          assert(total<=budget);
          for(const auto &core:cores){
            unsigned owners=0;
            for(const auto &pool:planned.pools)if(pool.second.count(*core.begin())){
              ++owners;for(auto cpu:core)assert(pool.second.count(cpu));
            }
            assert(owners<=1);
          }
          ++cases;
        }
  auto pending=P::plan({{"running",1,2},{"pending",2,2,false}},4,{0,2,1,3},{{0,1},{2,3}});
  assert(pending.workers.at("pending")==2 && !pending.pools.count("pending"));
  assert(pending.workers.at("running")==2 && pending.pools.at("running").size()==4);
  auto ready=P::plan({{"running",1,2},{"pending",2,2,true}},4,{0,2,1,3},{{0,1},{2,3}});
  assert(ready.pools.at("running").size()==2 && ready.pools.at("pending").size()==2);
  auto unknown=P::plan({{"A",1,1}},8,fourCoreOrder,{});
  assert(unknown.pools.at("A")==CPUExecutionPlan::CPUSet({0}));
  // Reject collisions even with no budget or worker-only pending requests.
  for (unsigned budget: {0u,4u}) for (bool owns: {false,true}) {
    bool rejected=false;
    try {P::plan({{"same",1,2,true},{"same",1,2,owns}},
      budget,{0,1,2,3},{{0,1},{2,3}});}
    catch(const std::invalid_argument &e) {
      rejected=std::string(e.what())=="Duplicate WU request ID";
    }
    assert(rejected);
  }
  assert(cases==1215);
  std::cout<<"PASS: pure WU planner minimum recovery and 1215 budget/SMT cases\n";
}
