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

// Direct production-planner tests need no cbang, Groups, Config or OS stubs.
#include "CPUWholeCorePacking.h"
#include "CPUAllocationPlanner.h"
#include <cassert>
#include <iostream>
#include <stdexcept>
using namespace FAH::Client;
using P = CPUAllocationPlanner;
static bool equal(const P::Result &a, const P::Result &b) {
  return a.rebalanceReports == b.rebalanceReports &&
    a.managed == b.managed && a.runtimeFallback == b.runtimeFallback &&
    a.runtimeFallbackReason == b.runtimeFallbackReason && a.allocatable == b.allocatable &&
    a.gpuReservedCPUs == b.gpuReservedCPUs && a.allocatablePerformanceLevels == b.allocatablePerformanceLevels &&
    a.allocations == b.allocations && a.workerBudgets == b.workerBudgets &&
    a.allocationSlices == b.allocationSlices &&
    a.gpuDeviceAllocations == b.gpuDeviceAllocations && a.gpuAllocationShortages == b.gpuAllocationShortages;
}
int main() {
  P::Topology t;
  t.hardAffinity = t.homogeneous = true;
  for (unsigned cpu=0; cpu<16; ++cpu) t.available.insert(cpu);
  for (unsigned core=0; core<8; ++core) t.coreThreads.push_back({2*core,2*core+1});
  t.performanceLevels= {t.available};
  t.fastPhysicalCores = t.coreThreads;
  P::Request a,b,g;
  a.name="A";b.name="B";g.name="GPU";g.gpus={"gpu0"};g.reservedCores=1;
  for (bool gpu: {false, true}) for (const auto &name: {"", "duplicate"}) {
    P::Request first, second;first.name=second.name=name;
    first.workers=1;second.workers=2;
    if (gpu) {first.gpus={"GPU0"};first.reservedCores=1;
      second.gpus={"GPU1"};second.reservedCores=2;}
    bool rejected=false;
    try {P::plan(t,{first,second});}
    catch (const std::invalid_argument &error) {
      rejected=std::string(error.what()).find("Duplicate CPU allocation request name:")==0;
    }
    assert(rejected);
  }
  std::cout<<"PASS: duplicate CPU and GPU requests rejected, including Default names\n";
  // Inactive saved demand owns no CPUs/reservations and cannot activate classes.
  {
    a.workers=8;b.workers=16;a.wantsResources=false;
    auto released=P::plan(t,{a,b});
    assert(released.workerBudgets.at("A")==0 && released.workerBudgets.at("B")==16);
    assert(!released.allocations.count("A") && a.workers==8);
    a.wantsResources=true;auto resumed=P::plan(t,{a,b});
    assert(resumed.workerBudgets.at("A")==8 && resumed.workerBudgets.at("B")==8);
    g.wantsResources=false;released=P::plan(t,{b,g});
    assert(released.gpuReservedCPUs.empty() && released.workerBudgets.at("B")==16);
    g.wantsResources=true;released=P::plan(t,{b,g});
    assert(released.gpuReservedCPUs.size()==2 && released.workerBudgets.at("B")==14);
    auto hybrid=t;hybrid.homogeneous=false;hybrid.effectiveClasses=true;
    hybrid.performanceLevels={{0,1,2,3,4,5,6,7},{8,9,10,11,12,13,14,15}};
    a.classes=true;a.classCounts={8,0};a.wantsResources=false;
    assert(!P::plan(hybrid,{a,b}).managed);
    a.classes=false;a.classCounts.clear();a.wantsResources=true;
  }
  // General overcommit never removes explicit P/E boundaries. Class shortage
  // shrinks within the level and keeps unrelated class requests isolated.
  {
    auto hybrid=t;hybrid.homogeneous=false;hybrid.effectiveClasses=true;
    hybrid.performanceLevels={{0,1,2,3,4,5,6,7},{8,9,10,11,12,13,14,15}};
    P::Request fast,slow,general;fast.name="A";slow.name="B";general.name="C";
    fast.classes=slow.classes=true;
    for(unsigned p=1;p<=12;++p)for(unsigned e=0;e<=12;++e)for(unsigned n=0;n<=24;++n){
      fast.workers=p;fast.classCounts={p,0};slow.workers=e;slow.classCounts={0,e};general.workers=n;
      auto allocation=P::plan(hybrid,{fast,slow,general});
      for(auto cpu:allocation.allocations["A"])assert(cpu<8);
      for(auto cpu:allocation.allocations["B"])assert(cpu>=8);
      assert(allocation.workerBudgets["A"]==std::min(p,8u));
      assert(allocation.workerBudgets["B"]==std::min(e,8u));
      assert(allocation.workerBudgets["C"]<=n);
    }
    fast.workers=8;fast.classCounts={8,0};slow.workers=8;slow.classCounts={0,8};
    hybrid.available={0,1,2,3,8,9,10,11,12,13,14,15};hybrid.performanceLevels[0]={0,1,2,3};
    hybrid.coreThreads={{0,1},{2,3},{8,9},{10,11},{12,13},{14,15}};hybrid.fastPhysicalCores={{0,1},{2,3}};
    auto restricted=P::plan(hybrid,{fast,slow});
    assert(restricted.workerBudgets["A"]==4 && restricted.workerBudgets["B"]==8);
    for(auto cpu:restricted.allocations["A"])assert(cpu<8);
    for(auto cpu:restricted.allocations["B"])assert(cpu>=8);
    // Entirely unavailable P level must not migrate its work onto E CPUs.
    hybrid.available={8,9,10,11,12,13,14,15};hybrid.performanceLevels[0].clear();
    hybrid.coreThreads={{8,9},{10,11},{12,13},{14,15}};hybrid.fastPhysicalCores.clear();
    auto missing=P::plan(hybrid,{fast,slow});
    assert(missing.workerBudgets["A"]==0 && missing.workerBudgets["B"]==8);
    assert(!missing.allocations.count("A"));
    // Unknown class topology blocks class work while General keeps usable CPUs.
    hybrid.effectiveClasses=false;general.workers=8;
    auto unknown=P::plan(hybrid,{fast,general});
    assert(unknown.workerBudgets["A"]==0 && unknown.workerBudgets["C"]==8);
  }
  // Whole-core partial packing maximizes workers across three General RGs.
  {
    P::Topology small;small.hardAffinity=small.homogeneous=true;
    small.available={0,1,2,3};small.coreThreads={{0,1},{2,3}};
    small.performanceLevels={small.available};
    std::vector<P::Request> requests;
    for(auto entry:std::map<std::string,unsigned>{{"A",1},{"B",1},{"C",2}}){
      P::Request request;request.name=entry.first;request.workers=entry.second;requests.push_back(request);
    }
    auto result=P::plan(small,requests);
    assert(result.workerBudgets["A"]==1&&result.workerBudgets["B"]==0&&result.workerBudgets["C"]==2);
    assert(result.runtimeFallback&&result.rebalanceReports[0].improved);
  }
  // Production assignment diagnostics survive the detached planner result.
  {
    P::Topology mixed;
    mixed.hardAffinity=mixed.homogeneous=true;
    mixed.available={0,1,2,3,4,5,6};mixed.coreThreads={{0,1},{2,3},{4},{5},{6}};
    mixed.performanceLevels={mixed.available};
    std::vector<P::Request> requests;
    for(const auto &entry:std::map<std::string,unsigned>{{"A",1},{"B",1},{"C",2},{"D",3}}){
      P::Request request;request.name=entry.first;request.workers=entry.second;requests.push_back(request);
    }
    auto result=P::plan(mixed,requests);
    assert(result.rebalanceReports.size()==1);
    assert(result.rebalanceReports[0].outcome==CPUWholeCorePacking::RebalanceReport::Outcome::Improved);
    assert(equal(result,P::plan(mixed,requests)));
  }
  // A one-worker RG owns one SMT core; adding a peer leaves its pool stable.
  a.workers=1;auto single=P::plan(t,{a});b.workers=1;auto peers=P::plan(t,{a,b});
  assert(single.allocations.at("A")==P::CPUList({0,1}));
  assert(peers.allocations.at("A")==single.allocations.at("A"));
  assert(peers.allocations.at("B")==P::CPUList({2,3}));
  // Partial topology must neither lose unknown LPs nor publish split ownership.
  // Sweep class and General demand through bounded physical spreading.
  unsigned partialCases = 0;
  for (const auto &cores: std::vector<std::vector<P::CPUSet>>{
      {}, {{8,9},{0}}, {{8,9},{0,1},{2}}, {{},{8,9},{0,1},{2}}}) {
    P::Topology partial;
    partial.hardAffinity = partial.effectiveClasses = true;
    partial.available = {0,1,2,3,8,9};
    partial.performanceLevels= {{8,9},{0,1,2,3}};
    partial.coreThreads = cores;
    const auto order = P::orderCPUs(partial.available, cores);
    assert(P::CPUSet(order.begin(), order.end()) == partial.available);
    assert(order.size() == partial.available.size());
    for (unsigned fast=0; fast<=2; ++fast)
      for (unsigned slow=0; slow<=4; ++slow)
        for (unsigned general=0; general<=6; ++general) {
          P::Request classes, count;
          classes.name="Classes"; classes.classes=true;
          classes.classCounts={fast,slow}; classes.workers=fast+slow;
          count.name="General"; count.workers=general;
          auto result=P::plan(partial,{classes,count});
          assert(result.managed == bool(fast || slow));
          assert(equal(result,P::plan(partial,{classes,count})));
          assert(result.allocatable == partial.available);
          auto checked=result;
          assert(P::validate(checked,cores) && equal(checked,result));
          P::CPUSet used;
          for (const auto &entry: result.allocations) {
            for (auto cpu: entry.second) {
              assert(partial.available.count(cpu));
              assert(used.insert(cpu).second);
            }
            const auto requested=entry.first=="Classes" ? fast+slow : general;
            assert(result.workerBudgets.at(entry.first)<=requested);
            assert(result.workerBudgets.at(entry.first)<=entry.second.size());
          }
          // Incomplete known sibling maps may force fail-closed publication;
          // empty ownership must also clear budgets, never imply unrestricted work.
          if (result.runtimeFallbackReason==
              "internal CPU allocation overlap; CPU folding suspended")
            assert(result.allocations.empty() && result.workerBudgets.empty());
          ++partialCases;
        }
  }
  assert(partialCases == 420);
  const auto originalTopology=t;
  for (unsigned n=0;n<=20;++n) for (unsigned m=0;m<=20;++m) {
    a.workers=n;b.workers=m;
    const std::vector<P::Request> requests={a,b,g};
    auto result=P::plan(t,requests);
    assert(equal(result,P::plan(t,requests)));
    assert(t.available==originalTopology.available && t.coreThreads==originalTopology.coreThreads &&
      t.performanceLevels==originalTopology.performanceLevels && t.fastPhysicalCores==originalTopology.fastPhysicalCores);
    assert(requests[0].workers==n && requests[1].workers==m && requests[2].reservedCores==1);
    assert(result.managed && result.gpuReservedCPUs==P::CPUSet({0,1}));
    assert(result.gpuDeviceAllocations.at("GPU").at("gpu0")==P::CPUSet({0,1}));
    assert(result.allocatable.size()==14);
    auto checked=result;assert(P::validate(checked,t.coreThreads));assert(equal(checked,result));
    assert(result.workerBudgets.at("A")<=n && result.workerBudgets.at("B")<=m);
  }
  // Zero-demand Classes mode must not invalidate an unrelated GPU reservation.
  {
    auto hybrid=t;hybrid.homogeneous=false;hybrid.effectiveClasses=true;
    hybrid.performanceLevels={{0,1,2,3,4,5,6,7},{8,9,10,11,12,13,14,15}};
    hybrid.fastPhysicalCores={{0,1},{2,3},{4,5},{6,7}};
    P::Request zero;zero.name="Zero";zero.classes=true;zero.classCounts={0,0};
    P::Request gpu;gpu.name="GPU";gpu.gpus={"gpu0"};gpu.reservedCores=1;
    auto result=P::plan(hybrid,{zero,gpu});
    assert(!result.runtimeFallback && result.gpuDeviceAllocations.at("GPU").at("gpu0")==P::CPUSet({0,1}));
    assert(result.allocationSlices.at("Zero").size()==2);
    zero.wantsResources=false;
    assert(!P::plan(hybrid,{zero,gpu}).runtimeFallback);
    zero.wantsResources=true;hybrid.hardAffinity=false;
    auto unavailable=P::plan(hybrid,{zero,gpu});
    assert(unavailable.gpuDeviceAllocations.empty());
  }
  // Shared masks intentionally overlap CPU pools, but exclude exclusive GPUs.
  a.workers=4;b.workers=0;b.gpus={"shared"};
  auto shared=P::plan(t,{a,b,g});
  assert(shared.gpuDeviceAllocations.at("B").at("shared")==shared.allocatable);
  assert(shared.gpuAllocationShortages.empty());
  // Corrupted worker budgets fail closed through either validator entry point.
  for (unsigned fault=0;fault<3;++fault) {
    auto broken=shared;broken.managed=false;
    if (fault==0)broken.workerBudgets["A"]=broken.allocations.at("A").size()+1;
    if (fault==1) {broken.allocations.erase("A");broken.workerBudgets["A"]=1;}
    if (fault==2) {broken.allocations["A"].clear();broken.workerBudgets["A"]=1;}
    auto direct=broken;
    assert(!P::validate(direct,t.coreThreads));
    assert(direct.managed&&direct.runtimeFallback&&direct.allocations.empty()&&direct.workerBudgets.empty());
    assert(!P::validate(broken,t,std::vector<P::Request>{a,b,g}));
    assert(broken.managed&&broken.allocations.empty()&&broken.workerBudgets.empty());
    for(const auto &group:broken.gpuDeviceAllocations)
      for(const auto &device:group.second)assert(device.second.empty());
  }
  {auto boundary=shared;boundary.workerBudgets["A"]=boundary.allocations.at("A").size();
   boundary.allocationSlices.at("A")[0].workers=boundary.workerBudgets.at("A");
   auto requests=std::vector<P::Request>{a,b,g};requests[0].workers=boundary.workerBudgets.at("A");
   boundary.workerBudgets["no-pool"]=0;assert(P::validate(boundary,t,requests));}
  // Corrupted GPU publications must fail closed, while shared overlap is valid.
  const std::vector<P::Request> sharedRequests={a,b,g};
  auto validShared=shared;assert(P::validate(validShared,t,sharedRequests));
  for(unsigned fault=0;fault<7;++fault){
    auto broken=shared;
    if(fault==0)broken.gpuDeviceAllocations["GPU"]["gpu0"]={0}; // Split SMT core.
    if(fault==1)broken.gpuDeviceAllocations["GPU"]["gpu0"]={2,3}; // Wrong union.
    if(fault==2)broken.gpuDeviceAllocations["B"]["shared"]={0,1}; // Shared/exclusive overlap.
    if(fault==3)broken.gpuReservedCPUs.insert(2); // Orphaned reservation.
    if(fault==4)broken.gpuDeviceAllocations["GPU"]["unknown"]={0,1};
    if(fault==5)broken.gpuDeviceAllocations["GPU"]["gpu0"]={99};
    if(fault==6)broken.allocations["A"].push_back(0); // CPU overlaps exclusive GPU.
    assert(!P::validate(broken,t,sharedRequests));
    assert(broken.managed&&broken.runtimeFallback&&broken.allocations.empty()&&broken.workerBudgets.empty());
    for(const auto &group:broken.gpuDeviceAllocations)for(const auto &device:group.second)assert(device.second.empty());
  }
  {auto topology=t;topology.fastPhysicalCores={{0,1}};
   auto broken=shared;broken.gpuReservedCPUs={2,3};broken.gpuDeviceAllocations["GPU"]["gpu0"]={2,3};
   assert(!P::validate(broken,topology,sharedRequests));}
  {auto requests=sharedRequests;requests[1].reservedCores=1;
   auto broken=shared;broken.gpuDeviceAllocations["B"]["shared"]={0,1};
   assert(!P::validate(broken,t,requests));} // Two exclusive devices own the same core.
  // Missing metadata and request bounds must be checked independently of capacity.
  {
    auto topology=t;topology.homogeneous=false;topology.effectiveClasses=true;
    topology.performanceLevels={{0,1,2,3,4,5,6,7},{8,9,10,11,12,13,14,15}};
    P::Request request;request.name="class";request.classes=true;
    request.workers=2;request.classCounts={1,1};
    const std::vector<P::Request> input={request};
    auto original=P::plan(topology,input);
    assert(!original.runtimeFallback);
    for(unsigned fault=0;fault<4;++fault) {
      auto broken=original;
      if(fault==0) broken.allocationSlices.erase("class");
      if(fault==1) {broken.allocationSlices.erase("class");broken.workerBudgets.erase("class");}
      if(fault>=2) {
        ++broken.allocationSlices.at("class")[0].workers;
        ++broken.workerBudgets.at("class");
      }
      auto limits=input;
      if(fault==3) limits[0].workers=3; // Only the per-class bound is exceeded.
      assert(!P::validate(broken,topology,limits));
      assert(broken.runtimeFallbackReason=="internal CPU allocation policy invariant failure; folding suspended");
    }
    P::Request helper;helper.name="helper";helper.gpus={"shared"};
    auto result=P::plan(topology,{helper});
    result.gpuDeviceAllocations.at("helper").at("shared")={8};
    assert(!P::validate(result,topology,{helper}));
    auto subset=P::plan(topology,{helper});
    subset.gpuDeviceAllocations.at("helper").at("shared")={0};
    assert(P::validate(subset,topology,{helper}));
  }
  // Lexical GPU priority is independent of incoming request order under shortage.
  b.gpus={"reserved"};b.reservedCores=7;g.reservedCores=2;
  auto priority=P::plan(t,{g,b});
  auto reversed=P::plan(t,{b,g});
  assert(priority.gpuDeviceAllocations==reversed.gpuDeviceAllocations);
  assert(priority.gpuAllocationShortages==reversed.gpuAllocationShortages);
  assert(priority.gpuDeviceAllocations.at("B").at("reserved").size()==14);
  assert(!priority.gpuDeviceAllocations.count("GPU"));
  assert(!priority.gpuAllocationShortages.at("GPU").at("gpu0").empty());
  b.gpus={"shared"};b.reservedCores=0;g.reservedCores=8;
  auto exhausted=P::plan(t,{g,b});
  assert(exhausted.gpuDeviceAllocations.at("B").at("shared").empty());
  assert(!exhausted.gpuAllocationShortages.at("B").at("shared").empty());
  // A saved class vector can outlive topology. Preserve requests and fall back.
  a.classes=true;a.classCounts={2,2};a.workers=4;
  auto fallback=P::plan(t,{a});
  assert(fallback.managed && fallback.runtimeFallback);
  assert(fallback.runtimeFallbackReason=="performance-class topology unavailable; class CPU folding is waiting");
  assert(fallback.workerBudgets.at("A")==0 && !fallback.allocations.count("A"));
  assert(a.classCounts==std::vector<uint32_t>({2,2}));
  t.hardAffinity=false;
  auto unavailable=P::plan(t,{a,g});
  assert(!unavailable.managed && unavailable.runtimeFallback && unavailable.allocations.empty());
  assert(!unavailable.gpuAllocationShortages.at("GPU").at("gpu0").empty());
  for (const auto &reason: {"strict CPU affinity is unsupported on this platform",
      "available CPU mask is empty or could not be read"}) {
    t.affinityUnavailableReason = reason;
    assert(P::plan(t,{a}).runtimeFallbackReason == reason);
  }
  // General hybrid scheduling still uses the legacy path without reservations.
  t.hardAffinity=true;t.homogeneous=false;t.effectiveClasses=true;
  t.performanceLevels={{0,1,2,3,4,5,6,7},{8,9,10,11,12,13,14,15}};
  a.classes=false;a.classCounts.clear();
  auto legacy=P::plan(t,{a});assert(!legacy.managed && !legacy.runtimeFallback);
  std::cout<<"PASS: pure planner determinism, immutable inputs, 441 budgets, 420 partial-topology cases, GPU priority/shared shortages and policy fallbacks\n";
}
