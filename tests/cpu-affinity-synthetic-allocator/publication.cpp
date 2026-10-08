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
#include "FixtureCPUResources.h"
#include "Groups.h"
#include <cbang/os/SystemInfo.h>
#include <cassert>
#include <iostream>
#include <utility>
#include <cbang/log/Logger.h>
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

struct Published : CPUResources {
  P::Result snapshot() const {return allocation;}
  void nextTopologyGeneration() {++topologyGeneration;}
};
int main() {
  static_assert(noexcept(std::declval<P::Result &>().swap(std::declval<P::Result &>())),
    "publication must not throw");
  auto &sys=cb::SystemInfo::instance();
  sys.available={0,1,2,3,4,5,6,7};sys.levels={sys.available};
  sys.cores={{0,1},{2,3},{4,5},{6,7}};
  Groups groups;groups.set("A",Config::Count(4));
  auto gpu=Config::Count(0);gpu.gpus={"gpu"};gpu.reserved=1;groups.set("GPU",gpu);
  Published resources;resources.update(groups);
  const auto before=resources.snapshot();const auto generation=resources.getAllocationGeneration();
  resources.update(groups,"unrelated settings");
  assert(resources.getAllocationGeneration()==generation && equal(before,resources.snapshot()));
  assert(before.managed && before.gpuReservedCPUs==P::CPUSet({0,1}));
  groups.set("A",Config::Count(2));gpu.reserved=2;gpu.failRead=true;groups.set("GPU",gpu);
  bool rejected=false;
  try {resources.update(groups);} catch(const std::runtime_error &) {rejected=true;}
  assert(rejected && resources.getAllocationGeneration()==generation);
  assert(equal(before,resources.snapshot()));
  gpu.failRead=false;groups.set("GPU",gpu);resources.update(groups);
  const auto after=resources.snapshot();
  assert(resources.getAllocationGeneration()==generation+1);
  assert(after.gpuReservedCPUs==P::CPUSet({0,1,2,3}));
  assert(after.allocatable==P::CPUSet({4,5,6,7}));
  assert(after.workerBudgets.at("A")==2 && after.gpuDeviceAllocations.at("GPU").at("gpu")==after.gpuReservedCPUs);
  groups.getGroup("A").demand=false;resources.update(groups);
  const auto inactiveGeneration=resources.getAllocationGeneration();
  groups.getGroup("A").getConfig()=Config::Count(7);resources.update(groups);
  assert(resources.getAllocationGeneration()==inactiveGeneration);
  resources.update(groups);assert(resources.getAllocationGeneration()==inactiveGeneration);
  // A new probe can require another bounded search without changing placement.
  sys.available.clear(); sys.cores.clear();
  for (unsigned i=0;i<130;++i) {
    sys.available.insert(2*i);sys.available.insert(2*i+1);
    sys.cores.push_back({2*i,2*i+1});
  }
  sys.levels={sys.available};
  Groups limited;limited.set("A",Config::Count(129));
  limited.set("B",Config::Count(130));limited.set("C",Config::Count(1));
  Published bounded;bounded.update(limited);
  const auto boundedBefore=bounded.snapshot();
  assert(!boundedBefore.rebalanceReports.empty());
  assert(boundedBefore.rebalanceReports.front().outcome==
    CPUWholeCorePacking::RebalanceReport::Outcome::CoreLimit);
  const auto boundedGeneration=bounded.getAllocationGeneration();
  std::ostringstream diagnostics;auto output=std::cout.rdbuf(diagnostics.rdbuf());
  testDebugLevel=2;bounded.nextTopologyGeneration();bounded.update(limited);
  assert(bounded.getAllocationGeneration()==boundedGeneration);
  assert(equal(boundedBefore,bounded.snapshot()));
  assert(diagnostics.str().find("CPU pool rebalance: core limit skipped")!=std::string::npos);
  diagnostics.str("");diagnostics.clear();bounded.update(limited);
  assert(diagnostics.str().find("CPU pool rebalance:")==std::string::npos);
  testDebugLevel=1;std::cout.rdbuf(output);
  // A diagnostic exception cannot cache new inputs against an old allocation.
  limited.set("C",Config::Count(2));
  Published expectedResources;expectedResources.update(limited);
  const auto expectedPublication=expectedResources.snapshot();
  assert(!(expectedPublication==boundedBefore) && !expectedPublication.rebalanceReports.empty());
  testDebugLevel=2;testFailRebalanceLog=true;
  rejected=false;
  try {bounded.update(limited);} catch(const std::runtime_error &error) {
    rejected=std::string(error.what())=="Injected rebalance logging failure";
  }
  testFailRebalanceLog=false;testDebugLevel=1;
  assert(rejected && bounded.getAllocationGeneration()==boundedGeneration+1);
  assert(equal(expectedPublication,bounded.snapshot()));
  bounded.update(limited);
  assert(bounded.getAllocationGeneration()==boundedGeneration+1);
  assert(equal(expectedPublication,bounded.snapshot()));
  // The same non-throwing swap moves every field, including shortage/fallback.
  auto first=before,second=after;second.runtimeFallback=true;
  second.runtimeFallbackReason="test shortage";second.gpuAllocationShortages["GPU"]["gpu"]="waiting";
  CPUWholeCorePacking::RebalanceReport report;
  report.outcome=CPUWholeCorePacking::RebalanceReport::Outcome::StateLimit;
  report.cores=18;report.groups=12;report.visits=50001;
  second.rebalanceReports.push_back(report);
  const auto expected=second;first.swap(second);
  assert(equal(first,expected) && equal(second,before));
  std::cout<<"PASS: complete publication swap, failed staging retains state/generation, successful retry advances once\n";
}
