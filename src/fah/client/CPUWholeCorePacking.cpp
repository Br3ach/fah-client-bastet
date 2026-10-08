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
#include "CPUExecutionPlan.h"
#include <algorithm>
#include <cstdint>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <utility>

namespace FAH { namespace Client {
namespace {
  using CPUSet = CPUExecutionPlan::CPUSet;
  using Pools = CPUWholeCorePacking::Pools;
  using WorkerRequests = std::map<std::string, unsigned>;

  std::string ordinalKey(unsigned index, size_t count) {
    auto ordinal = std::to_string(index);
    return std::string(std::to_string(count).size() - ordinal.size(), '0') + ordinal;
  }

  void validatePacking(const CPUWholeCorePacking::Layout &allocations,
      const std::vector<CPUWholeCorePacking::Request> &requests,
      const CPUAllocationSlices &resources, const std::vector<CPUSet> &physical) {
    // Validate the complete result independently of search termination.
    CPUSet owned; std::vector<unsigned> used(resources.size());
    for (unsigned i = 0; i < requests.size(); ++i) {
      unsigned total = 0;
      for (unsigned level = 0; level < resources.size(); ++level) {
        const auto &slice = allocations[i][level];
        total += slice.workers; used[level] += slice.workers;
        if ((!slice.workers && !slice.pool.empty()) ||
            (requests[i].ownsCores ? slice.workers > slice.pool.size() : !slice.pool.empty()))
          throw std::logic_error("Invalid resource ownership capacity");
        for (auto cpu: slice.pool) if (!resources[level].pool.count(cpu) || !owned.insert(cpu).second)
          throw std::logic_error("Invalid resource ownership");
        for (const auto &core: physical) {
          unsigned present = 0; for (auto cpu: core) present += slice.pool.count(cpu);
          if (present && present != core.size()) throw std::logic_error("Split physical core");
        }
      }
      if (total > requests[i].maximum || (total && total < requests[i].minimum))
        throw std::logic_error("Invalid fulfilled request total");
    }
    for (unsigned level = 0; level < resources.size(); ++level)
      if (used[level] > resources[level].workers) throw std::logic_error("Resource budget exceeded");
  }

  // Partition moves complete, non-empty available-core sets: every visible
  // sibling belongs to the same pool, or none does. Excluded siblings need not
  // be present. The first LP identifies ownership, including singleton fallback.
  // This helper must not be used with arbitrary partial process masks.
  bool ownsCore(const CPUSet &pool, const CPUSet &core) {
    return pool.count(*core.begin());
  }

  // Return the first smaller recipient core that keeps the donor satisfied.
  // An empty result means this donor core cannot be exchanged safely.
  CPUSet findReplacementCore(const CPUSet &recipientPool,
      const std::vector<CPUSet> &availableCores, unsigned assignedCoreCount,
      unsigned transferredWidth, unsigned donorRemaining, unsigned donorWorkers) {
    for (unsigned i = 0; i < assignedCoreCount; ++i) {
      const auto &candidate = availableCores[i];
      const bool recipientOwnsCore = ownsCore(recipientPool, candidate);
      const bool reducesWidth = candidate.size() < transferredWidth;
      const bool keepsDonorSatisfied = donorRemaining + candidate.size() >= donorWorkers;
      if (recipientOwnsCore && reducesWidth && keepsDonorSatisfied) return candidate;
    }
    return {};
  }

  void exchangeCores(CPUSet &donorPool, CPUSet &recipientPool,
      const CPUSet &transferredCore, const CPUSet &replacementCore) {
    for (auto cpu: transferredCore) donorPool.erase(cpu);
    for (auto cpu: replacementCore) recipientPool.erase(cpu);
    // Both sides move complete sets, preserving all-or-none ownership.
    donorPool.insert(replacementCore.begin(), replacementCore.end());
    recipientPool.insert(transferredCore.begin(), transferredCore.end());
  }

  // Greedy assignment can overfill small requests on mixed-SMT hardware.
  // Each transfer/swap increases fulfilled demand without undersupplying its
  // donor. Scan order determines ties and must remain stable.
  void repairWorkerDeficits(const WorkerRequests &requests,
      const std::vector<CPUSet> &availableCores, unsigned assignedCoreCount,
      Pools &pools) {
    bool repaired = true;
    while (repaired) {
      repaired = false;
      for (const auto &recipient: requests) {
        auto &recipientPool = pools[recipient.first];
        if (recipientPool.size() >= recipient.second) continue;
        for (const auto &donor: requests) {
          auto &donorPool = pools[donor.first];
          const bool sameGroup = donor.first == recipient.first;
          const bool hasSpareCapacity = donorPool.size() > donor.second;
          if (sameGroup || !hasSpareCapacity) continue;
          for (unsigned i = 0; i < assignedCoreCount && !repaired; ++i) {
            const auto &transferredCore = availableCores[i];
            if (!ownsCore(donorPool, transferredCore)) continue;
            const unsigned donorRemaining = donorPool.size() - transferredCore.size();
            CPUSet replacementCore;
            if (donorRemaining < donor.second) {
              replacementCore = findReplacementCore(recipientPool, availableCores,
                assignedCoreCount, transferredCore.size(), donorRemaining, donor.second);
              if (replacementCore.empty()) continue;
            }
            exchangeCores(donorPool, recipientPool, transferredCore, replacementCore);
            repaired = true;
          }
          if (repaired) break;
        }
        if (repaired) break;
      }
    }
  }

  unsigned poolCapacity(const Pools &pools, const std::string &name) {
    const auto pool = pools.find(name);
    return pool == pools.end() ? 0 : pool->second.size();
  }

  // Total workers are compared by the caller. Equal totals prefer max-min
  // fulfilled counts, then input priority (RG names or WU ordinal keys).
  bool hasFairerFulfillment(const std::vector<unsigned> &candidate,
      const std::vector<unsigned> &baseline) {
    auto sortedCandidate = candidate, sortedBaseline = baseline;
    std::sort(sortedCandidate.begin(), sortedCandidate.end());
    std::sort(sortedBaseline.begin(), sortedBaseline.end());
    return sortedCandidate != sortedBaseline ?
      sortedCandidate > sortedBaseline : candidate > baseline;
  }

  // File-local search state separates scoring and traversal from publication.
  // Local swaps cannot repair every packing. Complete fulfillment exits early;
  // exhaustion retains the best safe result found within the shared state limit.
  struct PackingSearch {
    const std::vector<CPUSet> &cores;
    CPUWholeCorePacking::RebalanceReport &report;
    std::vector<std::string> names;
    std::vector<unsigned> requested, remainingDemand, coreOrder;
    std::vector<unsigned> bestFulfillment, owners, bestOwners;
    std::vector<uint64_t> remainingCapacity;
    std::set<std::pair<unsigned, std::vector<unsigned>>> visited;
    uint64_t totalDemand = 0, bestTotal = 0;
    unsigned unowned;
    bool exhausted = false;
    // Empty constraints retain the original single-pool traversal exactly.
    const std::vector<CPUWholeCorePacking::Request> *constraints = nullptr;
    CPUWholeCorePacking::Prefer prefer;
    std::vector<unsigned> resources, resourceRemaining, resourceBudget;
    std::vector<std::vector<unsigned>> allocation, bestAllocation;

    PackingSearch(const std::vector<CPUWholeCorePacking::Request> &requests,
        const CPUAllocationSlices &slices, const std::vector<CPUSet> &physical,
        const std::vector<unsigned> &resourceIndices, CPUWholeCorePacking::RebalanceReport &report,
        const CPUWholeCorePacking::Prefer &prefer)
      : cores(physical), report(report), constraints(&requests), prefer(prefer) {
      for (const auto &request: requests) {
        requested.push_back(request.maximum); totalDemand += request.maximum;
      }
      remainingDemand = requested; unowned = requests.size();
      for (const auto &slice: slices) resourceBudget.push_back(slice.workers);
      resourceRemaining = resourceBudget;
      allocation.assign(requests.size(), std::vector<unsigned>(slices.size()));
      bestAllocation = allocation; bestFulfillment.assign(requests.size(), 0);
      for (unsigned i = 0; i < physical.size(); ++i) {
        coreOrder.push_back(i); resources.push_back(resourceIndices[i]);
      }
      // Worker-only capacity does not own an OS core. It uses the same search
      // transitions and resource budget as physical ownership.
      auto pending = std::count_if(requests.begin(), requests.end(),
        [](const auto &request) {return !request.ownsCores;});
      // Each worker-only consumer needs at most one chunk per resource.
      // Budget limits, rather than fake per-worker core objects, cap capacity.
      for (unsigned level = 0; level < slices.size(); ++level)
        for (unsigned i = 0; i < std::min<unsigned>(pending, slices[level].workers); ++i) {
          coreOrder.push_back(physical.size()); resources.push_back(level);
        }
      owners.assign(coreOrder.size(), unowned);
      remainingCapacity.resize(coreOrder.size()+1);
      for (size_t i = coreOrder.size(); i > 0; --i)
        remainingCapacity[i-1] = remainingCapacity[i] + width(i-1);
    }

    bool isVirtual(unsigned index) const {return coreOrder[index] == cores.size();}
    unsigned width(unsigned index) const {
      return isVirtual(index) ? resourceBudget[resources[index]] : cores[coreOrder[index]].size();
    }

    bool validMinimums(const std::vector<unsigned> &counts) const {
      if (!constraints) return true;
      for (unsigned i = 0; i < counts.size(); ++i)
        if (counts[i] && counts[i] < (*constraints)[i].minimum) return false;
      return true;
    }

    void baseline() {
      // Establish a safe request-ordered baseline before bounded optimization.
      // Unsuccessful minimums release both worker budget and core ownership.
      for (unsigned group = 0; group < requested.size(); ++group) {
        auto beforeResources = resourceRemaining;
        auto beforeOwners = owners;
        for (unsigned i = 0; i < coreOrder.size() && remainingDemand[group]; ++i) {
          if (owners[i] != unowned || isVirtual(i) == (*constraints)[group].ownsCores) continue;
          auto gain = std::min({remainingDemand[group], resourceRemaining[resources[i]], width(i)});
          if (!gain) continue;
          owners[i] = group;
          allocation[group][resources[i]] += gain;
          remainingDemand[group] -= gain; resourceRemaining[resources[i]] -= gain;
        }
        if (requested[group]-remainingDemand[group] < (*constraints)[group].minimum) {
          resourceRemaining = std::move(beforeResources);
          owners = std::move(beforeOwners);
          remainingDemand[group] = requested[group];
          std::fill(allocation[group].begin(), allocation[group].end(), 0);
        }
      }
      considerCandidate(totalDemand-std::accumulate(remainingDemand.begin(), remainingDemand.end(), uint64_t(0)));
      remainingDemand = requested; resourceRemaining = resourceBudget;
      for (auto &row: allocation) std::fill(row.begin(), row.end(), 0);
      std::fill(owners.begin(), owners.end(), unowned);
    }


    PackingSearch(const WorkerRequests &requests,
        const std::vector<CPUSet> &cores, unsigned assignedCoreCount,
        const Pools &pools, CPUWholeCorePacking::RebalanceReport &report) :
        cores(cores), report(report) {
      for (const auto &request: requests) if (request.second) {
        names.push_back(request.first);
        requested.push_back(request.second);
        totalDemand += request.second;
        const auto fulfilled = std::min(request.second, poolCapacity(pools, request.first));
        bestFulfillment.push_back(fulfilled);
        bestTotal += fulfilled;
      }
      remainingDemand = requested;
      coreOrder.resize(assignedCoreCount);
      std::iota(coreOrder.begin(), coreOrder.end(), 0);
      std::stable_sort(coreOrder.begin(), coreOrder.end(), [&](unsigned a, unsigned b) {
        return cores[a].size() > cores[b].size();
      });
      unowned = names.size();
      owners.assign(coreOrder.size(), unowned);
      remainingCapacity.resize(coreOrder.size() + 1);
      for (size_t i = coreOrder.size(); i > 0; --i)
        remainingCapacity[i - 1] = remainingCapacity[i] + cores[coreOrder[i - 1]].size();
    }

    void considerCandidate(uint64_t fulfilled) {
      if (fulfilled < bestTotal) return;
      std::vector<unsigned> candidate;
      for (unsigned i = 0; i < requested.size(); ++i)
        candidate.push_back(requested[i] - remainingDemand[i]);
      if (!validMinimums(candidate)) return;
      if (fulfilled > bestTotal || (constraints ? prefer(candidate, bestFulfillment) :
          hasFairerFulfillment(candidate, bestFulfillment))) {
        bestTotal = fulfilled;
        bestFulfillment = std::move(candidate);
        bestOwners = owners;
        if (constraints) bestAllocation = allocation;
      }
    }

    bool canAssign(unsigned index, unsigned request) const {
      if (!remainingDemand[request]) return false;
      if (!constraints) return true;
      return isVirtual(index) != (*constraints)[request].ownsCores &&
        resourceRemaining[resources[index]] != 0;
    }

    bool search(unsigned index, uint64_t fulfilled) {
      if (report.visits >= report.MaxStates) {exhausted = true; return false;}
      ++report.visits;
      auto state = remainingDemand;
      if (constraints) state.insert(state.end(), resourceRemaining.begin(), resourceRemaining.end());
      // Retain the first traversal reaching this remaining-demand/budget state.
      // Owner history is not part of the key; stable traversal preserves ties.
      if (!visited.emplace(index, std::move(state)).second) return false;
      considerCandidate(fulfilled);
      if (fulfilled == totalDemand) return true;
      // Prune only strictly lower throughput bounds: equal totals may improve
      // the caller tie policy (or fixed-target fairness).
      if (index == coreOrder.size() || fulfilled +
          std::min(totalDemand - fulfilled, remainingCapacity[index]) < bestTotal)
        return false;
      const auto coreWidth = width(index);
      std::vector<unsigned> choices;
      for (unsigned group = 0; group < remainingDemand.size(); ++group)
        if (canAssign(index, group))
          choices.push_back(group);
      std::stable_sort(choices.begin(), choices.end(), [&](unsigned a, unsigned b) {
        return std::min<uint64_t>(remainingDemand[a], coreWidth) >
               std::min<uint64_t>(remainingDemand[b], coreWidth);
      });
      for (auto group: choices) {
        const unsigned before = remainingDemand[group];
        const auto maximum = constraints ? std::min({before, coreWidth, resourceRemaining[resources[index]]}) :
          std::min<unsigned>(before, coreWidth);
        // Fixed targets keep their original one-transition behavior. Constrained
        // resources may need a smaller gain to leave budget for another request.
        for (unsigned gain = maximum; gain; --gain) {
          remainingDemand[group] -= gain; owners[index] = group;
          if (constraints) {
            resourceRemaining[resources[index]] -= gain;
            allocation[group][resources[index]] += gain;
          }
          const bool complete = search(index + 1, fulfilled + gain);
          remainingDemand[group] = before; owners[index] = unowned;
          if (constraints) {
            resourceRemaining[resources[index]] += gain;
            allocation[group][resources[index]] -= gain;
          }
          if (complete) return true;
          if (exhausted) return false;
          if (!constraints) break;
        }
      }
      // A constrained problem may leave a core unused rather than exclude a
      // feasible consumer elsewhere. The legacy path never adds this branch.
      if (constraints && !exhausted) return search(index + 1, fulfilled);
      return false;
    }

    void run(Pools &pools) {
      using Outcome = CPUWholeCorePacking::RebalanceReport::Outcome;
      search(0, 0);
      report.improved = !bestOwners.empty();
      if (report.improved) {
        Pools replacement;
        for (unsigned i = 0; i < coreOrder.size(); ++i) {
          if (bestOwners[i] == unowned) continue;
          const auto &core = cores[coreOrder[i]];
          auto &pool = replacement[names[bestOwners[i]]];
          // Publish whole input cores only, including a partial best solution.
          pool.insert(core.begin(), core.end());
        }
        pools.swap(replacement);
      }
      report.outcome = exhausted ? Outcome::StateLimit :
        report.improved ? Outcome::Improved : Outcome::Infeasible;
    }
  };

  void rebalanceWorkerDeficits(const WorkerRequests &requests,
      const std::vector<CPUSet> &cores, unsigned assignedCoreCount, Pools &pools,
      CPUWholeCorePacking::RebalanceReport &report) {
    report.cores = assignedCoreCount;
    bool deficit = false;
    for (const auto &request: requests) {
      deficit |= poolCapacity(pools, request.first) < request.second;
      if (request.second) ++report.groups;
    }
    if (!deficit) return;
    if (assignedCoreCount > report.MaxCores) {
      report.outcome = CPUWholeCorePacking::RebalanceReport::Outcome::CoreLimit;
      return;
    }
    PackingSearch(requests, cores, assignedCoreCount, pools, report).run(pools);
  }

  // Index the validated partition cores, including singleton fallback pools.
  // Missing topology must not make every pool appear to own zero cores.
  using PhysicalCoreIndex = std::map<unsigned, unsigned>;

  PhysicalCoreIndex indexPhysicalCores(const std::vector<CPUSet> &cores) {
    PhysicalCoreIndex index;
    for (unsigned i = 0; i < cores.size(); ++i)
      for (auto cpu: cores[i]) index.emplace(cpu, i);
    return index;
  }

  unsigned countOwnedPhysicalCores(const CPUSet &pool,
      const PhysicalCoreIndex &index) {
    std::set<unsigned> owned;
    for (auto cpu: pool) {
      const auto entry = index.find(cpu);
      if (entry != index.end()) owned.insert(entry->second);
    }
    return owned.size();
  }

  // Spread up to worker demand, leaving surplus cores unowned. Claiming cores
  // no worker can use would force restarts when another WU needs them.
  void distributeUnusedCores(const WorkerRequests &requests,
      const std::vector<CPUSet> &availableCores, unsigned next,
      const PhysicalCoreIndex &coreIndex, Pools &pools) {
    while (next < availableCores.size()) {
      bool progress = false;
      for (const auto &request: requests) if (request.second && next < availableCores.size()) {
        auto &pool = pools[request.first];
        const unsigned physicalCount = countOwnedPhysicalCores(pool, coreIndex);
        const bool hasEnoughPhysicalCores = physicalCount >= request.second;
        if (hasEnoughPhysicalCores) continue;
        const auto &core = availableCores[next++];
        // The complete unused core is assigned to one pool, never split.
        pool.insert(core.begin(), core.end());
        progress = true;
      }
      if (!progress) break;
    }
  }
}

CPUWholeCorePacking::Pools CPUWholeCorePacking::partition(const std::map<std::string, unsigned> &requests,
      const std::vector<unsigned> &ordered, const std::vector<CPUSet> &cores,
      bool fillUnused, RebalanceReport *report) {
  RebalanceReport rebalance;
  Pools pools;
  auto availableCores = CPUExecutionPlan::physicalPools(ordered, cores);
  if (availableCores.empty()) { // Unknown topology retains deterministic logical pools.
    for (auto cpu: ordered) availableCores.push_back({cpu});
  }
  // Phase 1: satisfy demand greedily without splitting physical-core ownership.
  unsigned assignedCoreCount = 0;
  auto remainingWorkerDemand = requests;
  while (assignedCoreCount < availableCores.size()) {
    bool progress = false;
    for (auto &entry: remainingWorkerDemand) if (entry.second && assignedCoreCount < availableCores.size()) {
      const auto &core = availableCores[assignedCoreCount++];
      // Establish the all-or-none ownership assumed by repair/spreading.
      pools[entry.first].insert(core.begin(), core.end());
      // The deficit counts workers, not physical cores. A whole SMT core
      // supplies up to core.size() worker slots without splitting ownership.
      // Later spreading increases physical coverage without raising workers.
      entry.second -= std::min<unsigned>(entry.second, core.size());
      progress = true;
    }
    if (!progress) break;
  }
  // Phase 2: local repair protects donors; bounded search can trade whole
  // cores between groups to improve total fulfillment under shortage.
  repairWorkerDeficits(requests, availableCores, assignedCoreCount, pools);
  rebalanceWorkerDeficits(requests, availableCores, assignedCoreCount, pools, rebalance);
  if (report) *report = rebalance;
  if (fillUnused) {
    // Phase 3: improve physical spreading without claiming surplus cores. Worker demand
    // stays fixed even when extra logical capacity enters a process pool.
    const auto coreIndex = indexPhysicalCores(availableCores);
    distributeUnusedCores(requests, availableCores, assignedCoreCount, coreIndex, pools);
  }
  std::vector<Request> bounds;
  Layout layout;
  CPUAllocationSlice resource;
  for (auto cpu: ordered) resource.pool.insert(cpu);
  resource.workers = resource.pool.size();
  for (const auto &entry: requests) {
    bounds.push_back({0, entry.second, true});
    const auto it = pools.find(entry.first);
    auto pool = it == pools.end() ? CPUSet{} : it->second;
    // Zero-worker entries in legacy repair may exist but never own a core.
    layout.push_back({{std::min<unsigned>(entry.second, pool.size()), std::move(pool)}});
  }
  validatePacking(layout, bounds, {resource}, availableCores);
  return pools;
}

CPUWholeCorePacking::Result CPUWholeCorePacking::pack(
    const std::vector<Request> &requests, const CPUAllocationSlices &resources,
    const std::vector<CPUSet> &cores, const Prefer &prefer,
    const std::vector<unsigned> &tieTarget) {
  if (!prefer) throw std::invalid_argument("Missing packing tie policy");
  CPUSet allowed;
  uint64_t budget = 0;
  for (const auto &slice: resources) {
    if (slice.workers > slice.pool.size()) throw std::invalid_argument("Resource budget exceeds pool");
    budget += slice.workers;
    for (auto cpu: slice.pool) if (!allowed.insert(cpu).second)
      throw std::invalid_argument("Resource pools overlap");
  }
  if (budget > std::numeric_limits<unsigned>::max()) throw std::invalid_argument("Worker budget overflows");
  for (const auto &request: requests) if (request.minimum > request.maximum)
    throw std::invalid_argument("Request minimum exceeds maximum");
  auto physical = CPUExecutionPlan::physicalPools(std::vector<unsigned>(allowed.begin(), allowed.end()), cores);
  if (!allowed.empty() && physical.empty()) {
    if (!cores.empty()) throw std::invalid_argument("Incomplete resource core map");
    for (auto cpu: allowed) physical.push_back({cpu});
  }
  std::vector<unsigned> resourceIndices;
  for (const auto &core: physical) {
    unsigned level = 0;
    while (level < resources.size() && !resources[level].pool.count(*core.begin())) ++level;
    if (level == resources.size()) throw std::invalid_argument("Core has no resource");
    for (auto cpu: core) if (!resources[level].pool.count(cpu))
      throw std::invalid_argument("Physical core crosses resources");
    resourceIndices.push_back(level);
  }
  Result result;
  result.report.cores = physical.size(); result.report.groups = requests.size();
  PackingSearch search(requests, resources, physical, resourceIndices, result.report, prefer);
  search.baseline();
  const auto baseline = search.bestFulfillment;
  uint64_t upper = 0;
  for (const auto &request: requests) upper += request.maximum;
  upper = std::min(upper, budget);
  if (search.bestTotal < upper ||
      (tieTarget.size() == requests.size() && prefer(tieTarget, search.bestFulfillment))) {
    if (physical.size() > RebalanceReport::MaxCores || requests.size() > RebalanceReport::MaxCores)
      result.report.outcome = RebalanceReport::Outcome::CoreLimit;
    else if (search.coreOrder.size() > RebalanceReport::MaxSearchDepth)
      result.report.outcome = RebalanceReport::Outcome::DepthLimit;
    else {
      search.search(0, 0);
      result.report.improved = search.bestFulfillment != baseline;
      result.report.outcome = search.exhausted ? RebalanceReport::Outcome::StateLimit :
        result.report.improved ? RebalanceReport::Outcome::Improved :
        search.bestTotal == upper ? RebalanceReport::Outcome::NoImprovement :
        RebalanceReport::Outcome::Infeasible;
    }
  }
  result.allocations.resize(requests.size(), CPUAllocationSlices(resources.size()));
  for (unsigned i = 0; i < requests.size(); ++i)
    for (unsigned level = 0; level < resources.size(); ++level)
      result.allocations[i][level].workers = search.bestAllocation[i][level];
  CPUSet owned;
  for (unsigned i = 0; i < search.bestOwners.size(); ++i) {
    auto owner = search.bestOwners[i];
    if (owner == search.unowned || search.isVirtual(i)) continue;
    const auto &core = physical[search.coreOrder[i]];
    auto &pool = result.allocations[owner][search.resources[i]].pool;
    pool.insert(core.begin(), core.end()); owned.insert(core.begin(), core.end());
  }
  // Spread only within the resource already granted to a consumer. This uses
  // the same whole-core spreading helper as the single-resource path.
  const auto coreIndex = indexPhysicalCores(physical);
  for (unsigned level = 0; level < resources.size(); ++level) {
    std::vector<CPUSet> ordered;
    Pools pools; std::map<std::string, unsigned> demand;
    for (unsigned i = 0; i < requests.size(); ++i) if (requests[i].ownsCores) {
      auto key = ordinalKey(i, requests.size());
      demand[key] = result.allocations[i][level].workers;
      pools[key] = result.allocations[i][level].pool;
    }
    for (unsigned i = 0; i < physical.size(); ++i)
      if (resourceIndices[i] == level && !owned.count(*physical[i].begin())) ordered.push_back(physical[i]);
    distributeUnusedCores(demand, ordered, 0, coreIndex, pools);
    for (unsigned i = 0; i < requests.size(); ++i) if (requests[i].ownsCores) {
      auto key = ordinalKey(i, requests.size());
      result.allocations[i][level].pool = std::move(pools[key]);
    }
  }
  validatePacking(result.allocations, requests, resources, physical);
  return result;
}
}}
