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

#include <map>
#include <set>
#include <string>
#include <vector>

namespace FAH { namespace Client {

// The client selects a process pool, never GROMACS/OpenMP worker identities.
// Policy selection requires a usable physical-core map:
// - a8/a9: if N workers fit on P owned physical cores, allow exactly N LPs,
//   one per core. Otherwise allow all L LPs in the owned pool, keeping N workers.
// - Other core types: use a matching-size process mask without a8/a9 expansion.
// - Missing/partial topology: use conservative process-only affinity.
// Without hard affinity, the scheduler uses the ordinary unpinned launch path.
// The a8/a9 policy follows workload-specific measurements: unnecessary siblings
// hurt below physical capacity, while the full pool helps once SMT is needed.
// Full SMT utilisation (N == L, with SMT present) regressed in testing compared
// with one fewer worker. This motivates the advisory UI warning and launch log.
// Results depend on the workload; never lower N automatically or promise a gain.
//
// A future native policy requires a FahCore that explicitly supports native
// worker placement within the client's process affinity mask. GROMACS -pin
// inherit is one possible implementation, subject to core/platform validation.
// Do not assume specific MPI/OpenMP settings or a performance benefit.
// No current execution path selects this policy or passes GROMACS arguments.
struct ReferenceExecutionPlan {
  static constexpr unsigned CoreA8 = 0xa8;
  static constexpr unsigned CoreA9 = 0xa9;

  using CPUSet = std::set<unsigned>;
  using Pools = std::map<std::string, CPUSet>;
  struct RebalanceReport {
    enum class Outcome {NotNeeded, CoreLimit, StateLimit, Infeasible, Improved};
    static constexpr unsigned MaxCores = 128, MaxStates = 50000;
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
      case Outcome::StateLimit: return "state limit exhausted";
      case Outcome::Infeasible: return "no feasible packing found";
      case Outcome::Improved: return "rebalance succeeded";
      default: return "not needed";
      }
    }
  };
  CPUSet mask;
  unsigned physical = 0;
  unsigned logical = 0;
  bool hasSMT = false;
  bool fullSMT = false;

  static std::vector<CPUSet> physicalPools(const std::vector<unsigned> &ordered,
      const std::vector<CPUSet> &cores);

  // Assign whole physical cores before deriving any process mask. Requests
  // consume worker capacity, not every LP exposed by an expanded SMT pool.
  // fillUnused spreads up to worker demand; surplus physical cores stay unowned.
  // Optional reports distinguish bounded-search aborts from infeasibility.
  // Speculative callers can omit the report; allocation semantics are identical.
  // Whole-core granularity can require a runtime reduction even when summed
  // logical requests fit. Never resolve that by sharing a core across CPU WUs.
  static Pools partition(const std::map<std::string, unsigned> &requests,
      const std::vector<unsigned> &ordered, const std::vector<CPUSet> &cores,
      bool fillUnused = true, RebalanceReport *report = nullptr);

  static ReferenceExecutionPlan create(unsigned type, unsigned threads,
      const CPUSet &pool, const std::vector<CPUSet> &cores,
      const std::vector<unsigned> &order = {});
};

}}
