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

#include "ReferenceRGPlanner.h"
#include "CPUSetUtils.h"
#include "ReferenceExecutionPlan.h"
#include <algorithm>
#include <iterator>
#include <utility>
#include <stdexcept>

using namespace std;
using namespace FAH::Client;

// Planning phases: reserve GPU helper CPUs, select feasible class policy, then
// allocate class pools and General worker targets from the remaining capacity.
// Repair/spread whole-core pools before validating managed CPU ownership. The
// local Planner accumulates a detached Result; CPUResources publishes it later.

namespace {
  using CPUSet = ReferenceRGPlanner::CPUSet;
  using CPUList = ReferenceRGPlanner::CPUList;

  // Round-robin max-min targets use lexical RG-name order. An indivisible
  // remainder consistently favours earlier names, rather than rotating priority
  // and causing mask churn. These count workers, before whole-core granularity
  // can lower fulfilled demand.
  map<string, unsigned> generalWorkerTargets(
      const vector<pair<string, unsigned>> &requests, unsigned capacity) {
    map<string, unsigned> targets;
    while (capacity) {
      bool progress = false;
      for (const auto &request: requests)
        if (capacity && targets[request.first] < request.second) {
          ++targets[request.first]; --capacity; progress = true;
        }
      if (!progress) break;
    }
    return targets;
  }


  // Compare fulfilled worker demand, not raw mask sizes. Extra SMT capacity
  // in a whole-core pool must not hide a shortage introduced in another group.
  bool preservesWorkerBudgets(const map<string, unsigned> &targets,
      const ReferenceExecutionPlan::Pools &baseline,
      const ReferenceExecutionPlan::Pools &trial) {
    for (const auto &entry: baseline) {
      auto candidate = trial.find(entry.first);
      size_t capacity = candidate == trial.end() ? 0 : candidate->second.size();
      if (min<size_t>(targets.at(entry.first), capacity) <
          min<size_t>(targets.at(entry.first), entry.second.size())) return false;
    }
    return true;
  }



  class Planner : public ReferenceRGPlanner::Topology, public ReferenceRGPlanner::Result {
  public:
    map<string, ReferenceRGPlanner::Request> requests;
    vector<string> requestOrder;
    Planner(const ReferenceRGPlanner::Topology &topology,
        const vector<ReferenceRGPlanner::Request> &input) : Topology(topology) {
      for (const auto &request: input) {
        if (!requests.emplace(request.name, request).second)
          throw invalid_argument("Duplicate CPU allocation request name: " + request.name);
        requestOrder.push_back(request.name);
      }
      // Canonical priority must not depend on insertion/load order at the caller.
      sort(requestOrder.begin(), requestOrder.end());
    }
    bool supportsGPUAffinity() const {
      return ReferenceRGPlanner::supportsGPUAffinity(
        hardAffinity, homogeneous, effectiveClasses, performanceLevels);
    }
    CPUList orderCPUs(const CPUSet &cpus) const {
      return ReferenceRGPlanner::orderCPUs(cpus, coreThreads);
    }
    CPUList orderByPerformance(const vector<CPUSet> &levels) const;
    void configureGPUAllocations();
    void allocateCPUPools(const vector<string> &names, bool useClasses);
    // Per-invocation scratch state. Named phases preserve the original policy
    // order, and never touch the live CPUResources object.
    CPUSet free;
    map<string, CPUSet> pools;
    map<string, vector<unsigned>> classBudgets;
    map<string, unsigned> targets;
    CPUList order, generalOrder;
    bool classTopologyUsable = false;
    ReferenceExecutionPlan::Pools assign(const map<string, unsigned> &demand, const CPUList &order);
    void allocateClasses(const vector<string> &names, bool useClasses);
    void spreadClassPools(const vector<string> &names, bool useClasses);
    void distributeSpareCores(const vector<string> &names, bool useClasses);
    void build();
  };
}

ReferenceRGPlanner::CPUList ReferenceRGPlanner::orderCPUs(
    const CPUSet &cpus, const vector<CPUSet> &coreThreads) {
  CPUList result;
  if (cpus.empty()) return result;

  if (coreThreads.empty()) {
    result.insert(result.end(), cpus.begin(), cpus.end());
    return result;
  }

  CPUSet represented;
  vector<CPUList> siblings;

  // Pass 1: one logical CPU from every physical core represented in the set.
  for (auto &core: coreThreads) {
    CPUList inCore;
    for (auto cpu: core)
      if (cpus.count(cpu)) inCore.push_back(cpu);
    if (inCore.empty()) continue;

    result.push_back(inCore.front());
    represented.insert(inCore.begin(), inCore.end());

    if (1 < inCore.size())
      siblings.push_back(CPUList(inCore.begin() + 1, inCore.end()));
  }

  // Defensive fallback for a topology transition. Do not lose a usable CPU
  // because the optional core map changed between discovery and allocation.
  for (auto cpu: cpus)
    if (!represented.count(cpu)) result.push_back(cpu);

  // Pass 2: SMT siblings only after every physical core got one thread.
  for (auto &list: siblings)
    for (auto cpu: list) result.push_back(cpu);

  return result;
}


bool ReferenceRGPlanner::validate(Result &result, const vector<CPUSet> &coreThreads) {
  map<unsigned, string> owners;
  bool overlap = false;
  for (const auto &entry: result.allocations)
    for (auto cpu: entry.second)
      if (!result.allocatable.count(cpu) || result.gpuReservedCPUs.count(cpu) ||
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
  bool invalidBudget = false;
  for (const auto &entry: result.workerBudgets) {
    auto pool = result.allocations.find(entry.first);
    // Zero budgets may intentionally have no pool. Positive budgets require
    // enough distinct owned LPs; the ownership checks above reject duplicates.
    if (entry.second && (pool == result.allocations.end() ||
        entry.second > pool->second.size())) invalidBudget = true;
  }
  if (!overlap && !invalidBudget) return true;
  // Keep managed mode with empty pools so an invariant failure cannot turn
  // into an unrestricted launch. This covers physical as well as LP overlap.
  result.managed = true;
  result.allocations.clear();
  result.workerBudgets.clear();
  result.runtimeFallback = true;
  result.runtimeFallbackReason = overlap ?
    "internal CPU allocation overlap; CPU folding suspended" :
    "internal CPU worker budget exceeds owned capacity; CPU folding suspended";
  return false;
}


bool ReferenceRGPlanner::validate(Result &result, const Topology &topology,
    const vector<Request> &requests) {
  const bool cpuValid = validate(result, topology.coreThreads);
  bool valid = cpuValid;
  CPUSet exclusive;
  map<string, const Request *> byName;
  for (const auto &request: requests) byName.emplace(request.name, &request);
  for (const auto &group: result.gpuDeviceAllocations) {
    auto request = byName.find(group.first);
    for (const auto &device: group.second) {
      const auto &mask = device.second;
      if (request == byName.end() || !request->second->gpus.count(device.first)) {
        valid = false; continue;
      }
      for (auto cpu: mask) if (!topology.available.count(cpu)) valid = false;
      if (!request->second->reservedCores) {
        if (!intersectCPUs(mask, result.gpuReservedCPUs).empty()) valid = false;
        continue;
      }
      // An empty exclusive mask is a blocked reservation, never shared fallback.
      if (mask.empty()) continue;
      CPUSet covered;
      unsigned count = 0;
      for (const auto &core: topology.fastPhysicalCores) {
        if (intersectCPUs(mask, core).empty()) continue;
        if (core.empty() || intersectCPUs(mask, core) != core) {valid = false; continue;}
        ++count;
        for (auto cpu: core) if (!covered.insert(cpu).second) valid = false;
      }
      if (covered != mask || count != request->second->reservedCores) valid = false;
      for (auto cpu: mask) if (!exclusive.insert(cpu).second) valid = false;
    }
  }
  if (exclusive != result.gpuReservedCPUs) valid = false;
  if (valid) return true;
  // Reject every launch mask, not just CPU folding, after invariant failure.
  result.managed = true;
  result.allocations.clear(); result.workerBudgets.clear();
  for (auto &group: result.gpuDeviceAllocations)
    for (auto &device: group.second) {
      device.second.clear();
      result.gpuAllocationShortages[group.first][device.first] =
        "Internal allocation validation failed. GPU folding is waiting.";
    }
  result.runtimeFallback = true;
  if (cpuValid)
    result.runtimeFallbackReason = "internal GPU allocation invariant failure; folding suspended";
  return false;
}


void Planner::configureGPUAllocations() {
  auto shortage = [&](const string &name, const string &gpu, const string &reason) {
    gpuAllocationShortages[name][gpu] = reason;
  };
  gpuDeviceAllocations.clear();
  gpuReservedCPUs.clear();
  allocatable = available;
  allocatablePerformanceLevels = performanceLevels;
  if (!supportsGPUAffinity()) {
    for (const auto &name: requestOrder) {
      const auto &config = requests.at(name);
      if (!config.reservedCores) continue;
      for (const auto &gpu: config.gpus)
        shortage(name, gpu, "Requested " + to_string(config.reservedCores) +
          " exclusive Performance 1 physical cores, but hard affinity or usable topology is unavailable. GPU folding is waiting.");
    }
    return;
  }

  // GPU reservations precede all CPU allocations, including their own RG.
  // Reservation priority is lexical RG name order, then lexical GPU ID order
  // within each request (gpus is a set). Earlier successful reservations stay
  // allocated during shortages. A later GPU waits if its complete reservation
  // cannot fit. Caller insertion/load order does not change this policy.
  for (auto &name: requestOrder) {
    const auto &config = requests.at(name);
    if (config.gpus.empty() || !config.reservedCores) continue;
    for (const auto &gpu: config.gpus) {
      CPUSet mask;
      unsigned count = 0;
      for (const auto &core: fastPhysicalCores) {
        bool free = true;
        for (auto cpu: core) if (gpuReservedCPUs.count(cpu)) free = false;
        if (!free) continue;
        mask.insert(core.begin(), core.end());
        if (++count == config.reservedCores) break;
      }
      // A shortage never silently converts exclusive GPU work to shared.
      if (count != config.reservedCores) {
        shortage(name, gpu, "Requested " + to_string(config.reservedCores) +
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
  for (auto &name: requestOrder) {
    const auto &config = requests.at(name);
    if (!config.gpus.empty() && !config.reservedCores) {
      // Shared helper masks may overlap each other and CPU pools. They must
      // exclude all positively reserved GPU cores. Do not treat them as owners
      // when checking cross-WU exclusive launch reservations.
      for (const auto &gpu: config.gpus) {
        gpuDeviceAllocations[name][gpu] = allocatablePerformanceLevels.front();
        if (allocatablePerformanceLevels.front().empty())
          shortage(name, gpu, "No unreserved Performance 1 CPUs remain for shared GPU helpers. Reduce another GPU reservation or reserve cores for this GPU. GPU folding is waiting.");
      }
    }
  }
}


CPUList Planner::orderByPerformance(
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


ReferenceExecutionPlan::Pools Planner::assign(
    const map<string, unsigned> &demand, const CPUList &order) {
  ReferenceExecutionPlan::RebalanceReport report;
  auto part = ReferenceExecutionPlan::partition(demand, order, coreThreads, false, &report);
  if (report.outcome != ReferenceExecutionPlan::RebalanceReport::Outcome::NotNeeded)
    rebalanceReports.push_back(report);
  for (const auto &entry: demand) {
    const auto &mask = part[entry.first];
    unsigned budget = min<unsigned>(entry.second, mask.size());
    workerBudgets[entry.first] += budget;
    // Merge the partitioned pool unchanged; its visible cores are indivisible.
    pools[entry.first].insert(mask.begin(), mask.end());
    for (auto cpu: mask) free.erase(cpu);
  }
  return part;
}


void Planner::allocateClasses(const vector<string> &names, bool useClasses) {
  // Explicit classes have priority. Allocate whole cores, never LPs that
  // could later be expanded into another group's physical-core pool.
  if (useClasses && classTopologyUsable)
    for (unsigned i = 0; i < allocatablePerformanceLevels.size(); ++i) {
      vector<pair<string, unsigned>> requested;
      for (const auto &name: names) {
        const auto &config = requests.at(name);
        if (config.classes) requested.emplace_back(name,
          i < config.classCounts.size() ? config.classCounts[i] : 0);
      }
      const auto capacity = intersectCPUs(free, allocatablePerformanceLevels[i]);
      // Shortages are fair within this class, never redirected to another level.
      auto demand = generalWorkerTargets(requested, capacity.size());
      for (const auto &request: requested) demand.emplace(request.first, 0);
      auto part = assign(demand, orderCPUs(capacity));
      for (const auto &entry: demand) {
        classBudgets[entry.first].resize(allocatablePerformanceLevels.size());
        classBudgets[entry.first][i] = min<unsigned>(entry.second, part[entry.first].size());
      }
    }
}


void Planner::spreadClassPools(const vector<string> &names, bool useClasses) {
  // Finish physical spreading for explicit classes before General pools
  // consume those cores. Preserve the General budgets achievable without
  // spreading: extra class cores must not create an avoidable shortage.
  if (useClasses && !coreThreads.empty()) {
    const auto baseline = ReferenceExecutionPlan::partition(targets, generalOrder, coreThreads, false);
    for (unsigned level = 0; level < allocatablePerformanceLevels.size(); ++level) {
      auto candidates = ReferenceExecutionPlan::physicalPools(
        orderCPUs(intersectCPUs(free, allocatablePerformanceLevels[level])), coreThreads);
      bool progress = true;
      while (progress && !candidates.empty()) {
        progress = false;
        for (const auto &name: names) {
          if (!classBudgets.count(name)) continue;
          auto owned = intersectCPUs(pools[name], allocatablePerformanceLevels[level]);
          if (ReferenceExecutionPlan::physicalPools(CPUList(owned.begin(), owned.end()), coreThreads).size()
              >= classBudgets[name][level]) continue;
          for (auto it = candidates.begin(); it != candidates.end(); ++it) {
            CPUList trialOrder;
            for (auto cpu: generalOrder) if (free.count(cpu) && !it->count(cpu)) trialOrder.push_back(cpu);
            auto trial = ReferenceExecutionPlan::partition(targets, trialOrder, coreThreads, false);
            if (!preservesWorkerBudgets(targets, baseline, trial)) continue;
            // Transfer all visible siblings of this free core together.
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
}


void Planner::distributeSpareCores(const vector<string> &names, bool useClasses) {
  auto spare = ReferenceExecutionPlan::physicalPools(generalOrder, coreThreads);
  if (spare.empty()) for (auto cpu: generalOrder) spare.push_back({cpu});
  spare.erase(remove_if(spare.begin(), spare.end(), [&] (const CPUSet &core) {
    return !free.count(*core.begin());
  }), spare.end());
  // Spread only up to worker demand; leave surplus cores available for later
  // groups/WUs. Class pools grow only within classes the user selected.
  while (!spare.empty()) {
    bool progress = false;
    for (const auto &name: names) {
      if (!workerBudgets[name]) continue;
      const auto &config = requests.at(name);
      bool explicitClass = useClasses && config.classes;
      for (auto it = spare.begin(); it != spare.end(); ++it) {
        unsigned limit = workerBudgets[name];
        CPUSet owned = pools[name];
        if (explicitClass) {
          if (!classBudgets.count(name)) continue;
          unsigned level = 0;
          while (level < allocatablePerformanceLevels.size() &&
                 !allocatablePerformanceLevels[level].count(*it->begin())) ++level;
          if (level == allocatablePerformanceLevels.size()) continue;
          limit = classBudgets[name][level];
          owned = intersectCPUs(owned, allocatablePerformanceLevels[level]);
        }
        auto physical = ReferenceExecutionPlan::physicalPools(CPUList(owned.begin(), owned.end()), coreThreads);
        unsigned have = physical.empty() ? owned.size() : physical.size();
        if (have >= limit) continue;
        // Widen ownership with a complete free core, not selected LPs.
        pools[name].insert(it->begin(), it->end());
        for (auto cpu: *it) free.erase(cpu);
        spare.erase(it); progress = true; break;
      }
    }
    if (!progress) break;
  }
}


// Consume only the GPU-filtered pool. Assign whole-core ownership and worker
// budgets together, then verify disjoint allocations before publication.
void Planner::allocateCPUPools(const vector<string> &names, bool useClasses) {
  free = allocatable;
  // Phase order is policy: reserve class pools, protect General targets,
  // spread physical coverage up to worker demand, leaving surplus unowned.
  // Pool capacity can exceed the requested worker count.
  allocateClasses(names, useClasses);
  // General shortages use only the remaining capacity. Class requests never
  // enter this queue, even if their selected level is missing or oversubscribed.
  vector<pair<string, unsigned>> generalRequests;
  for (const auto &name: names) {
    const auto &config = requests.at(name);
    if (!useClasses || !config.classes)
      generalRequests.emplace_back(name, config.workers);
  }
  targets = generalWorkerTargets(generalRequests, free.size());
  order = effectiveClasses ? orderByPerformance(allocatablePerformanceLevels) : orderCPUs(allocatable);
  for (auto cpu: order) if (free.count(cpu)) generalOrder.push_back(cpu);
  spreadClassPools(names, useClasses);
  assign(targets, generalOrder);
  distributeSpareCores(names, useClasses);
  for (const auto &name: names) {
    for (auto cpu: order) if (pools[name].count(cpu)) allocations[name].push_back(cpu);
    if (workerBudgets[name] < requests.at(name).workers) {
      runtimeFallback = true;
      if (runtimeFallbackReason.empty()) runtimeFallbackReason = requests.at(name).classes ?
        "selected performance-class capacity is below requested CPU workers; class boundaries retained" :
        "exclusive whole-core capacity is below requested CPU workers";
    }
  }
  // CPU and GPU invariants are checked together before returning the result.
}


void Planner::build() {
  const auto &names = requestOrder;
  // Stable lexical ties favour earlier RG names during shortages. This is
  // deterministic allocation, not time-based fairness or rotating priority.
  configureGPUAllocations();
  bool anyClass = false;
  classTopologyUsable = effectiveClasses;
  for (const auto &entry: requests)
    if (entry.second.classes)
      anyClass |= any_of(entry.second.classCounts.begin(), entry.second.classCounts.end(),
        [](uint32_t count) {return count != 0;});
  // Unknown/ambiguous class topology blocks class work, not General demand.
  // Whole physical cores cannot straddle selected performance levels.
  for (const auto &core: coreThreads) {
    unsigned matches = 0;
    for (const auto &level: performanceLevels)
      if (!intersectCPUs(core, level).empty()) ++matches;
    if (matches > 1) classTopologyUsable = false;
  }
  if (!anyClass && !homogeneous && gpuReservedCPUs.empty()) {
    // General hybrid/unknown topology retains the legacy OS scheduler.
  } else if (!hardAffinity) {
    runtimeFallback = true;
    runtimeFallbackReason = affinityUnavailableReason;
  } else {
    managed = true;
    if (anyClass && !classTopologyUsable) {
      runtimeFallback = true;
      runtimeFallbackReason = "performance-class topology unavailable; class CPU folding is waiting";
    }
    allocateCPUPools(names, anyClass);
  }
}

ReferenceRGPlanner::Result ReferenceRGPlanner::plan(const Topology &topology,
    const vector<Request> &requests) {
  auto demand = requests;
  for (auto &request: demand) if (!request.wantsResources) {
    request.workers = request.reservedCores = 0;
    request.classes = false;
    request.classCounts.clear();
    request.gpus.clear();
  }
  Planner planner(topology, demand);
  planner.build();
  validate(planner, topology, demand);
  return std::move(static_cast<Result &>(planner));
}
