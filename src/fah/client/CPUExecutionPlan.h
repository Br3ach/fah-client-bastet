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
#include <optional>
#include <vector>

namespace FAH { namespace Client {

// The client selects a process pool, never GROMACS/OpenMP worker identities.
// Policy selection requires a usable physical-core map:
// - a8/a9: if N workers fit on P owned physical cores, allow exactly N LPs,
//   one per core. Otherwise allow all L LPs in the owned pool, keeping N workers.
// - Other core types: use a matching-size process mask without a8/a9 expansion.
// - Missing/partial topology: use conservative process-only affinity.
// Affinity capability and fallback decisions belong to the caller.
// The a8/a9 policy follows workload-specific measurements: unnecessary siblings
// hurt below physical capacity, while the full pool helps once SMT is needed.
// Full SMT utilisation (N == L, with SMT present) regressed in testing compared
// with one fewer worker. This motivates the advisory UI warning and launch log.
// Results depend on the workload; never lower N automatically or promise a gain.
struct CPUExecutionPlan {
  static constexpr unsigned CoreA8 = 0xa8;
  static constexpr unsigned CoreA9 = 0xa9;

  using CPUSet = FAH::Client::CPUSet;
  struct SliceExecution;
  // Preserve slice indexes, including empty zero-worker slices. Pools must be
  // disjoint, and known physical cores cannot cross slices. Return nullopt for
  // invalid allocations or no executable mask.
  static std::optional<SliceExecution> createSlices(unsigned type,
    const CPUAllocationSlices &slices, const std::vector<CPUSet> &cores,
    const std::vector<unsigned> &order = {});
  CPUSet mask;
  // Owned-pool capacities; physical is zero when physical coverage is not established.
  unsigned physical = 0;
  unsigned logical = 0;
  bool hasSMT = false;
  bool fullSMT = false;

  // Intersect cores with allowed CPUs and order by first appearance in ordered.
  // Return empty for no CPUs or incomplete/overlapping coverage. This does not
  // require every hardware sibling to be present in the allowed set.
  static std::vector<CPUSet> physicalPools(const std::vector<unsigned> &ordered,
      const std::vector<CPUSet> &cores);

  static CPUExecutionPlan create(unsigned type, unsigned threads,
      const CPUSet &pool, const std::vector<CPUSet> &cores,
      const std::vector<unsigned> &order = {});
};

struct CPUExecutionPlan::SliceExecution {
  CPUExecutionPlan::CPUSet mask;
  std::vector<CPUExecutionPlan> levels;
  // Sum owned-pool capacities; fullSMT means any slice fully uses its SMT pool.
  CPUExecutionPlan summary() const;
  // Selected physical coverage; slices with unknown topology contribute zero.
  unsigned maskPhysical() const;
};

}}
