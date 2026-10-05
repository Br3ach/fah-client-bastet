/******************************************************************************\

                  This file is part of the Folding@home Client.

          The fah-client runs Folding@home protein folding simulations.
                    Copyright (c) 2001-2026, foldingathome.org
                               All rights reserved.

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
// NativeInherit is reserved for a future FahCore that explicitly supports native
// worker placement within the client's process affinity mask. GROMACS -pin
// inherit is one possible implementation, subject to core/platform validation.
// Do not assume specific MPI/OpenMP settings or a performance benefit.
// No current execution path selects this policy or passes GROMACS arguments.
struct CPUExecutionPlan {
  enum Path {ProcessOnly, LegacyPool, NativeInherit};
  using CPUSet = std::set<unsigned>;
  using Pools = std::map<std::string, CPUSet>;
  Path path = ProcessOnly;
  CPUSet mask;
  unsigned physical = 0;
  unsigned logical = 0;
  bool hasSMT = false;
  bool fullSMT = false;
  bool oversubscribed = false;

  static std::vector<CPUSet> physicalPools(const std::vector<unsigned> &ordered,
      const std::vector<CPUSet> &cores);

  // Assign whole physical cores before deriving any process mask. Requests
  // consume worker capacity, not every LP exposed by an expanded SMT pool.
  // Whole-core granularity can require a runtime reduction even when summed
  // logical requests fit. Never resolve that by sharing a core across CPU WUs.
  static Pools partition(const std::map<std::string, unsigned> &requests,
      const std::vector<unsigned> &ordered, const std::vector<CPUSet> &cores,
      bool fillUnused = true);

  static CPUExecutionPlan create(unsigned type, unsigned threads,
      const CPUSet &pool, const std::vector<CPUSet> &cores,
      const std::vector<unsigned> &order = {});
};

}}
