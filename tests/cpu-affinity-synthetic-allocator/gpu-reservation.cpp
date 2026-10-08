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

#include "FixtureCPUResources.h"
#include "Groups.h"
#include <cbang/os/SystemInfo.h>
#include <cassert>
#include <iostream>
#include <sstream>
using namespace FAH::Client;
using namespace std;
static Config gpu(unsigned n) {auto c=Config::Count(0);c.gpus={"gpu"};c.reserved=n;return c;}
static void disjoint(const CPUResources &r,const Groups &groups) {
 for(const auto &name:groups.keys())
  for(auto cpu:r.getGroupCPUs(name)) assert(!r.getGPUReservedCPUs().count(cpu));
}
int main() {
 auto &si=cb::SystemInfo::instance();
 si.available={0,1,2,3,4,5,6,7,8,9};
 si.levels={{0,1,2,3,4,5,6,7},{8,9}};
 si.cores={{0,4},{1,5},{2,6},{3,7},{8},{9}};
 // Production keys preserve insertion order, but shortages use lexical priority.
 {
  Groups reverse;reverse.set("C",gpu(2));reverse.set("A",gpu(2));
  assert(reverse.keys()==vector<string>({"C","A"}));
  CPUResources shrink;shrink.update(reverse);
  assert(shrink.getGPUCPUs("A","gpu").size()==4);
  assert(shrink.getGPUCPUs("C","gpu").size()==4);
  si.available={0,1,2,4,5,6,8,9};
  shrink.refreshTopology();shrink.update(reverse);
  assert(shrink.getGPUCPUs("A","gpu")==set<unsigned>({0,1,4,5}));
  assert(shrink.getGPUCPUs("C","gpu").empty());
  assert(!shrink.getGPUAllocationShortage("C","gpu").empty());
  Groups forward;forward.set("A",gpu(2));forward.set("C",gpu(2));
  shrink.update(forward);
  assert(shrink.getGPUCPUs("A","gpu")==set<unsigned>({0,1,4,5}));
  assert(shrink.getGPUCPUs("C","gpu").empty());
  si.available={0,1,2,3,4,5,6,7,8,9};
 }
 Groups groups;groups.set("A",gpu(1));groups.set("B",Config::Count(10));groups.set("C",gpu(0));
 CPUResources r;r.update(groups);
 assert(r.supportsGPUAffinity());assert(r.getFastPhysicalCores().size()==4);
 assert(r.getGPUCPUs("A","gpu")==set<unsigned>({0,4}));
 assert(r.getGPUReservedCPUs()==set<unsigned>({0,4}));
 assert(r.isManaged());assert(r.getAllocatableCPUs().size()==8);
 assert(r.getGroupCPUs("B").size()==8);disjoint(r,groups);
 assert(r.getGPUCPUs("C","gpu")==set<unsigned>({1,2,3,5,6,7}));
 auto stable=r.getGPUCPUs("A","gpu");
 assert(!r.refreshTopology("unchanged-poll"));
 assert(r.getAllocatableCPUs().size()==8);
 // An unchanged topology preserves the published shared helper pool.
 assert(r.getGPUCPUs("C","gpu")==set<unsigned>({1,2,3,5,6,7}));
 r.update(groups);assert(r.getGPUCPUs("A","gpu")==stable);
 groups.set("C",gpu(2));r.update(groups);
 assert(r.getGPUCPUs("C","gpu")==set<unsigned>({1,2,5,6}));
 assert(r.getAllocatableCPUs()==set<unsigned>({3,7,8,9}));disjoint(r,groups);
 // Reserving all fast cores is permitted when no CPU work needs them.
 groups.getGroup("A").getConfig().reserved=2;r.update(groups);
 assert(r.getAllocatableCPUs()==set<unsigned>({8,9}));disjoint(r,groups);
 // Zero can share only the remaining unreserved Performance 1 threads.
 groups.getGroup("A").getConfig().reserved=0;r.update(groups);
 assert(r.getGPUCPUs("A","gpu")==set<unsigned>({2,3,6,7}));
 // No selected GPU means no active reservation, regardless of saved value.
 groups.getGroup("C").getConfig().gpus.clear();r.update(groups);
 assert(r.getGPUReservedCPUs().empty());assert(r.getAllocatableCPUs()==si.available);
 // A request too large after topology loss stays unallocated, never shared.
 groups.getGroup("A").getConfig().reserved=5;r.update(groups);
 assert(r.getGPUCPUs("A","gpu").empty());assert(r.getGPUReservedCPUs().empty());
 assert(r.getGPUAllocationShortage("A","gpu").find("Requested 5")!=string::npos);
 assert(r.getGPUAllocationShortage("A","gpu").find("only 4")!=string::npos);
 auto shortage=r.getGPUAllocationShortage("A","gpu");
 ostringstream repeated;auto output=cout.rdbuf(repeated.rdbuf());r.update(groups);cout.rdbuf(output);
 assert(repeated.str().find("GPU helper allocation")==string::npos);
 assert(r.getGPUAllocationShortage("A","gpu")==shortage);
 groups.getGroup("A").getConfig().reserved=1;r.update(groups);
 assert(r.getGPUAllocationShortage("A","gpu").empty());
 groups.set("SHARED1",gpu(0));groups.set("SHARED2",gpu(0));r.update(groups);
 assert(!r.getGPUCPUs("SHARED1","gpu").empty());
 assert(r.getGPUCPUs("SHARED1","gpu")==r.getGPUCPUs("SHARED2","gpu"));
 for(auto cpu:r.getGPUCPUs("SHARED1","gpu"))assert(!r.getGPUReservedCPUs().count(cpu));
 groups.getGroup("A").getConfig().reserved=4;r.update(groups);
 assert(!r.getGPUAllocationShortage("SHARED1","gpu").empty());
 groups.getGroup("A").getConfig().reserved=0;
 groups.getGroup("SHARED1").getConfig().gpus.clear();
 groups.getGroup("SHARED2").getConfig().gpus.clear();r.update(groups);
 assert(r.getGPUAllocationShortage("SHARED1","gpu").empty());
 groups.getGroup("A").getConfig().reserved=5;r.update(groups);
 // A partially available SMT core cannot be reserved as a complete core.
 si.available.erase(4);r.refreshTopology();assert(r.getFastPhysicalCores().size()==3);
 groups.getGroup("A").getConfig().reserved=1;r.update(groups);
 assert(r.getGPUCPUs("A","gpu")==set<unsigned>({1,5}));disjoint(r,groups);
 // Explicit class budgets cannot acquire reserved threads, including the same RG.
 groups.getGroup("A").getConfig().setClass({2,0});r.update(groups);disjoint(r,groups);
 // Capability loss leaves exclusive GPU allocation empty.
 si.available.clear();r.refreshTopology();r.update(groups);
 assert(!r.supportsGPUAffinity());assert(r.getGPUCPUs("A","gpu").empty());
 assert(r.getGPUAllocationShortage("A","gpu").find("unavailable")!=string::npos);
 // Homogeneous CPUs also support Performance 1 GPU affinity.
 si.available={0,1,2,3};si.levels={si.available};si.cores={{0,2},{1,3}};
 r.refreshTopology();groups=Groups();groups.set("A",gpu(1));groups.set("B",gpu(0));r.update(groups);
 assert(r.supportsGPUAffinity());assert(r.getGPUCPUs("A","gpu")==set<unsigned>({0,2}));
 assert(r.getGPUCPUs("B","gpu")==set<unsigned>({1,3}));
 // Every enabled GPU in one RG receives its own complete physical core.
 si.available={0,1,2,3,4,5,6,7};si.levels={si.available};si.cores={{0,1},{2,3},{4,5},{6,7}};
 r.refreshTopology();groups=Groups();auto multi=gpu(1);multi.gpus={"gpu1","gpu2"};
 groups.set("A",multi);groups.set("B",Config::Count(4));r.update(groups);
 assert(r.getGPUCPUs("A","gpu1")==set<unsigned>({0,1}));
 assert(r.getGPUCPUs("A","gpu2")==set<unsigned>({2,3}));
 assert(r.getGPUReservedCPUs().size()==4);disjoint(r,groups);
 groups.getGroup("A").getConfig().reserved=2;r.update(groups);
 assert(r.getGPUCPUs("A","gpu1").size()==4 && r.getGPUCPUs("A","gpu2").size()==4);
 assert(r.getGroupWorkerCount("B")==0);
 // A topology shortage is per device: a successful earlier reservation is kept.
 groups.getGroup("A").getConfig().reserved=3;r.update(groups);
 assert(r.getGPUCPUs("A","gpu1").size()==6);
 assert(r.getGPUAllocationShortage("A","gpu1").empty());
 assert(r.getGPUCPUs("A","gpu2").empty());
 assert(r.getGPUAllocationShortage("A","gpu2").find("only 1")!=string::npos);
 disjoint(r,groups);
 // Invalid sibling topology cannot be used for a complete-core reservation.
 si.cores={{0,2},{2,3}};r.refreshTopology();r.update(groups);
 assert(r.getFastPhysicalCores().empty());assert(r.getGPUCPUs("A","gpu").empty());
 cout<<"PASS: per-RG whole-core GPU priority, shared masks, CPU exclusion, all-core reservation, shortages, cpuset fragments, capability loss and homogeneous topology\n";
}
