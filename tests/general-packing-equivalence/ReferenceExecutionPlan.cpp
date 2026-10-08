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

#include "ReferenceExecutionPlan.h"

#include <algorithm>
#include <cstdint>
#include <numeric>
#include <utility>

namespace FAH { namespace Client {

namespace {
  using CPUSet = ReferenceExecutionPlan::CPUSet;
  using Pools = ReferenceExecutionPlan::Pools;
  using WorkerRequests = std::map<std::string, unsigned>;

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
    ReferenceExecutionPlan::RebalanceReport &report;
    std::vector<std::string> names;
    std::vector<unsigned> requested, remainingDemand, coreOrder;
    std::vector<unsigned> bestFulfillment, owners, bestOwners;
    std::vector<uint64_t> remainingCapacity;
    std::set<std::pair<unsigned, std::vector<unsigned>>> visited;
    uint64_t totalDemand = 0, bestTotal = 0;
    unsigned unowned;
    bool exhausted = false;

    PackingSearch(const WorkerRequests &requests,
        const std::vector<CPUSet> &cores, unsigned assignedCoreCount,
        const Pools &pools, ReferenceExecutionPlan::RebalanceReport &report) :
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
      for (unsigned i = 0; i < names.size(); ++i)
        candidate.push_back(requested[i] - remainingDemand[i]);
      if (fulfilled > bestTotal || hasFairerFulfillment(candidate, bestFulfillment)) {
        bestTotal = fulfilled;
        bestFulfillment = std::move(candidate);
        bestOwners = owners;
      }
    }

    bool search(unsigned index, uint64_t fulfilled) {
      if (report.visits >= report.MaxStates) {exhausted = true; return false;}
      ++report.visits;
      if (!visited.emplace(index, remainingDemand).second) return false;
      considerCandidate(fulfilled);
      if (fulfilled == totalDemand) return true;
      // Equality remains searchable because fairness can still improve.
      if (index == coreOrder.size() || fulfilled +
          std::min(totalDemand - fulfilled, remainingCapacity[index]) < bestTotal)
        return false;
      const auto width = cores[coreOrder[index]].size();
      std::vector<unsigned> choices;
      for (unsigned group = 0; group < remainingDemand.size(); ++group)
        if (remainingDemand[group]) choices.push_back(group);
      std::stable_sort(choices.begin(), choices.end(), [&](unsigned a, unsigned b) {
        return std::min<uint64_t>(remainingDemand[a], width) >
               std::min<uint64_t>(remainingDemand[b], width);
      });
      for (auto group: choices) {
        const unsigned before = remainingDemand[group];
        const auto gain = std::min<uint64_t>(before, width);
        remainingDemand[group] -= gain;
        owners[index] = group;
        const bool complete = search(index + 1, fulfilled + gain);
        remainingDemand[group] = before;
        owners[index] = unowned;
        if (complete) return true;
        if (exhausted) return false;
      }
      return false;
    }

    void run(Pools &pools) {
      using Outcome = ReferenceExecutionPlan::RebalanceReport::Outcome;
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
      ReferenceExecutionPlan::RebalanceReport &report) {
    report.cores = assignedCoreCount;
    bool deficit = false;
    for (const auto &request: requests) {
      deficit |= poolCapacity(pools, request.first) < request.second;
      if (request.second) ++report.groups;
    }
    if (!deficit) return;
    if (assignedCoreCount > report.MaxCores) {
      report.outcome = ReferenceExecutionPlan::RebalanceReport::Outcome::CoreLimit;
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

std::vector<ReferenceExecutionPlan::CPUSet> ReferenceExecutionPlan::physicalPools(const std::vector<unsigned> &ordered,
      const std::vector<CPUSet> &cores) {
  CPUSet allowed(ordered.begin(), ordered.end()), covered;
  std::map<unsigned, unsigned> byCPU;
  std::vector<CPUSet> usableCores;
  for (const auto &core: cores) {
    CPUSet usable;
    for (auto cpu: core) if (allowed.count(cpu)) {
      if (!covered.insert(cpu).second) return {};
      usable.insert(cpu);
    }
    for (auto cpu: usable) byCPU[cpu] = usableCores.size();
    usableCores.push_back(std::move(usable));
  }
  if (covered != allowed) return {};
  std::vector<CPUSet> result;
  covered.clear();
  for (auto cpu: ordered) if (covered.insert(cpu).second) {
    const auto &core = usableCores.at(byCPU.at(cpu));
    result.push_back(core);
    covered.insert(core.begin(), core.end());
  }
  return result;
}

ReferenceExecutionPlan::Pools ReferenceExecutionPlan::partition(const std::map<std::string, unsigned> &requests,
      const std::vector<unsigned> &ordered, const std::vector<CPUSet> &cores,
      bool fillUnused, RebalanceReport *report) {
  RebalanceReport rebalance;
  Pools pools;
  auto availableCores = physicalPools(ordered, cores);
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
  if (!fillUnused) return pools;
  // Phase 3: improve physical spreading without claiming surplus cores. Worker demand
  // stays fixed even when extra logical capacity enters a process pool.
  const auto coreIndex = indexPhysicalCores(availableCores);
  distributeUnusedCores(requests, availableCores, assignedCoreCount, coreIndex, pools);
  return pools;
}

ReferenceExecutionPlan ReferenceExecutionPlan::create(unsigned type, unsigned threads,
      const CPUSet &pool, const std::vector<CPUSet> &cores,
      const std::vector<unsigned> &order) {
  ReferenceExecutionPlan plan;
  plan.logical = pool.size();
  if (!threads || threads > pool.size()) return plan;
  std::vector<unsigned> ordered;
  for (auto cpu: order) if (pool.count(cpu)) ordered.push_back(cpu);
  for (auto cpu: pool)
    if (std::find(ordered.begin(), ordered.end(), cpu) == ordered.end()) ordered.push_back(cpu);
  auto physical = physicalPools(ordered, cores);
  if (physical.empty()) {
    // A missing/partial map cannot justify SMT expansion. Keep the process-
    // only fallback and never attempt worker detection or per-thread pinning.
    for (auto cpu: ordered) {plan.mask.insert(cpu); if (plan.mask.size() == threads) break;}
    return plan;
  }
  plan.physical = physical.size();
  plan.hasSMT = pool.size() > physical.size();
  // Placement policies: a8/a9 expand to the owned pool when SMT is needed;
  // every other core keeps an exactly N-LP mask. Both prefer physical spread.
  const bool fullPoolPolicy = type == CoreA8 || type == CoreA9;
  plan.fullSMT = fullPoolPolicy && plan.hasSMT && threads == pool.size();
  if (fullPoolPolicy && threads > plan.physical) {
    // Once SMT is necessary, expose the complete pool owned by this WU.
    // A matching N-LP mask needlessly constrains OpenMP/OS scheduling.
    plan.mask = pool;
  } else {
    // At/below physical capacity, extra siblings can hurt performance.
    // Select exactly N different physical cores before using any sibling.
    for (const auto &core: physical) {
      plan.mask.insert(*core.begin());
      if (plan.mask.size() == threads) return plan;
    }
    // Non-a8/a9 cores retain matching-size process masks above physical count.
    for (auto cpu: ordered) {plan.mask.insert(cpu); if (plan.mask.size() == threads) break;}
  }
  return plan;
}

}}
