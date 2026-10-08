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
#include "CPUAllocationPlanner.h"
#include "CPUSetUtils.h"
#include "StrictAffinitySupport.h"
#include "Groups.h"
#include "Group.h"
#include "Config.h"

#include <cbang/log/Logger.h>
#include <cbang/os/SystemInfo.h>

#ifdef _WIN32
#include <windows.h>
#endif

#include <algorithm>
#include <sstream>
#include <utility>


using namespace std;
using namespace cb;
using namespace FAH::Client;


namespace {



  using CPUSet = CPUResources::CPUSet;

  struct TopologySnapshot {
    CPUSet available;
    vector<CPUSet> rawLevels, levels, cores, fastCores;
    bool hard = false, homogeneous = false;
    bool validRawLevels = false, validEffectiveLevels = false, validCores = false;
    bool configurable = false, effective = false;
  };


  vector<CPUSet> completeFastCores(const vector<CPUSet> &rawCores,
      const CPUSet &available, const CPUSet &fastest) {
    vector<CPUSet> result;
    for (const auto &core: rawCores) {
      if (core.empty()) continue;
      bool complete = true;
      for (auto cpu: core)
        if (!available.count(cpu) || !fastest.count(cpu)) complete = false;
      if (complete) result.push_back(core);
    }
    return result;
  }


  // Build a coherent local result before changing the live topology. Source
  // reads can span a hardware transition, so validation remains fail-closed.
  TopologySnapshot validateTopology(CPUSet available,
      vector<CPUSet> rawLevels, const vector<CPUSet> &rawCores, bool trustedSingleClassSource) {
    TopologySnapshot result;
    result.available = std::move(available);
    result.rawLevels = std::move(rawLevels);
    result.hard = supportsStrictAffinity() && !result.available.empty();
    // Persistent class indexes follow raw topology. Effective masks preserve
    // those indexes, including empty classes under process/cpuset restrictions.
    CPUSet rawCovered;
    result.validRawLevels = result.rawLevels.size() > 1;

    for (auto &raw: result.rawLevels) {
      if (raw.empty()) result.validRawLevels = false;
      for (auto cpu: raw)
        if (!rawCovered.insert(cpu).second) result.validRawLevels = false;
    }

    // Whole-core class ownership cannot split sibling threads across levels.
    // Raw topology prevents availability restrictions from hiding a conflict.
    for (const auto &core: rawCores) {
      unsigned matches = 0;
      for (const auto &level: result.rawLevels)
        if (!intersectCPUs(core, level).empty()) ++matches;
      if (matches > 1) {
        result.validRawLevels = false;
        break;
      }
    }

    // Effective classes are what this process can use right now.
    CPUSet effectiveCovered;
    result.validEffectiveLevels = result.validRawLevels;

    for (auto &raw: result.rawLevels) {
      CPUSet level = intersectCPUs(raw, result.available);
      result.levels.push_back(level); // Preserve indexes even when currently empty.
      for (auto cpu: level)
        if (!effectiveCovered.insert(cpu).second) result.validEffectiveLevels = false;
    }

    if (effectiveCovered != result.available) result.validEffectiveLevels = false;

    // SMT/core topology is optional. If invalid after intersecting with the
    // current available mask, disable only physical-core allocation, not affinity.
    CPUSet coreCovered;
    result.validCores = true;

    for (const auto &raw: rawCores) {
      CPUSet core = intersectCPUs(raw, result.available);
      if (core.empty()) continue;

      for (auto cpu: core)
        if (!coreCovered.insert(cpu).second) result.validCores = false;
      result.cores.push_back(core);
    }

    if (coreCovered != result.available) result.validCores = false;
    if (!result.validCores) result.cores.clear();

    // Windows EfficiencyClass is structural classification. Linux metric
    // clustering can merge heterogeneous types, so one level is not sufficient.
    result.homogeneous = trustedSingleClassSource && result.hard && !result.cores.empty() &&
      result.rawLevels.size() == 1 && effectiveCovered == result.available;

    result.configurable = result.hard && result.validRawLevels;
    result.effective = result.hard && result.validEffectiveLevels;
    if (result.hard && (result.effective || result.homogeneous) &&
        !result.levels.empty() && result.validCores)
      result.fastCores = completeFastCores(rawCores, result.available, result.levels.front());
    return result;
  }


  template <class C>
  string joinValues(const C &values) {
    ostringstream out;
    out << '[';
    bool first = true;
    for (auto value: values) {
      if (!first) out << ',';
      first = false;
      out << value;
    }
    out << ']';
    return out.str();
  }



}


string CPUResources::formatCPUs(const CPUSet &cpus) {return joinValues(cpus);}
string CPUResources::formatCPUs(const CPUList &cpus) {return joinValues(cpus);}


CPUResources::CPUResources() : CPUResources(
#ifdef _WIN32
  true
#else
  false
#endif
) {}


CPUResources::CPUResources(bool trustedSingleClassSource) :
  trustedSingleClassSource(trustedSingleClassSource) {
  refreshTopology("startup");
#ifdef _WIN32
  // cbang cannot represent affinity spanning Windows processor groups.
  // Diagnose once at startup, rather than repeating on every topology probe.
  if (!hardAffinity && GetActiveProcessorGroupCount() > 1)
    LOG_WARNING("Managed CPU affinity is unavailable on this Windows system: "
      "multiple processor groups are not supported. General CPU folding uses "
      "legacy OS scheduling. Exclusive GPU CPU reservations are unavailable.");
#endif
  allocation.allocatable = available;
  allocation.allocatablePerformanceLevels = performanceLevels;
}


bool CPUResources::refreshTopology(const string &reason) {
  topologyRefreshCount++;
  auto &sys = SystemInfo::instance();

  LOG_DEBUG(1, "CPU topology refresh #" << topologyRefreshCount
    << " requested: reason=" << reason);

  // Read in the same order as before, then validate without mutating members.
  auto sourceAvailable = sys.getAvailableCPUs();
  auto sourceLevels = sys.getCPUPerformanceLevels();
  auto sourceCores = sys.getCPUCoreThreads();
  auto next = validateTopology(std::move(sourceAvailable), std::move(sourceLevels), sourceCores, trustedSingleClassSource);
  bool changed = hardAffinity != next.hard || homogeneous != next.homogeneous ||
    configurableClasses != next.configurable || effectiveClasses != next.effective ||
    available != next.available ||
    rawPerformanceLevels != next.rawLevels ||
    performanceLevels != next.levels ||
    coreThreads != next.cores ||
    fastPhysicalCores != next.fastCores;

  if (changed) {
    LOG_DEBUG(1, "CPU topology previous: generation=" << topologyGeneration
      << " hard-affinity=" << hardAffinity
      << " configurable-classes=" << configurableClasses
      << " effective-classes=" << effectiveClasses
      << " available=" << formatCPUs(available));
  }

  hardAffinity = next.hard;
  homogeneous = next.homogeneous;
  configurableClasses = next.configurable;
  effectiveClasses = next.effective;
  available.swap(next.available);
  rawPerformanceLevels.swap(next.rawLevels);
  performanceLevels.swap(next.levels);
  coreThreads.swap(next.cores);
  fastPhysicalCores.swap(next.fastCores);
  if (changed) topologyGeneration++;

  LOG_DEBUG(1, "CPU topology current: generation=" << topologyGeneration
    << " changed=" << changed
    << " hard-affinity=" << hardAffinity
    << " configurable-classes=" << configurableClasses
    << " effective-classes=" << effectiveClasses
    << " available=" << formatCPUs(available));

  for (unsigned i = 0; i < rawPerformanceLevels.size(); i++) {
    LOG_DEBUG(3, "CPU topology class " << i
      << ": raw=" << formatCPUs(rawPerformanceLevels[i])
      << " raw-count=" << rawPerformanceLevels[i].size()
      << " effective=" << formatCPUs(performanceLevels[i])
      << " effective-count=" << performanceLevels[i].size());
  }

  if (!next.validRawLevels && !rawPerformanceLevels.empty())
    LOG_DEBUG(1, "CPU topology: raw performance-class map is not usable for "
      "class configuration");

  if (!next.validEffectiveLevels && next.validRawLevels)
    LOG_DEBUG(1, "CPU topology: effective CPU set does not currently form a "
      "complete performance-class map");

  // Diagnose discovery gaps here, keeping the allocation planner pure. Avoid
  // repeating the same missing-coverage warning on unchanged periodic refreshes.
  if (changed && !sourceCores.empty()) {
    CPUSet represented;
    for (const auto &core: sourceCores) represented.insert(core.begin(), core.end());
    CPUSet missing;
    set_difference(available.begin(), available.end(), represented.begin(), represented.end(),
      inserter(missing, missing.end()));
    if (!missing.empty())
      LOG_DEBUG(1, "CPU topology: available CPUs absent from the physical-core map="
        << formatCPUs(missing) << "; allocator retains them in logical-CPU fallback ordering");
  }

  if (coreThreads.empty())
    LOG_DEBUG(1, "CPU topology: SMT/core sibling map unavailable; allocator "
      "will use deterministic logical-CPU ordering");
  else
    for (unsigned i = 0; i < coreThreads.size(); i++)
      LOG_DEBUG(3, "CPU topology physical core " << i
        << ": threads=" << formatCPUs(coreThreads[i]));

  if (changed)
    LOG_INFO(3, "CPU topology/capability changed (generation "
      << topologyGeneration << "); saved CPU-class configuration is unchanged");
  else
    LOG_DEBUG(2, "CPU topology refresh #" << topologyRefreshCount
      << " completed with no change");

  return changed;
}


void CPUResources::update(const Groups &groups, const string &reason) {
  CPUAllocationPlanner::Topology topology;
  topology.hardAffinity = hardAffinity;
  if (!supportsStrictAffinity())
    topology.affinityUnavailableReason = "strict CPU affinity is unsupported on this platform";
  else if (available.empty())
    topology.affinityUnavailableReason = "available CPU mask is empty or could not be read";
  topology.homogeneous = homogeneous;
  topology.effectiveClasses = effectiveClasses;
  topology.available = available;
  topology.performanceLevels = performanceLevels;
  topology.coreThreads = coreThreads;
  topology.fastPhysicalCores = fastPhysicalCores;
  vector<CPUAllocationPlanner::Request> requests;
  for (const auto &name: groups.keys()) {
    const auto &group = groups.getGroup(name);
    const auto &config = group.getConfig();
    CPUAllocationPlanner::Request request;
    request.wantsResources = group.wantsResources();
    request.name = name;
    request.workers = config.getConfiguredCPUTotal();
    request.classes = config.usesCPUClasses();
    request.classCounts = config.getCPUClassCounts();
    request.reservedCores = config.getGPUReservedCores();
    for (const auto &gpu: config.getGPUs()) request.gpus.insert(gpu);
    requests.push_back(std::move(request));
  }
  // Ignore unrelated config edits and group insertion order. Only changed allocation
  // results advance generations, not every settings notification.
  sort(requests.begin(), requests.end(), [](const auto &a, const auto &b) {return a.name < b.name;});
  if (publishedTopologyGeneration == topologyGeneration && requests == publishedRequests) return;
  auto result = CPUAllocationPlanner::plan(topology, requests);
  const bool changed = !(allocation == result);
  // Result's container swaps do not allocate or throw. No observer runs
  // inside this event-loop publication, so all allocation fields and their
  // generation become visible together. Discovery/process ownership stay separate.
  static_assert(noexcept(allocation.swap(result)), "allocation publication must not throw");
  if (changed) {
    allocation.swap(result);
    ++allocationGeneration;
  }
  // Cache changed inputs even when they yield identical published allocations.
  publishedRequests.swap(requests);
  publishedTopologyGeneration = topologyGeneration;
  // Search diagnostics belong to this replan, even when placement is unchanged.
  const auto &reports = changed ? allocation.rebalanceReports : result.rebalanceReports;
  for (const auto &report: reports)
    LOG_DEBUG(2, "CPU pool rebalance: " << report.outcomeName()
      << "; assigned-cores=" << report.cores << " groups=" << report.groups
      << " visited-states=" << report.visits << " improved=" << report.improved);
  if (!changed) return;
  // result now holds the previous publication for change-only diagnostics.
  for (const auto &group: allocation.gpuAllocationShortages)
    for (const auto &device: group.second) {
      auto previous = result.gpuAllocationShortages.find(group.first);
      if (previous == result.gpuAllocationShortages.end() ||
          !previous->second.count(device.first) || previous->second.at(device.first) != device.second)
        LOG_WARNING("GPU helper allocation RG '" << (group.first.empty() ? "Default" : group.first)
          << "' GPU '" << device.first << "': " << device.second);
    }
  if (allocation.runtimeFallback &&
      (!result.runtimeFallback || result.runtimeFallbackReason != allocation.runtimeFallbackReason))
    LOG_WARNING("CPU allocation fallback: " << allocation.runtimeFallbackReason
      << "; saved configuration is unchanged");
  if (allocation.runtimeFallbackReason == "internal CPU allocation overlap; CPU folding suspended" ||
      allocation.runtimeFallbackReason == "internal CPU worker budget exceeds owned capacity; CPU folding suspended" ||
      allocation.runtimeFallbackReason == "internal CPU allocation policy invariant failure; folding suspended" ||
      allocation.runtimeFallbackReason == "internal GPU allocation invariant failure; folding suspended")
    LOG_ERROR(allocation.runtimeFallbackReason);
  // These are desired pools. Unit retains old process ownership until exit.
  logAllocationChanges(groups, publishedRequests);
  LOG_DEBUG(2, "CPU allocation generation " << allocationGeneration << " reason=" << reason);
}

void CPUResources::logAllocationChanges(const Groups &groups,
    const vector<CPUAllocationPlanner::Request> &requests) {
  map<string, string> summaries;
  for (const auto &request: requests) {
    const auto &name = request.name;
    ostringstream out;
    out << "managed=" << allocation.managed << " fallback=" << allocation.runtimeFallback
        << " workers=" << getGroupWorkerCount(name)
        << " physical-pool=" << formatCPUs(getGroupCPUs(name))
        << " policy={" << groups.getGroup(name).getConfig().getCPUConfigDescription() << '}';
    const auto summary = out.str();
    summaries.emplace(name, summary);
    const auto previous = loggedAllocationSummaries.find(name);
    if (previous == loggedAllocationSummaries.end() || previous->second != summary)
      LOG_INFO(3, "CPU allocation RG '" << (name.empty() ? "Default" : name) << "': " << summary);
  }
  loggedAllocationSummaries.swap(summaries);
}


unsigned CPUResources::physicalCoreCount(const CPUSet &mask) const {
  unsigned count = 0;
  CPUSet covered;
  for (const auto &core: coreThreads) {
    bool used = false;
    for (auto logical: core)
      if (mask.count(logical)) {
        covered.insert(logical);
        used = true;
      }
    if (used) ++count;
  }
  return covered == mask ? count : 0;
}


const CPUResources::CPUList &CPUResources::getGroupCPUs(const string &name) const {
  static const CPUList empty;
  auto it = allocation.allocations.find(name);
  return it == allocation.allocations.end() ? empty : it->second;
}
const CPUAllocationSlices &CPUResources::getGroupAllocationSlices(const string &name) const {
  static const CPUAllocationSlices empty;
  auto it = allocation.allocationSlices.find(name);
  return it == allocation.allocationSlices.end() ? empty : it->second;
}
unsigned CPUResources::getGroupWorkerCount(const string &name) const {
  auto it = allocation.workerBudgets.find(name);
  return it == allocation.workerBudgets.end() ? 0 : it->second;
}
const CPUResources::CPUSet &CPUResources::getGPUCPUs(const string &name, const string &gpu) const {
  static const CPUSet empty;
  auto group = allocation.gpuDeviceAllocations.find(name);
  if (group == allocation.gpuDeviceAllocations.end()) return empty;
  auto device = group->second.find(gpu);
  return device == group->second.end() ? empty : device->second;
}

const string &CPUResources::getGPUAllocationShortage(
    const string &name, const string &gpu) const {
  static const string empty;
  auto group = allocation.gpuAllocationShortages.find(name);
  if (group == allocation.gpuAllocationShortages.end()) return empty;
  auto device = group->second.find(gpu);
  return device == group->second.end() ? empty : device->second;
}
