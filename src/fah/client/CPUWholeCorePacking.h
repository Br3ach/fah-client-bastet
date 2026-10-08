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

#pragma once

#include "CPUTypes.h"

#include "CPUAllocationSlice.h"

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace FAH { namespace Client {
struct CPUWholeCorePacking {
  using CPUSet = FAH::Client::CPUSet;
  using Pools = std::map<std::string, CPUSet>;
  // Search limits retain the baseline or best candidate found;
  // the returned allocation is valid but may not be optimal.
  struct RebalanceReport {
    enum class Outcome {NotNeeded, CoreLimit, StateLimit, Infeasible, Improved, NoImprovement, DepthLimit};
    static constexpr unsigned MaxCores = 128, MaxStates = 50000;
    // Bounds traversal chunks, including virtual chunks for pending consumers.
    static constexpr unsigned MaxSearchDepth = 512;
    Outcome outcome = Outcome::NotNeeded;
    unsigned cores = 0, groups = 0, visits = 0;
    bool improved = false;
    bool operator==(const RebalanceReport &other) const {
      return outcome == other.outcome && cores == other.cores &&
        groups == other.groups && visits == other.visits && improved == other.improved;
    }
    const char *outcomeName() const {
      switch (outcome) {
      case Outcome::CoreLimit: return "core limit skipped";
      case Outcome::DepthLimit: return "search depth limit skipped";
      case Outcome::StateLimit: return "state limit exhausted";
      case Outcome::Infeasible: return "no feasible packing found";
      case Outcome::Improved: return "rebalance succeeded";
      case Outcome::NoImprovement: return "no better packing found";
      default: return "not needed";
      }
    }
  };
  // A request receives zero workers or a count within [minimum, maximum].
  // ownsCores=false consumes worker budget without acquiring CPU ownership.
  struct Request {
    unsigned minimum = 0, maximum = 0;
    bool ownsCores = true;
  };
  // Indexed by input request, then input resource.
  using Layout = std::vector<CPUAllocationSlices>;
  // Returns true when candidate is preferred to baseline.
  // Search uses this to resolve equal-throughput candidates.
  using Prefer = std::function<bool(const std::vector<unsigned> &candidate,
    const std::vector<unsigned> &baseline)>;
  struct Result {Layout allocations; RebalanceReport report;};
  // Resource indices have no scheduling meaning here. The caller supplies
  // request order and tie policy; the engine only enforces resource constraints.
  // Prefer resolves equal-throughput candidates encountered during search.
  // tieTarget requests additional tie search when it beats the baseline;
  // an empty target does not force search when baseline throughput is maximal.
  // Resource pools must be disjoint, with worker budgets no larger than their pools.
  // An empty core map uses singleton CPUs. Within the resource pools, a supplied
  // map must cover every CPU without overlap; no represented core may cross slices.
  static Result pack(const std::vector<Request> &requests,
    const CPUAllocationSlices &resources, const std::vector<CPUSet> &cores,
    const Prefer &prefer, const std::vector<unsigned> &tieTarget = {});
  // Fixed-target compatibility path: preserves greedy, repair, search and
  // spreading order, including reports. Callers retain minimum exclusion and
  // target preparation; fillUnused adds physical spread only up to demand.
  // ordered contains unique CPU IDs. An unusable core map retains the legacy
  // singleton fallback. Requests are traversed in map key order.
  static Pools partition(const std::map<std::string, unsigned> &requests,
    const std::vector<unsigned> &ordered, const std::vector<CPUSet> &cores,
    bool fillUnused = true, RebalanceReport *report = nullptr);
};
}}
