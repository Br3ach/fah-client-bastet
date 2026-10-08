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

#include "CPUWholeCorePacking.h"
#include "WUCPUAllocationPlanner.h"
#include "CPUAllocationPlanner.h"
#include "CPUExecutionPlan.h"
#include <cassert>
#include <functional>
#include <iostream>
#include <numeric>
#include <stdexcept>
using namespace FAH::Client;
using P = WUCPUAllocationPlanner;
using Set = CPUExecutionPlan::CPUSet;

unsigned total(const P::Result &r) {
  unsigned result=0;for(const auto &p:r.workers)result+=p.second;return result;
}

bool feasible(const std::vector<unsigned> &demand,const std::vector<unsigned> &widths) {
  std::vector<unsigned> capacity(demand.size());
  std::function<bool(unsigned)> assign=[&](unsigned i) {
    if(i==widths.size()) {
      for(unsigned u=0;u<demand.size();u++)if(capacity[u]<demand[u])return false;
      return true;
    }
    if(assign(i+1))return true;
    for(unsigned u=0;u<demand.size();u++){
      capacity[u]+=widths[i];if(assign(i+1))return true;capacity[u]-=widths[i];
    }
    return false;
  };
  return assign(0);
}

// Independent oracle: enumerate worker matrices, then independently assign
// physical core widths to prove each class's whole-core feasibility.
unsigned oracle(const std::vector<P::Request> &requests,const std::vector<unsigned> &budgets,
    const std::vector<std::vector<unsigned>> &widths, std::vector<unsigned> *preferred=nullptr) {
  std::vector<std::vector<unsigned>> matrix(budgets.size(),std::vector<unsigned>(requests.size()));
  std::vector<unsigned> totals(requests.size()), bestCounts(requests.size());unsigned best=0;
  std::function<void(unsigned,unsigned,unsigned)> visit=[&](unsigned l,unsigned u,unsigned left){
    if(u==requests.size()) {
      if(!feasible(matrix[l],widths[l]))return;
      if(l+1<budgets.size())visit(l+1,0,budgets[l+1]);
      else {
        unsigned sum=0;
        for(unsigned i=0;i<requests.size();i++){
          if(totals[i] && totals[i]<requests[i].minimum)return;
          sum+=totals[i];
        }
        if(sum>best||(sum==best&&totals>bestCounts)){best=sum;bestCounts=totals;}
      }
      return;
    }
    unsigned maximum=std::min(left,requests[u].maximum-totals[u]);
    for(unsigned n=0;n<=maximum;n++){
      matrix[l][u]=n;totals[u]+=n;visit(l,u+1,left-n);totals[u]-=n;
    }
  };
  visit(0,0,budgets[0]);if(preferred)*preferred=bestCounts;return best;
}

int main() {
  {auto general=P::plan({{"one",2,2}},2,{0,1,2,3},{{0,1},{2,3}});
   assert(total(general)==2&&general.allocationSlices.at("one").size()==1);}
  { CPUAllocationSlices classes={{1,{0,1}},{2,{2,3}}};
    std::vector<Set> cores={{0,1},{2,3}};
    auto r=P::planClasses({{"first",2,2},{"second",1,1}},classes,cores);
    assert(total(r)==3);
    auto a=FAH::Client::CPUExecutionPlan::createSlices(CPUExecutionPlan::CoreA8,r.allocationSlices.at("first"),cores);
    auto b=FAH::Client::CPUExecutionPlan::createSlices(CPUExecutionPlan::CoreA8,r.allocationSlices.at("second"),cores);
    assert(a&&b);Set mask=a->mask;mask.insert(b->mask.begin(),b->mask.end());
    unsigned p=0,e=0;for(auto cpu:mask)(cpu<2?p:e)++;
    assert(p==1&&e==2);
    auto one=P::planClasses({{"one",3,3}},classes,cores);
    auto execution=FAH::Client::CPUExecutionPlan::createSlices(CPUExecutionPlan::CoreA8,one.allocationSlices.at("one"),cores);
    assert(execution&&!execution->levels[0].fullSMT&&execution->levels[1].fullSMT);
  }
  { CPUAllocationSlices classes={{2,{0,1,2,3}},{2,{4,5}}};
    std::vector<Set> cores={{0,1},{2,3},{4},{5}};
    auto r=P::planClasses({{"one",4,4}},classes,cores);
    auto e=FAH::Client::CPUExecutionPlan::createSlices(CPUExecutionPlan::CoreA8,r.allocationSlices.at("one"),cores);
    assert(e&&e->mask==Set({0,2,4,5}));
  }
  { CPUAllocationSlices classes={{4,{0,1,2,3}}};
    std::vector<Set> cores={{0,1},{2,3}};
    auto r=P::planClasses({{"A",1,1},{"B",1,1},{"C",2,2}},classes,cores);
    assert(total(r)==3&&r.workers.at("A")==1&&r.workers.at("C")==2&&!r.workers.count("B"));
    auto renamed=P::planClasses({{"Z",1,1},{"Y",1,1},{"X",2,2}},classes,cores);
    assert(total(renamed)==3&&renamed.workers.at("Z")==1&&renamed.workers.at("X")==2);
    auto minimum=P::planClasses({{"too-large",5,5},{"survivor",1,3}},classes,cores);
    assert(!minimum.workers.count("too-large")&&minimum.workers.at("survivor")==3);
    auto competingMinima=P::planClasses({{"first",3,3},{"second",2,2},{"third",2,2}},classes,cores);
    assert(total(competingMinima)==4&&!competingMinima.workers.count("first"));
    auto pending=P::planClasses({{"download",2,2,false},{"run",2,2}},classes,cores);
    assert(total(pending)==4&&!pending.pools.count("download")&&pending.pools.at("run").size()==4);
    auto incomplete=P::planClasses({{"wait",5,5}},classes,cores);
    assert(incomplete.workers.empty());
  }
  { CPUAllocationSlices classes={{2,{0,1,2,3}},{0,{4,5}}};
    std::vector<Set> cores={{0,1},{2,3},{4},{5}};
    auto r=P::planClasses({{"one",2,2}},classes,cores);
    assert(r.allocationSlices.at("one")[1].pool.empty());
    auto e=FAH::Client::CPUExecutionPlan::createSlices(0x27,r.allocationSlices.at("one"),cores);
    assert(e&&e->mask==Set({0,2}));
  }
  {bool rejected=false;try{P::planClasses({{"same",1,1},{"same",1,1}},{{2,{0,1}}},{{0},{1}});}catch(const std::invalid_argument&){rejected=true;}assert(rejected);
   rejected=false;try{P::planClasses({{"one",2,2}},{{1,{0}},{1,{1}}},{{0,1}});}catch(const std::invalid_argument&){rejected=true;}assert(rejected);
   rejected=false;try{P::planClasses({{"one",1,1}},{{1,{0,1}}},{{0}});}catch(const std::invalid_argument&){rejected=true;}assert(rejected);
   assert(!FAH::Client::CPUExecutionPlan::createSlices(0xa8,{{1,{0}},{1,{0}}},{{0}}));
   assert(!FAH::Client::CPUExecutionPlan::createSlices(0xa8,{{2,{0}}},{{0}}));
  }
  // Known siblings cannot belong to separate resource slices.
  for (unsigned type: {
      CPUExecutionPlan::CoreA8, CPUExecutionPlan::CoreA9, 0x27u}) {
    const CPUAllocationSlices slices{{1, {0}}, {1, {1}}};
    assert(!CPUExecutionPlan::createSlices(type, slices, {{0, 1}}));
    assert(CPUExecutionPlan::createSlices(type, slices, {{0}, {1}}));

    // Missing or partial topology retains the conservative fallback.
    assert(CPUExecutionPlan::createSlices(type, slices, {}));
    assert(CPUExecutionPlan::createSlices(type, slices, {{0}}));

    // A partial map still identifies known cross-slice sibling conflicts.
    const CPUAllocationSlices partial{{1, {0, 2}}, {1, {1, 3}}};
    assert(!CPUExecutionPlan::createSlices(type, partial, {{0, 1}}));
  }
  {auto singleton=P::planClasses({{"one",2,2}},{{1,{0}},{1,{1}}},{});assert(total(singleton)==2);}
  {CPUAllocationSlices large(1);large[0].workers=260;std::vector<Set> cores;for(unsigned i=0;i<130;i++){Set core={2*i,2*i+1};large[0].pool.insert(core.begin(),core.end());cores.push_back(core);}
   auto limited=P::planClasses({{"small",1,1},{"large",259,259}},large,cores);
   assert(limited.report.outcome==CPUWholeCorePacking::RebalanceReport::Outcome::CoreLimit);
   assert(limited.workers.at("small")==1&&!limited.workers.count("large"));}
  {CPUAllocationSlices classes(1);classes[0].workers=16;std::vector<Set> cores;
   for(unsigned i=0;i<8;i++){Set core={2*i,2*i+1};classes[0].pool.insert(core.begin(),core.end());cores.push_back(core);}
   std::vector<P::Request> requests;for(unsigned i=0;i<16;i++)requests.push_back({std::to_string(i),1,1});
   auto limited=P::planClasses(requests,classes,cores);
   assert(total(limited)==8);
   assert(limited.report.visits<=CPUWholeCorePacking::RebalanceReport::MaxStates);
   assert(limited.report.outcome==CPUWholeCorePacking::RebalanceReport::Outcome::StateLimit);
  }
  {
    // Physical cores remain within their limit; pending chunks reach 512/513.
    using Packing = CPUWholeCorePacking;
    using Outcome = Packing::RebalanceReport::Outcome;
    CPUAllocationSlices resources(4);
    std::vector<Set> cores;
    for (unsigned i = 0; i < 128; ++i) {
      Set core{4*i, 4*i+1, 4*i+2, 4*i+3};
      const unsigned level = i < 32 ? 0 : i < 64 ? 1 : i < 97 ? 2 : 3;
      resources[level].pool.insert(core.begin(), core.end());
      cores.push_back(core);
    }
    resources[0].workers = resources[1].workers = 128;
    resources[2].workers = 129;
    resources[3].workers = 3;
    std::vector<Packing::Request> requests{{2,2,true}};
    for (unsigned i = 0; i < 126; ++i) requests.push_back({1,1,false});
    requests.push_back({1000,1000,false});
    const auto prefer = [](const auto &a, const auto &b) {return a > b;};
    // 128 physical chunks + 127 + 127 + 127 + 3 virtual chunks = 512.
    const auto boundary = Packing::pack(requests, resources, cores, prefer);
    assert(boundary.report.cores == 128 && boundary.report.groups == 128);
    assert(boundary.report.outcome != Outcome::CoreLimit);
    assert(boundary.report.outcome != Outcome::DepthLimit);
    assert(boundary.report.visits > 0 && boundary.report.visits <= Packing::RebalanceReport::MaxStates);
    resources[3].workers = 4; // One additional virtual chunk exceeds the bound.
    const auto limited = Packing::pack(requests, resources, cores, prefer);
    assert(limited.report.outcome == Outcome::DepthLimit);
    assert(limited.report.visits == 0 && !limited.report.improved);
    assert(std::string(limited.report.outcomeName()) == "search depth limit skipped");
    assert(limited.allocations == boundary.allocations);
    std::vector<unsigned> used(resources.size());
    Set owned;
    unsigned total = 0;
    for (unsigned i = 0; i < requests.size(); ++i) {
      unsigned workers = 0;
      for (unsigned level = 0; level < resources.size(); ++level) {
        const auto &slice = limited.allocations[i][level];
        workers += slice.workers; used[level] += slice.workers;
        if (!requests[i].ownsCores) assert(slice.pool.empty());
        else {
          assert(slice.workers <= slice.pool.size());
          for (auto cpu: slice.pool) {
            assert(resources[level].pool.count(cpu));
            assert(owned.insert(cpu).second);
          }
        }
      }
      assert(workers <= requests[i].maximum);
      assert(!workers || workers >= requests[i].minimum);
      assert(workers == (i == 0 ? 2u : i + 1 == requests.size() ? 0u : 1u));
      total += workers;
    }
    for (unsigned level = 0; level < resources.size(); ++level)
      assert(used[level] <= resources[level].workers);
    for (const auto &core: cores) {
      unsigned present = 0;
      for (auto cpu: core) present += owned.count(cpu);
      assert(!present || present == core.size());
    }
    assert(total == 128 && !owned.empty());
  }
  {
    // Throughput is maximal, but the preferred first request cannot meet its minimum.
    std::vector<CPUWholeCorePacking::Request> requests{{3,3,true},{2,2,true}};
    CPUAllocationSlices resources{{2,{0,1}}};
    std::vector<Set> cores{{0,1}};
    auto prefer=[](const auto &a,const auto &b){return a>b;};
    auto baseline=CPUWholeCorePacking::pack(requests,resources,cores,prefer);
    auto searched=CPUWholeCorePacking::pack(requests,resources,cores,prefer,{3,0});
    assert(baseline.report.outcome==CPUWholeCorePacking::RebalanceReport::Outcome::NotNeeded);
    assert(searched.allocations==baseline.allocations);
    assert(searched.report.visits>0&&!searched.report.improved);
    assert(searched.report.outcome==CPUWholeCorePacking::RebalanceReport::Outcome::NoImprovement);
    assert(std::string(searched.report.outcomeName())=="no better packing found");
  }
  assert(!FAH::Client::CPUExecutionPlan::createSlices(0xa8,{},{}));
  assert(!FAH::Client::CPUExecutionPlan::createSlices(0xa8,{{0,{}}},{}));
  { auto r=P::planClasses({{"first",2,2},{"second",2,2}},{{4,{0,1,2,3}}},{{0},{1,2},{3}}); assert(total(r)==4); }
  {
    CPUAllocationPlanner::Topology topology;
    topology.hardAffinity = topology.effectiveClasses = true;
    topology.available = {0,1,2,3};
    topology.performanceLevels= {{0,1},{2,3}};
    topology.coreThreads = {{0,1},{2,3}};
    CPUAllocationPlanner::Request request;
    request.name = "mixed"; request.classes = true;
    request.workers = 3; request.classCounts = {1,2};
    auto rg = CPUAllocationPlanner::plan(topology,{request});
    assert(rg.workerBudgets.at("mixed") == 3);
    auto wu = P::planClasses({{"one",3,3}},rg.allocationSlices.at("mixed"),topology.coreThreads);
    auto execution = FAH::Client::CPUExecutionPlan::createSlices(0x27,wu.allocationSlices.at("one"),topology.coreThreads);
    assert(execution && execution->mask == Set({0,2,3}));
    auto multiple = P::planClasses({{"first",2,2},{"second",1,1}},rg.allocationSlices.at("mixed"),topology.coreThreads);
    unsigned perClass[2] = {};
    for (const auto &entry: multiple.allocationSlices) {
      auto e = FAH::Client::CPUExecutionPlan::createSlices(0x27,entry.second,topology.coreThreads);
      assert(e);
      for (unsigned level=0;level<2;++level) perClass[level] += unsigned(e->levels[level].mask.size());
    }
    assert(perClass[0]==1 && perClass[1]==2);
    auto corrupted = rg;
    ++corrupted.allocationSlices.at("mixed")[0].workers;
    assert(!CPUAllocationPlanner::validate(corrupted,topology,{request}));
    assert(corrupted.managed && corrupted.allocationSlices.empty() && corrupted.allocations.empty());
    topology.available={0,1}; topology.performanceLevels={{0,1},{}};
    auto reduced=CPUAllocationPlanner::plan(topology,{request});
    assert(reduced.allocationSlices.at("mixed")[0].workers==1);
    assert(reduced.allocationSlices.at("mixed")[1].workers==0);
  }
  unsigned cases=0;
  for(unsigned pwidth=1;pwidth<=3;pwidth++)for(unsigned ewidth=1;ewidth<=3;ewidth++)
  for(unsigned pbudget=1;pbudget<=3;pbudget++)for(unsigned ebudget=1;ebudget<=3;ebudget++){
    CPUAllocationSlices classes(2);std::vector<Set> cores;unsigned id=0;
    for(unsigned l=0;l<2;l++)for(unsigned c=0;c<2;c++){
      Set core;for(unsigned j=0;j<(l?ewidth:pwidth);j++){core.insert(id);classes[l].pool.insert(id++);}cores.push_back(core);
    }
    if(pbudget>classes[0].pool.size()||ebudget>classes[1].pool.size())continue;
    classes[0].workers=pbudget;classes[1].workers=ebudget;
    for(unsigned a=1;a<=3;a++)for(unsigned b=1;b<=3;b++){
      std::vector<P::Request> requests={{"z",1,a},{"a",b,b},{"m",1,2}};
      auto r=P::planClasses(requests,classes,cores);
      std::vector<unsigned> preferred;
      assert(total(r)==oracle(requests,{pbudget,ebudget},{{pwidth,pwidth},{ewidth,ewidth}},&preferred));
      for(unsigned i=0;i<requests.size();++i) {
        auto count=r.workers.find(requests[i].id);
        assert((count==r.workers.end()?0:count->second)==preferred[i]);
      }
      auto again=P::planClasses(requests,classes,cores);assert(r.allocationSlices==again.allocationSlices);
      for(auto type:{CPUExecutionPlan::CoreA8,CPUExecutionPlan::CoreA9,0x27u}){
        Set seen;
        for(const auto &entry:r.allocationSlices){
          auto execution=FAH::Client::CPUExecutionPlan::createSlices(type,entry.second,cores);assert(execution);
          for(auto cpu:execution->mask)assert(seen.insert(cpu).second);
          for(unsigned l=0;l<2;l++){
            unsigned exposed=0;for(auto cpu:execution->levels[l].mask){assert(classes[l].pool.count(cpu));exposed++;}
            assert(exposed>=entry.second[l].workers);
          }
        }
      }
      cases++;
    }
  }
  std::cout<<"PASS: class/WU/mask regressions and "<<cases<<" independent throughput-oracle cases\n";
}
