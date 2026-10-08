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

// Link the shipped serializer and real cbang JSON implementation, without App.
#include <fah/client/CPUStatusJSON.h>
#include <fah/client/GPUProcessPriority.h>
#include <cbang/json/Reader.h>
#include <cbang/json/Value.h>
#include <iostream>
#include <stdexcept>
using namespace FAH::Client;
int main() {
  try {
    CPUStatusSnapshot snapshot;
    auto &topology = snapshot.topology;
    topology.hardAffinity = topology.classSelection = topology.effectiveClasses = true;
    topology.smtTopology = topology.managed = topology.runtimeFallback = topology.gpuReservation = true;
    topology.available = 12; topology.allocatable = 10; topology.physical = 6;
    topology.topologyGeneration = 7; topology.allocationGeneration = 9;
    topology.fallbackReason = "saved GPU reservation cannot fit";
    topology.fastCoreWidths = {2, 2}; topology.levels = {{8, 6, 3}, {4, 4, 4}};
    GroupCPUStatus group;
    group.name = "CPU"; group.mode = "classes";
    group.logical = 6; group.physical = 3; group.workers = 6; group.configured = 8;
    group.hasSMT = group.potentialFullSMT = true; group.classCounts = {8, 0};
    CPUClassAllocationStatus classStatus;
    classStatus.workers = classStatus.logical = classStatus.poolLogical = 6;
    classStatus.physical = classStatus.poolPhysical = 3;
    classStatus.potentialFullSMT = true;
    group.allocationSlices = {classStatus, {}};
    snapshot.groups.push_back(group);
    UnitCPUStatus unit;
    unit.id = "unit-id"; unit.group = "CPU"; unit.mode = "classes";
    unit.number = 42; unit.workers = 6; unit.logical = 6; unit.physical = 3;
    unit.poolPhysical = 3; unit.poolLogical = 6; unit.configured = 8;
    unit.fullSMT = false; unit.classCounts = {8, 0};
    unit.allocationSlices = {classStatus, {}};
    snapshot.units.push_back(unit);
    snapshot.gpuHelpers.push_back({"GPU", "no complete helper core"});
    cb::JSON::Factory factory;
    auto actual = serializeCPUStatus(snapshot, factory);
    auto expected = cb::JSON::Reader(std::cin).parse();
    auto priorities = factory.createList();
    for (const auto &value: GPUProcessPriority::options()) priorities->append(value);
    expected->insert("gpu_priority_options", priorities);
    if (*actual != *expected) {
      std::cerr << "Unexpected CPU status JSON: " << *actual << '\n'; return 1;
    }
    // Group potential must not overwrite a WU's actual non-a8/a9 status.
    if (actual->getDict("group_allocations").getDict("CPU").has("full_smt"))
      throw std::runtime_error("Group published actual full_smt");
    // Count mode omits class details, including for the Default group.
    snapshot.groups[0].name = ""; snapshot.groups[0].mode = "count";
    snapshot.units[0].group = ""; snapshot.units[0].mode = "count";
    snapshot.units[0].id = "unit\"\\id";
    snapshot.gpuHelpers[0] = {"", "reason\"\\text"};
    snapshot.topology.fallbackReason = "fallback\"\\text";
    auto countStatus = serializeCPUStatus(snapshot, factory);
    if (countStatus->getDict("group_allocations").getDict("").has("class_allocations") ||
        countStatus->getDict("unit_allocations").getDict(snapshot.units[0].id).has("class_allocations"))
      throw std::runtime_error("Count mode published class allocations");
    auto roundTrip = cb::JSON::Reader::parse(countStatus->toString());
    if (*roundTrip != *countStatus ||
        roundTrip->getString("runtime_fallback_reason") != snapshot.topology.fallbackReason ||
        roundTrip->getDict("gpu_helper_blocked_groups").getString("") != snapshot.gpuHelpers[0].reason)
      throw std::runtime_error("CPU status JSON round trip changed identifiers or diagnostics");
    snapshot.units.clear(); snapshot.gpuHelpers.clear();
    auto empty = serializeCPUStatus(snapshot, factory);
    if (!empty->getDict("unit_allocations").empty() ||
        !empty->getDict("gpu_helper_blocked_groups").empty())
      throw std::runtime_error("Empty snapshot retained WU/helper entries");
    std::cout << "PASS: real CPU status serializer, WU policy, GPU shortage and empty entries\n";
    return 0;
  } catch (...) {std::cerr << "CPU status serialization test failed\n"; return 1;}
}
