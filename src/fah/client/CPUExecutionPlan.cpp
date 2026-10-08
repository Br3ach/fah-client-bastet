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

#include "CPUExecutionPlan.h"

#include <algorithm>
#include <map>
#include <utility>
#include <optional>

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

CPUExecutionPlan CPUExecutionPlan::create(unsigned type, unsigned threads,
      const CPUSet &pool, const std::vector<CPUSet> &cores,
      const std::vector<unsigned> &order) {
  CPUExecutionPlan plan;
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

CPUExecutionPlan CPUExecutionPlan::SliceExecution::summary() const {
  CPUExecutionPlan plan;
  plan.mask = mask;
  for (const auto &level: levels) {
    plan.physical += level.physical;
    plan.logical += level.logical;
    plan.hasSMT |= level.hasSMT;
    // Any fully occupied SMT slice warrants the advisory.
    plan.fullSMT |= level.fullSMT;
  }
  return plan;
}

unsigned CPUExecutionPlan::SliceExecution::maskPhysical() const {
  // Slices with unknown physical topology contribute zero.
  unsigned count = 0;
  for (const auto &level: levels)
    count += std::min<unsigned>(level.physical, level.mask.size());
  return count;
}

std::optional<CPUExecutionPlan::SliceExecution> CPUExecutionPlan::createSlices(unsigned type,
    const CPUAllocationSlices &slices,
    const std::vector<CPUExecutionPlan::CPUSet> &cores,
    const std::vector<unsigned> &order) {
  SliceExecution result;
  result.levels.resize(slices.size());
  CPUExecutionPlan::CPUSet owned;
  for (unsigned level = 0; level < slices.size(); ++level) {
    const auto &allocation = slices[level];
    if (!allocation.workers) {
      if (!allocation.pool.empty()) return std::nullopt;
      continue;
    }
    for (auto cpu: allocation.pool)
      if (!owned.insert(cpu).second) return std::nullopt;
    auto plan = CPUExecutionPlan::create(type, allocation.workers,
      allocation.pool, cores, order);
    if (plan.mask.size() < allocation.workers) return std::nullopt;
    for (auto cpu: plan.mask)
      if (!allocation.pool.count(cpu) || !result.mask.insert(cpu).second)
        return std::nullopt;
    result.levels[level] = std::move(plan);
  }
  if (result.mask.empty()) return std::nullopt;
  // Known siblings must belong to the same resource slice.
  for (const auto &core: cores) {
    bool owned = false;
    for (const auto &allocation: slices) {
      bool intersects = false;
      for (auto cpu: core)
        if (allocation.pool.count(cpu)) {intersects = true; break;}
      if (!intersects) continue;
      if (owned) return std::nullopt;
      owned = true;
    }
  }
  return result;
}
}}
