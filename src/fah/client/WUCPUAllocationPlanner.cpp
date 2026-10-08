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

#include "WUCPUAllocationPlanner.h"
#include <algorithm>
#include "CPUWholeCorePacking.h"
#include <stdexcept>

using namespace FAH::Client;

WUCPUAllocationPlanner::Result WUCPUAllocationPlanner::plan(
    const std::vector<Request> &requests, unsigned budget,
    const std::vector<unsigned> &cpus,
    const std::vector<CPUWholeCorePacking::CPUSet> &cores) {
  Result result;
  std::set<std::string> excluded;
  bool first = true;
  // Partition sorts string keys in every greedy/repair/spread phase. Use
  // fixed-width ordinal keys there so WU IDs never become scheduling priority.
  // Published results retain the real IDs; request order controls both policies.
  std::map<std::string, std::string> partitionKeys;
  const auto width = std::to_string(requests.size()).size();
  for (size_t i = 0; i < requests.size(); ++i) {
    if (requests[i].minimum > requests[i].maximum)
      throw std::invalid_argument("Request minimum exceeds maximum");

    auto ordinal = std::to_string(i);
    if (!partitionKeys.emplace(requests[i].id,
        std::string(width - ordinal.size(), '0') + ordinal).second)
      throw std::invalid_argument("Duplicate WU request ID");
  }
  // Every failed pass excludes at least one WU, bounding retries by WU count.
  for (;;) {
    unsigned remaining = budget;
    std::map<std::string, unsigned> targets, ownershipTargets;
    for (const auto &request: requests) {
      if (excluded.count(request.id) || remaining < request.minimum) continue;
      unsigned count = std::min(request.maximum, remaining);
      if (!count || count < request.minimum) continue;
      targets[partitionKeys.at(request.id)] = count;
      if (request.ownsCores) ownershipTargets[partitionKeys.at(request.id)] = count;
      remaining -= count;
    }
    // Do not request additional assignments merely because an existing WU
    // cannot meet its minimum on the current physical topology.
    if (first) {result.remaining = remaining; first = false;}
    CPUWholeCorePacking::RebalanceReport report;
    auto pools = CPUWholeCorePacking::partition(
      ownershipTargets, cpus, cores, true, &report);
    // Preserve the first search limit across minimum-recovery passes.
    using Outcome = CPUWholeCorePacking::RebalanceReport::Outcome;
    if (result.report.outcome != Outcome::StateLimit &&
        result.report.outcome != Outcome::CoreLimit &&
        result.report.outcome != Outcome::DepthLimit)
      result.report = report;
    result.pools.clear();
    for (const auto &request: requests) {
      auto pool = pools.find(partitionKeys.at(request.id));
      if (pool != pools.end()) result.pools.emplace(request.id, std::move(pool->second));
    }
    bool retry = false;
    result.workers.clear();
    unsigned unused = budget - remaining;
    for (const auto &request: requests) {
      auto target = targets.find(partitionKeys.at(request.id));
      if (target == targets.end()) continue;
      unsigned count = request.ownsCores ?
        std::min<unsigned>(target->second, result.pools[request.id].size()) : target->second;
      if (count < request.minimum) {excluded.insert(request.id); retry = true;}
      else {result.workers[request.id] = count; unused -= count;}
    }
    if (retry) continue; // Reclaim rejected WUs' whole cores before redistributing.
    for (const auto &request: requests) {
      auto worker = result.workers.find(request.id);
      if (worker == result.workers.end() || !request.ownsCores) continue;
      unsigned limit = std::min<unsigned>(request.maximum, result.pools[request.id].size());
      unsigned extra = std::min(unused, limit - worker->second);
      worker->second += extra; unused -= extra;
    }
    for (const auto &entry: result.workers) {
      auto pool = result.pools.find(entry.first);
      result.allocationSlices[entry.first] = {{entry.second,
        pool == result.pools.end() ? CPUWholeCorePacking::CPUSet{} : pool->second}};
    }
    return result;
  }
}

WUCPUAllocationPlanner::Result WUCPUAllocationPlanner::planClasses(
    const std::vector<Request> &requests, const CPUAllocationSlices &classes,
    const std::vector<CPUWholeCorePacking::CPUSet> &cores) {
  std::vector<CPUWholeCorePacking::Request> constraints;
  std::set<std::string> ids;
  unsigned remaining = 0;
  std::vector<unsigned> tieTarget;
  for (const auto &slice: classes) remaining += slice.workers;
  for (const auto &request: requests) {
    if (!ids.insert(request.id).second) throw std::invalid_argument("Duplicate WU request ID");
    constraints.push_back({request.minimum, request.maximum, request.ownsCores});
    auto target = remaining >= request.minimum ? std::min(remaining, request.maximum) : 0;
    tieTarget.push_back(target); remaining -= target;
  }
  // Class WU ties retain request-vector priority. General exclusion/retry and
  // acquisition accounting above remain unchanged and are not engine policy.
  auto packed = CPUWholeCorePacking::pack(constraints, classes, cores,
    [](const auto &candidate, const auto &baseline) {return candidate > baseline;}, tieTarget);
  Result result; result.remaining = remaining; result.report = packed.report;
  for (unsigned i = 0; i < requests.size(); ++i) {
    unsigned total = 0; CPUWholeCorePacking::CPUSet pool;
    for (const auto &slice: packed.allocations[i]) {
      total += slice.workers; pool.insert(slice.pool.begin(), slice.pool.end());
    }
    if (!total) continue;
    result.workers.emplace(requests[i].id, total);
    if (requests[i].ownsCores) result.pools.emplace(requests[i].id, std::move(pool));
    result.allocationSlices.emplace(requests[i].id, std::move(packed.allocations[i]));
  }
  return result;
}
