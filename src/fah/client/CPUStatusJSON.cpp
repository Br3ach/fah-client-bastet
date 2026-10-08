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

#include "CPUStatusJSON.h"
#include "GPUProcessPriority.h"
#include <cbang/json/Value.h>
using namespace FAH::Client;

namespace {
cb::JSON::ValuePtr serializeClasses(const std::vector<CPUClassAllocationStatus> &classes,
    const cb::JSON::Factory &factory, bool potential) {
  auto output = factory.createList();
  for (const auto &level: classes) {
    auto item = factory.createDict();
    item->insert("allocated_workers", level.workers);
    item->insert("logical_cpus", level.logical);
    item->insert("physical_cpus", level.physical);
    item->insert("pool_logical_cpus", level.poolLogical);
    item->insert("pool_physical_cpus", level.poolPhysical);
    item->insertBoolean(potential ? "potential_full_smt" : "full_smt",
      potential ? level.potentialFullSMT : level.fullSMT);
    output->append(item);
  }
  return output;
}
}

cb::JSON::ValuePtr FAH::Client::serializeCPUStatus(const CPUStatusSnapshot &snapshot,
    const cb::JSON::Factory &factory) {
  auto output = factory.createDict();
  const auto &topology = snapshot.topology;
  auto priorities = factory.createList();
  for (const auto &value: GPUProcessPriority::options()) priorities->append(value);
  output->insert("capability", topology.hardAffinity ? "hard" : "none");
  output->insert("available", topology.available);
  output->insert("topology_generation", topology.topologyGeneration);
  output->insert("allocation_generation", topology.allocationGeneration);
  output->insertBoolean("class_selection", topology.classSelection);
  output->insertBoolean("effective_classes", topology.effectiveClasses);
  output->insertBoolean("smt_topology", topology.smtTopology);
  output->insertBoolean("managed", topology.managed);
  output->insertBoolean("runtime_fallback", topology.runtimeFallback);
  output->insert("runtime_fallback_reason", topology.fallbackReason);
  output->insertBoolean("gpu_cpu_reservation", topology.gpuReservation);
  output->insert("allocatable", topology.allocatable);
  output->insert("physical_cpus", topology.physical);
  auto fast = factory.createList();
  for (auto width: topology.fastCoreWidths) fast->append(width);
  output->insert("performance1_core_threads", fast);
  auto levels = factory.createList();
  for (const auto &level: topology.levels) {
    auto item = factory.createDict();
    item->insert("logical_cpus", level.logical);
    item->insert("available_logical_cpus", level.availableLogical);
    item->insert("physical_cpus", level.physical);
    levels->append(item);
  }
  output->insert("performance_levels", levels);
  auto groups = factory.createDict();
  for (const auto &group: snapshot.groups) {
    auto item = factory.createDict();
    item->insert("logical_cpus", group.logical);
    item->insert("physical_cpus", group.physical);
    item->insert("pool_logical_cpus", group.logical);
    item->insert("pool_physical_cpus", group.physical);
    item->insert("allocated_workers", group.workers);
    item->insertBoolean("has_smt", group.hasSMT);
    item->insertBoolean("potential_full_smt", group.potentialFullSMT);
    item->insert("configured_cpus", group.configured);
    item->insert("cpu_mode", group.mode);
    auto counts = factory.createList();
    for (auto count: group.classCounts) counts->append(count);
    item->insert("cpu_class_counts", counts);
    if (group.mode == "classes")
      item->insert("class_allocations", serializeClasses(group.allocationSlices, factory, true));
    groups->insert(group.name, item);
  }
  output->insert("group_allocations", groups);
  auto units = factory.createDict();
  for (const auto &unit: snapshot.units) {
    auto item = factory.createDict();
    item->insert("group", unit.group);
    item->insert("number", unit.number);
    item->insert("allocated_workers", unit.workers);
    item->insert("logical_cpus", unit.logical);
    item->insert("physical_cpus", unit.physical);
    item->insert("pool_physical_cpus", unit.poolPhysical);
    item->insert("pool_logical_cpus", unit.poolLogical);
    item->insertBoolean("full_smt", unit.fullSMT);
    item->insert("configured_cpus", unit.configured);
    item->insert("cpu_mode", unit.mode);
    auto counts = factory.createList();
    for (auto count: unit.classCounts) counts->append(count);
    item->insert("cpu_class_counts", counts);
    if (unit.mode == "classes")
      item->insert("class_allocations", serializeClasses(unit.allocationSlices, factory, false));
    units->insert(unit.id, item);
  }
  output->insert("unit_allocations", units);
  auto helpers = factory.createDict();
  for (const auto &helper: snapshot.gpuHelpers) helpers->insert(helper.group, helper.reason);
  output->insert("gpu_helper_blocked_groups", helpers);
  output->insert("gpu_priority_options", priorities);
  return output;
}
