/******************************************************************************\

                  This file is part of the Folding@home Client.

          The fah-client runs Folding@home protein folding simulations.
                    Copyright (c) 2001-2026, foldingathome.org
                               All rights reserved.

\******************************************************************************/

#include "CPUExecutionPlan.h"

#include <algorithm>
#include <utility>

namespace FAH { namespace Client {

std::vector<CPUExecutionPlan::CPUSet> CPUExecutionPlan::physicalPools(const std::vector<unsigned> &ordered,
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

CPUExecutionPlan::Pools CPUExecutionPlan::partition(const std::map<std::string, unsigned> &requests,
      const std::vector<unsigned> &ordered, const std::vector<CPUSet> &cores,
      bool fillUnused) {
  Pools pools;
  auto free = physicalPools(ordered, cores);
  if (free.empty()) { // Unknown topology retains deterministic logical pools.
    for (auto cpu: ordered) free.push_back({cpu});
  }
  unsigned next = 0;
  auto need = requests;
  while (next < free.size()) {
    bool progress = false;
    for (auto &entry: need) if (entry.second && next < free.size()) {
      const auto &core = free[next++];
      pools[entry.first].insert(core.begin(), core.end());
      // The deficit counts workers, not physical cores. A whole SMT core
      // supplies up to core.size() worker slots without splitting ownership.
      // Later spreading increases physical coverage without raising workers.
      entry.second -= std::min<unsigned>(entry.second, core.size());
      progress = true;
    }
    if (!progress) break;
  }
  // Greedy whole-core assignment can overfill a small request on mixed-SMT
  // hardware. Repair deficits by transfers or width-reducing swaps. Every
  // operation increases fulfilled worker demand and preserves whole cores.
  bool repaired = true;
  while (repaired) {
    repaired = false;
    for (const auto &recipient: requests) {
      auto &target = pools[recipient.first];
      if (target.size() >= recipient.second) continue;
      for (const auto &donor: requests) {
        auto &source = pools[donor.first];
        if (donor.first == recipient.first || source.size() <= donor.second) continue;
        for (unsigned i = 0; i < next && !repaired; ++i) {
          const auto &large = free[i];
          if (!source.count(*large.begin())) continue;
          CPUSet small;
          if (source.size() - large.size() < donor.second) {
            for (unsigned j = 0; j < next; ++j)
              if (target.count(*free[j].begin()) && free[j].size() < large.size() &&
                  source.size() - large.size() + free[j].size() >= donor.second) {
                small = free[j]; break;
              }
            if (small.empty()) continue;
          }
          for (auto cpu: large) source.erase(cpu);
          for (auto cpu: small) target.erase(cpu);
          source.insert(small.begin(), small.end());
          target.insert(large.begin(), large.end());
          repaired = true;
        }
        if (repaired) break;
      }
      if (repaired) break;
    }
  }
  if (!fillUnused) return pools;
  // With spare capacity, improve physical spreading before widening pools.
  for (bool spread: {true, false}) while (next < free.size()) {
    bool progress = false;
    for (const auto &entry: requests) if (entry.second && next < free.size()) {
      unsigned physical = 0;
      for (const auto &core: cores)
        for (auto cpu: core) if (pools[entry.first].count(cpu)) {++physical; break;}
      if (spread && physical >= entry.second) continue;
      const auto &core = free[next++];
      pools[entry.first].insert(core.begin(), core.end());
      progress = true;
    }
    if (!progress) break;
  }
  return pools;
}

CPUExecutionPlan CPUExecutionPlan::create(unsigned type, unsigned threads,
      const CPUSet &pool, const std::vector<CPUSet> &cores,
      const std::vector<unsigned> &order) {
  CPUExecutionPlan plan;
  plan.logical = pool.size();
  plan.oversubscribed = threads > pool.size();
  if (!threads || plan.oversubscribed) return plan;
  std::vector<unsigned> ordered;
  for (auto cpu: order) if (pool.count(cpu)) ordered.push_back(cpu);
  for (auto cpu: pool)
    if (std::find(ordered.begin(), ordered.end(), cpu) == ordered.end()) ordered.push_back(cpu);
  auto physical = physicalPools(ordered, cores);
  if (physical.empty()) {
    // A missing/partial map cannot justify SMT expansion. Keep the process-
    // only fallback and never attempt worker detection or per-thread pinning.
    for (auto cpu: pool) {plan.mask.insert(cpu); if (plan.mask.size() == threads) break;}
    return plan;
  }
  plan.physical = physical.size();
  plan.hasSMT = pool.size() > physical.size();
  const bool legacy = type == 0xa8 || type == 0xa9;
  if (legacy) plan.path = LegacyPool;
  plan.fullSMT = legacy && plan.hasSMT && threads == pool.size();
  if (legacy && threads > plan.physical) {
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
    for (auto cpu: pool) {plan.mask.insert(cpu); if (plan.mask.size() == threads) break;}
  }
  return plan;
}

}}
