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

#include "CPUStatusBuilder.h"
#include "CPUResources.h"
#include "Groups.h"
#include "Group.h"
#include "Config.h"
#include "Units.h"
#include "Unit.h"
#include <utility>
using namespace std;
using namespace FAH::Client;

CPUStatusSnapshot CPUStatusBuilder::build(const CPUResources &cpu,
    const Groups *groups, const Units *units) {
  CPUStatusSnapshot snapshot;
  auto &topology = snapshot.topology;
  topology.hardAffinity = cpu.hasHardAffinity();
  topology.available = cpu.getAvailableCPUs().size();
  topology.topologyGeneration = cpu.getTopologyGeneration();
  topology.allocationGeneration = cpu.getAllocationGeneration();
  topology.classSelection = cpu.hasPerformanceClasses();
  topology.effectiveClasses = cpu.hasEffectivePerformanceClasses();
  topology.smtTopology = !cpu.getCoreThreads().empty();
  topology.managed = cpu.isManaged();
  topology.runtimeFallback = cpu.isRuntimeFallback();
  topology.fallbackReason = cpu.getRuntimeFallbackReason();
  topology.gpuReservation = cpu.supportsGPUReservation();
  topology.allocatable = cpu.getAllocatableCPUs().size();
  topology.physical = cpu.physicalCoreCount(cpu.getAvailableCPUs());
  for (const auto &core: cpu.getFastPhysicalCores()) topology.fastCoreWidths.push_back(core.size());
  const auto &raw = cpu.getRawPerformanceLevels();
  const auto &effective = cpu.getPerformanceLevels();
  for (unsigned i = 0; i < raw.size(); ++i) {
    CPUClassStatus level;
    level.logical = raw[i].size();
    if (i < effective.size()) {
      level.availableLogical = effective[i].size();
      level.physical = cpu.physicalCoreCount(effective[i]);
    }
    topology.levels.push_back(level);
  }
  if (groups) for (const auto &name: groups->keys()) {
    const auto &group = groups->getGroup(name);
    const auto &config = group.getConfig();
    if (cpu.isManaged()) {
      const bool classes = config.usesCPUClasses();
      const auto &order = cpu.getGroupCPUs(name);
      CPUResources::CPUSet mask(order.begin(), order.end());
      GroupCPUStatus status;
      status.name = name;
      status.logical = mask.size();
      // Zero may mean incomplete topology; do not infer SMT from that count.
      status.physical = cpu.physicalCoreCount(mask);
      status.workers = cpu.getGroupWorkerCount(name);
      // Group capacity predicts SMT; only a WU confirms its core-specific policy.
      status.hasSMT = status.physical && mask.size() > status.physical;
      status.potentialFullSMT = status.hasSMT && status.workers == mask.size();
      status.configured = config.getConfiguredCPUTotal();
      status.mode = classes ? "classes" : "count";
      status.classCounts = config.getCPUClassCounts();
      if (classes) {
        status.potentialFullSMT = false;
        for (const auto &slice: cpu.getGroupAllocationSlices(name)) {
          CPUClassAllocationStatus level;
          level.workers = slice.workers;
          level.logical = level.poolLogical = slice.pool.size();
          level.physical = level.poolPhysical = cpu.physicalCoreCount(slice.pool);
          level.potentialFullSMT = level.physical && level.logical > level.physical &&
            level.workers == level.logical;
          status.potentialFullSMT |= level.potentialFullSMT;
          status.allocationSlices.push_back(level);
        }
      }
      snapshot.groups.push_back(std::move(status));
    }
    if (!group.wantsResources()) continue;
    const auto devices = config.getGPUs();
    bool blocked = false;
    string reasons;
    for (const auto &device: devices) {
      blocked |= cpu.getGPUCPUs(name, device).empty();
      const auto &reason = cpu.getGPUAllocationShortage(name, device);
      if (!reason.empty()) {
        if (!reasons.empty()) reasons += " ";
        reasons += "GPU '" + device + "': " + reason;
      }
    }
    if (!devices.empty() && (cpu.supportsGPUAffinity() || config.getGPUReservedCores()) && blocked) {
      if (reasons.empty()) reasons = config.getGPUReservedCores() ?
        "The reserved Performance 1 cores are currently unavailable. GPU folding is waiting for its helper CPU allocation." :
        "No unreserved Performance 1 CPUs are available for GPU helpers. Reduce another group's GPU reservation or reserve cores for this group.";
      snapshot.gpuHelpers.push_back({name, std::move(reasons)});
    }
  }
  // Reuse the WU plan and its generation/lifecycle guards; never guess a core type.
  if (units) for (unsigned i = 0; i < units->size(); ++i) {
    const auto unit = units->getUnit(i);
    auto status = unit->getDesiredCPUStatus();
    if (status) {
      status->id = unit->getID();
      snapshot.units.push_back(std::move(*status));
    }
  }
  return snapshot;
}
