/******************************************************************************\

                  This file is part of the Folding@home Client.

          The fah-client runs Folding@home protein folding simulations.
                    Copyright (c) 2001-2026, foldingathome.org
                               All rights reserved.

\******************************************************************************/

#include "CPUResources.h"
#include "CPUExecutionPlan.h"
#include "Groups.h"
#include "Group.h"
#include "Config.h"

#include <cbang/log/Logger.h>
#include <cbang/os/SystemInfo.h>

#include <algorithm>
#include <map>
#include <functional>
#include <sstream>
#include <utility>


using namespace std;
using namespace cb;
using namespace FAH::Client;


namespace {
  CPUResources::CPUSet intersect(const CPUResources::CPUSet &a,
                                 const CPUResources::CPUSet &b) {
    CPUResources::CPUSet result;
    set_intersection(a.begin(), a.end(), b.begin(), b.end(),
                     inserter(result, result.end()));
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
string CPUResources::formatCounts(const vector<uint32_t> &counts) {
  return joinValues(counts);
}


CPUResources::CPUResources() {
  refreshTopology("startup");
  allocatable = available;
  allocatablePerformanceLevels = performanceLevels;
}


void CPUResources::configureGPUAllocations(const Groups &groups) {
  const auto previousShortages = gpuAllocationShortages;
  gpuAllocationShortages.clear();
  auto shortage = [&](const string &name, const string &gpu, const string &reason) {
    gpuAllocationShortages[name][gpu] = reason;
    auto group = previousShortages.find(name);
    if (group == previousShortages.end() || !group->second.count(gpu) ||
        group->second.at(gpu) != reason)
      LOG_WARNING("GPU helper allocation RG '" << (name.empty() ? "Default" : name)
        << "' GPU '" << gpu << "': " << reason);
  };
  gpuDeviceAllocations.clear();
  gpuReservedCPUs.clear();
  allocatable = available;
  allocatablePerformanceLevels = performanceLevels;
  if (!supportsGPUAffinity()) {
    for (const auto &name: groups.keys()) {
      const auto &config = groups.getGroup(name).getConfig();
      if (!config.getGPUReservedCores()) continue;
      for (const auto &gpu: config.getGPUs())
        shortage(name, gpu, "Requested " + to_string(config.getGPUReservedCores()) +
          " exclusive Performance 1 physical cores, but hard affinity or usable topology is unavailable. GPU folding is waiting.");
    }
    return;
  }

  // GPU reservations precede all CPU allocations, including their own RG.
  for (auto &name: groups.keys()) {
    const auto &config = groups.getGroup(name).getConfig();
    if (config.getGPUs().empty() || !config.getGPUReservedCores()) continue;
    for (const auto &gpu: config.getGPUs()) {
      CPUSet mask;
      unsigned count = 0;
      for (const auto &core: fastPhysicalCores) {
        bool free = true;
        for (auto cpu: core) if (gpuReservedCPUs.count(cpu)) free = false;
        if (!free) continue;
        mask.insert(core.begin(), core.end());
        if (++count == config.getGPUReservedCores()) break;
      }
      // A shortage never silently converts exclusive GPU work to shared.
      if (count != config.getGPUReservedCores()) {
        shortage(name, gpu, "Requested " + to_string(config.getGPUReservedCores()) +
          " exclusive Performance 1 physical cores, but only " + to_string(count) +
          " remain available after earlier GPU reservations. GPU folding is waiting.");
        continue;
      }
      gpuDeviceAllocations[name][gpu] = mask;
      gpuReservedCPUs.insert(mask.begin(), mask.end());
    }
  }
  for (auto cpu: gpuReservedCPUs) {
    allocatable.erase(cpu);
    for (auto &level: allocatablePerformanceLevels) level.erase(cpu);
  }
  for (auto &name: groups.keys()) {
    const auto &config = groups.getGroup(name).getConfig();
    if (!config.getGPUs().empty() && !config.getGPUReservedCores()) {
      // Shared helper masks may overlap each other and CPU pools. They must
      // exclude all positively reserved GPU cores. Do not treat them as owners
      // when checking cross-WU exclusive launch reservations.
      for (const auto &gpu: config.getGPUs()) {
        gpuDeviceAllocations[name][gpu] = allocatablePerformanceLevels.front();
        if (allocatablePerformanceLevels.front().empty())
          shortage(name, gpu, "No unreserved Performance 1 CPUs remain for shared GPU helpers. Reduce another GPU reservation or reserve cores for this GPU. GPU folding is waiting.");
      }
    }
  }
}


bool CPUResources::refreshTopology(const string &reason) {
  auto previousFastCores = fastPhysicalCores;
  topologyRefreshCount++;
  auto &sys = SystemInfo::instance();

  LOG_DEBUG(1, "CPU topology refresh #" << topologyRefreshCount
    << " requested: reason=" << reason);

  CPUSet newAvailable = sys.getAvailableCPUs();
  bool newHard =
    sys.getCPUAffinityCapability() == SystemInfo::CPU_AFFINITY_HARD;

  // An empty availability set means cbang could not represent it reliably.
  if (newAvailable.empty()) newHard = false;

  // Keep raw classes separate from current process availability. Persistent
  // class configuration is validated against raw topology, not a temporary
  // cpuset/process restriction.
  auto newRawLevels = sys.getCPUPerformanceLevels();
  CPUSet rawCovered;
  bool validRawLevels = newRawLevels.size() > 1;

  for (auto &raw: newRawLevels) {
    if (raw.empty()) validRawLevels = false;
    for (auto cpu: raw)
      if (!rawCovered.insert(cpu).second) validRawLevels = false;
  }

  // Effective classes are what this process can use right now.
  vector<CPUSet> newLevels;
  CPUSet effectiveCovered;
  bool validEffectiveLevels = validRawLevels;

  for (auto &raw: newRawLevels) {
    CPUSet level = intersect(raw, newAvailable);
    newLevels.push_back(level); // Preserve indexes even when currently empty.
    for (auto cpu: level)
      if (!effectiveCovered.insert(cpu).second) validEffectiveLevels = false;
  }

  if (effectiveCovered != newAvailable) validEffectiveLevels = false;

  // SMT/core topology is optional. If invalid after intersecting with the
  // current available mask, disable only physical-core allocation, not affinity.
  vector<CPUSet> newCores;
  CPUSet coreCovered;
  bool validCores = true;

  const auto rawCores = sys.getCPUCoreThreads();
  for (const auto &raw: rawCores) {
    CPUSet core = intersect(raw, newAvailable);
    if (core.empty()) continue;

    for (auto cpu: core)
      if (!coreCovered.insert(cpu).second) validCores = false;
    newCores.push_back(core);
  }

  if (coreCovered != newAvailable) validCores = false;
  if (!validCores) newCores.clear();

  // cbang reports one complete class for homogeneous topology and an empty
  // map for unknown classification. Require a valid physical-core map too.
  bool newHomogeneous = newHard && !newCores.empty() &&
    newRawLevels.size() == 1 && effectiveCovered == newAvailable;
  bool newConfigurable = newHard && validRawLevels;
  bool newEffective = newHard && validEffectiveLevels;
  bool changed = hardAffinity != newHard || homogeneous != newHomogeneous ||
    configurableClasses != newConfigurable || effectiveClasses != newEffective ||
    available != newAvailable ||
    rawPerformanceLevels != newRawLevels ||
    performanceLevels != newLevels ||
    coreThreads != newCores;

  if (changed) {
    LOG_DEBUG(1, "CPU topology previous: generation=" << topologyGeneration
      << " hard-affinity=" << hardAffinity
      << " configurable-classes=" << configurableClasses
      << " effective-classes=" << effectiveClasses
      << " available=" << formatCPUs(available));
  }

  hardAffinity = newHard;
  homogeneous = newHomogeneous;
  configurableClasses = newConfigurable;
  effectiveClasses = newEffective;
  available.swap(newAvailable);
  rawPerformanceLevels.swap(newRawLevels);
  performanceLevels.swap(newLevels);
  coreThreads.swap(newCores);
  fastPhysicalCores.clear();
  if (supportsGPUAffinity() && validCores)
    for (const auto &raw: rawCores) {
      if (raw.empty()) continue;
      bool complete = true;
      for (auto cpu: raw)
        if (!available.count(cpu) || !performanceLevels.front().count(cpu))
          complete = false;
      if (complete) fastPhysicalCores.push_back(raw);
    }

  changed |= previousFastCores != fastPhysicalCores;
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

  if (!validRawLevels && !rawPerformanceLevels.empty())
    LOG_DEBUG(1, "CPU topology: raw performance-class map is not usable for "
      "class configuration");

  if (!validEffectiveLevels && validRawLevels)
    LOG_DEBUG(1, "CPU topology: effective CPU set does not currently form a "
      "complete performance-class map");

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


CPUResources::CPUList CPUResources::orderCPUs(const CPUSet &cpus) const {
  CPUList result;
  if (cpus.empty()) return result;

  if (coreThreads.empty()) {
    result.insert(result.end(), cpus.begin(), cpus.end());
    return result;
  }

  CPUSet emitted;
  vector<CPUList> siblings;

  // Pass 1: one logical CPU from every physical core represented in the set.
  for (auto &core: coreThreads) {
    CPUList inCore;
    for (auto cpu: core)
      if (cpus.count(cpu)) inCore.push_back(cpu);
    if (inCore.empty()) continue;

    sort(inCore.begin(), inCore.end());
    result.push_back(inCore.front());
    emitted.insert(inCore.front());

    if (1 < inCore.size())
      siblings.push_back(CPUList(inCore.begin() + 1, inCore.end()));
  }

  // Defensive fallback for a topology transition. Do not lose a usable CPU
  // because the optional core map changed between discovery and allocation.
  for (auto cpu: cpus)
    if (!emitted.count(cpu)) {
      bool isSibling = false;
      for (auto &list: siblings)
        if (find(list.begin(), list.end(), cpu) != list.end()) {
          isSibling = true;
          break;
        }
      if (!isSibling) result.push_back(cpu);
    }

  // Pass 2: SMT siblings only after every physical core got one thread.
  for (auto &list: siblings)
    for (auto cpu: list) result.push_back(cpu);

  return result;
}


CPUResources::CPUList CPUResources::orderByPerformance(
    const vector<CPUSet> &levels) const {
  CPUList result;
  CPUSet emitted;

  for (auto &level: levels)
    for (auto cpu: orderCPUs(level))
      if (emitted.insert(cpu).second) result.push_back(cpu);

  // If the current class map is incomplete, preserve correctness by appending
  // any usable CPUs not represented by a performance class.
  for (auto cpu: orderCPUs(allocatable))
    if (emitted.insert(cpu).second) result.push_back(cpu);

  return result;
}


bool CPUResources::validateAllocations() {
  map<unsigned, string> owners;
  bool overlap = false;
  for (const auto &entry: allocations)
    for (auto cpu: entry.second)
      if (!allocatable.count(cpu) || gpuReservedCPUs.count(cpu) ||
          !owners.emplace(cpu, entry.first).second) overlap = true;
  for (const auto &core: coreThreads) {
    const string *owner = nullptr;
    for (auto cpu: core) {
      auto it = owners.find(cpu);
      if (it == owners.end()) continue;
      if (owner && *owner != it->second) overlap = true;
      owner = &it->second;
    }
  }
  if (!overlap) return true;
  // Keep managed mode with empty pools so an invariant failure cannot turn
  // into an unrestricted launch. This covers physical as well as LP overlap.
  allocations.clear();
  workerBudgets.clear();
  runtimeFallback = true;
  runtimeFallbackReason = "internal CPU allocation overlap; CPU folding suspended";
  LOG_ERROR(runtimeFallbackReason);
  return false;
}


// Consume only the GPU-filtered pool. Assign whole-core ownership and worker
// budgets together, then verify disjoint allocations before publication.
void CPUResources::allocateCPUPools(const Groups &groups,
    const vector<string> &names, bool useClasses) {
  CPUSet free = allocatable;
  map<string, CPUSet> pools;
  map<string, vector<unsigned>> classBudgets;
  auto assign = [&] (const map<string, unsigned> &demand, const CPUList &order) {
    auto part = CPUExecutionPlan::partition(demand, order, coreThreads, false);
    for (const auto &entry: demand) {
      const auto &mask = part[entry.first];
      unsigned budget = min<unsigned>(entry.second, mask.size());
      workerBudgets[entry.first] += budget;
      pools[entry.first].insert(mask.begin(), mask.end());
      for (auto cpu: mask) free.erase(cpu);
    }
    return part;
  };
  auto allocateClasses = [&] {
    // Explicit classes have priority. Allocate whole cores, never LPs that
    // could later be expanded into another group's physical-core pool.
    if (useClasses)
      for (unsigned i = 0; i < allocatablePerformanceLevels.size(); ++i) {
        map<string, unsigned> demand;
        for (const auto &name: names) {
          const auto &config = groups.getGroup(name).getConfig();
          if (config.usesCPUClasses()) demand[name] = config.getCPUClassCounts()[i];
        }
        auto part = assign(demand, orderCPUs(intersect(free, allocatablePerformanceLevels[i])));
        for (const auto &entry: demand) {
          classBudgets[entry.first].resize(allocatablePerformanceLevels.size());
          classBudgets[entry.first][i] = min<unsigned>(entry.second, part[entry.first].size());
        }
      }
  };
  allocateClasses();
  // Max-min fair worker targets for General groups, including runtime class
  // fallback. Whole-core granularity can reduce a target further.
  map<string, unsigned> targets;
  auto calculateGeneralTargets = [&] {
    unsigned capacity = free.size();
    while (capacity) {
      bool progress = false;
      for (const auto &name: names) {
        const auto &config = groups.getGroup(name).getConfig();
        if (useClasses && config.usesCPUClasses()) continue;
        if (capacity && targets[name] < config.getConfiguredCPUTotal()) {
          ++targets[name]; --capacity; progress = true;
        }
      }
      if (!progress) break;
    }
  };
  calculateGeneralTargets();
  CPUList order = effectiveClasses ? orderByPerformance(allocatablePerformanceLevels) : orderCPUs(allocatable);
  CPUList generalOrder;
  for (auto cpu: order) if (free.count(cpu)) generalOrder.push_back(cpu);
  auto spreadClassPools = [&] {
    // Finish physical spreading for explicit classes before General pools
    // consume those cores. Preserve the General budgets achievable without
    // spreading: extra class cores must not create an avoidable shortage.
    if (useClasses && !coreThreads.empty()) {
      const auto baseline = CPUExecutionPlan::partition(targets, generalOrder, coreThreads, false);
      for (unsigned level = 0; level < allocatablePerformanceLevels.size(); ++level) {
        auto candidates = CPUExecutionPlan::physicalPools(
          orderCPUs(intersect(free, allocatablePerformanceLevels[level])), coreThreads);
        bool progress = true;
        while (progress && !candidates.empty()) {
          progress = false;
          for (const auto &name: names) {
            if (!classBudgets.count(name)) continue;
            auto owned = intersect(pools[name], allocatablePerformanceLevels[level]);
            if (CPUExecutionPlan::physicalPools(CPUList(owned.begin(), owned.end()), coreThreads).size()
                >= classBudgets[name][level]) continue;
            for (auto it = candidates.begin(); it != candidates.end(); ++it) {
              CPUList trialOrder;
              for (auto cpu: generalOrder) if (free.count(cpu) && !it->count(cpu)) trialOrder.push_back(cpu);
              auto trial = CPUExecutionPlan::partition(targets, trialOrder, coreThreads, false);
              bool fits = true;
              for (const auto &entry: baseline)
                if (min<size_t>(targets.at(entry.first), trial[entry.first].size()) <
                    min<size_t>(targets.at(entry.first), entry.second.size())) fits = false;
              if (!fits) continue;
              pools[name].insert(it->begin(), it->end());
              for (auto cpu: *it) free.erase(cpu);
              candidates.erase(it); progress = true; break;
            }
          }
        }
      }
      generalOrder.erase(remove_if(generalOrder.begin(), generalOrder.end(),
        [&] (unsigned cpu) {return !free.count(cpu);}), generalOrder.end());
    }
  };
  spreadClassPools();
  assign(targets, generalOrder);
  auto distributeSpareCores = [&] {
    auto spare = CPUExecutionPlan::physicalPools(generalOrder, coreThreads);
    if (spare.empty()) for (auto cpu: generalOrder) spare.push_back({cpu});
    spare.erase(remove_if(spare.begin(), spare.end(), [&] (const CPUSet &core) {
      return !free.count(*core.begin());
    }), spare.end());
    // Spread workers over distinct cores before widening the remaining General
    // pools. Class pools only grow within classes the user actually selected.
    for (bool spread: {true, false}) while (!spare.empty()) {
      bool progress = false;
      for (const auto &name: names) {
        if (!workerBudgets[name]) continue;
        const auto &config = groups.getGroup(name).getConfig();
        bool explicitClass = useClasses && config.usesCPUClasses();
        if (!spread && explicitClass) continue;
        for (auto it = spare.begin(); it != spare.end(); ++it) {
          unsigned limit = workerBudgets[name];
          CPUSet owned = pools[name];
          if (explicitClass) {
            unsigned level = 0;
            while (level < allocatablePerformanceLevels.size() &&
                   !allocatablePerformanceLevels[level].count(*it->begin())) ++level;
            if (level == allocatablePerformanceLevels.size()) continue;
            limit = classBudgets[name][level];
            owned = intersect(owned, allocatablePerformanceLevels[level]);
          }
          auto physical = CPUExecutionPlan::physicalPools(CPUList(owned.begin(), owned.end()), coreThreads);
          unsigned have = physical.empty() ? owned.size() : physical.size();
          if (spread && have >= limit) continue;
          pools[name].insert(it->begin(), it->end());
          for (auto cpu: *it) free.erase(cpu);
          spare.erase(it); progress = true; break;
        }
      }
      if (!progress) break;
    }
  };
  distributeSpareCores();
  for (const auto &name: names) {
    for (auto cpu: order) if (pools[name].count(cpu)) allocations[name].push_back(cpu);
    if (workerBudgets[name] < groups.getGroup(name).getConfig().getConfiguredCPUTotal()) {
      runtimeFallback = true;
      if (runtimeFallbackReason.empty()) runtimeFallbackReason =
        "exclusive whole-core capacity is below requested CPU workers";
    }
  }
  validateAllocations();
}


void CPUResources::update(const Groups &groups, const string &reason) {
  ++allocationGeneration;
  allocations.clear();
  workerBudgets.clear();
  managed = false;
  const bool previousFallback = runtimeFallback;
  const string previousReason = runtimeFallbackReason;
  runtimeFallback = false;
  runtimeFallbackReason.clear();
  vector<string> names;
  for (const auto &name: groups.keys()) names.push_back(name);
  sort(names.begin(), names.end());
  configureGPUAllocations(groups);
  bool anyClass = false;
  bool classesFit = effectiveClasses;
  uint64_t total = 0;
  vector<uint64_t> requested(rawPerformanceLevels.size(), 0);
  for (const auto &name: names) {
    const auto &config = groups.getGroup(name).getConfig();
    total += config.getConfiguredCPUTotal();
    if (!config.usesCPUClasses()) continue;
    anyClass = true;
    auto counts = config.getCPUClassCounts();
    if (counts.size() != requested.size()) classesFit = false;
    for (unsigned i = 0; i < counts.size() && i < requested.size(); ++i)
      requested[i] += counts[i];
  }
  if (total > allocatable.size()) classesFit = false;
  for (unsigned i = 0; i < requested.size(); ++i)
    if (requested[i] > allocatablePerformanceLevels[i].size()) classesFit = false;
  // A physical core cannot belong to two performance classes.
  for (const auto &core: coreThreads) {
    unsigned matches = 0;
    for (const auto &level: performanceLevels)
      if (!intersect(core, level).empty()) ++matches;
    if (matches > 1) classesFit = false;
  }
  if (!anyClass && !homogeneous && gpuReservedCPUs.empty()) {
    // General hybrid/unknown topology retains the legacy OS scheduler.
  } else if (!hardAffinity) {
    runtimeFallback = true;
    runtimeFallbackReason = "hard CPU affinity is not currently available";
  } else {
    managed = true;
    if (anyClass && !classesFit) {
      runtimeFallback = true;
      runtimeFallbackReason = "current topology cannot satisfy saved CPU classes";
    }
    allocateCPUPools(groups, names, anyClass && classesFit);
  }
  if (runtimeFallback && (!previousFallback || previousReason != runtimeFallbackReason))
    LOG_WARNING("CPU allocation fallback: " << runtimeFallbackReason
      << "; saved configuration is unchanged");
  map<string, string> summaries;
  for (const auto &name: names) {
    ostringstream out;
    out << "managed=" << managed << " fallback=" << runtimeFallback
        << " workers=" << getGroupWorkerCount(name)
        << " physical-pool=" << formatCPUs(getGroupCPUs(name))
        << " policy={" << groups.getGroup(name).getConfig().getCPUConfigDescription() << '}';
    summaries[name] = out.str();
    if (loggedAllocationSummaries[name] != out.str())
      LOG_INFO(3, "CPU allocation RG '" << (name.empty() ? "Default" : name) << "': " << out.str());
  }
  loggedAllocationSummaries.swap(summaries);
  LOG_DEBUG(2, "CPU allocation generation " << allocationGeneration << " reason=" << reason);
}

const CPUResources::CPUList &CPUResources::getGroupCPUs(const string &name) const {
  static const CPUList empty;
  auto it = allocations.find(name);
  return it == allocations.end() ? empty : it->second;
}
unsigned CPUResources::getGroupWorkerCount(const string &name) const {
  auto it = workerBudgets.find(name);
  return it == workerBudgets.end() ? 0 : it->second;
}
const CPUResources::CPUSet &CPUResources::getGPUCPUs(const string &name, const string &gpu) const {
  static const CPUSet empty;
  auto group = gpuDeviceAllocations.find(name);
  if (group == gpuDeviceAllocations.end()) return empty;
  auto device = group->second.find(gpu);
  return device == group->second.end() ? empty : device->second;
}

const string &CPUResources::getGPUAllocationShortage(
    const string &name, const string &gpu) const {
  static const string empty;
  auto group = gpuAllocationShortages.find(name);
  if (group == gpuAllocationShortages.end()) return empty;
  auto device = group->second.find(gpu);
  return device == group->second.end() ? empty : device->second;
}
