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

#include "CPUResources.h"
#include "Groups.h"
#include <cbang/os/SystemInfo.h>
#include <cassert>
#include <iostream>
using namespace FAH::Client;
int main() {
  auto &sys=cb::SystemInfo::instance();sys.available={0,1,2,3};
  sys.cores={{0,1},{2,3}};sys.levels={sys.available};
  CPUResources native;Groups groups;groups.set("General",Config::Count(4));native.update(groups);
#ifdef _WIN32
  assert(native.isManaged() && native.supportsGPUReservation());
#else
  assert(!native.isManaged() && !native.supportsGPUAffinity() && !native.supportsGPUReservation());
#endif
  // Distinct validated levels continue to support explicit class isolation.
  sys.levels={{0,1},{2,3}};native.refreshTopology();groups.set("General",Config::Class({2,0}));native.update(groups);
  assert(native.isManaged() && native.getGroupWorkerCount("General")==2);
  for(auto cpu:native.getGroupCPUs("General"))assert(cpu<2);
  // Invalid sibling classification is rejected before class edits are offered.
  // A cpuset hiding the other siblings must not conceal the raw conflict.
  for (const auto &available: {std::set<unsigned>{0,1,2,3},std::set<unsigned>{0,2}}) {
    sys.available=available;sys.levels={{0,2},{1,3}};sys.cores={{0,1},{2,3}};
    native.refreshTopology();
    assert(!native.hasPerformanceClasses()&&!native.hasEffectivePerformanceClasses());
    native.update(groups);assert(native.getGroupWorkerCount("General")==0);
    assert(native.getGroupCPUs("General").empty());
    assert(groups.getGroup("General").getConfig().getCPUClassCounts()==std::vector<uint32_t>({2,0}));
  }
  // Missing sibling data retains logical-only class allocation; consistent
  // sibling maps restore whole-core class configuration and folding.
  sys.available={0,1,2,3};sys.levels={{0,1},{2,3}};sys.cores.clear();
  native.refreshTopology();assert(native.hasPerformanceClasses());native.update(groups);
  assert(native.getGroupWorkerCount("General")==2);
  sys.cores={{0,1},{2,3}};native.refreshTopology();
  assert(native.hasPerformanceClasses()&&native.hasEffectivePerformanceClasses());
  native.update(groups);assert(native.getGroupWorkerCount("General")==2);
  std::cout<<"PASS: cross-class siblings rejected in raw and restricted topology, saved intent and logical-only fallback preserved\n";
  std::cout<<"PASS: native single-class confidence and explicit hybrid class isolation\n";
}
