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

#include "CPUConfigValidator.h"
#include <map>
#include <sstream>
#include <stdexcept>
using namespace std;
using namespace FAH::Client;
namespace {
using Request = CPUConfigValidator::Request;
using Topology = CPUConfigValidator::Topology;
using Requests = map<string, Request>;
struct Rejection : runtime_error {using runtime_error::runtime_error;};
template<class... Args> [[noreturn]] void reject(const Args &...args) {
  ostringstream message; (message << ... << args); throw Rejection(message.str());
}
struct State {
  vector<uint64_t> classTotals, classCapacity;
  uint64_t total = 0, capacity = 0, reservedCores = 0;
  bool hasClassPolicy = false, cpuIntentChanged = false;
  bool reservationPolicyChanged = false;
  bool gpuSelectionChanged = false, sharedGPURequested = false;
};
// Establish active reservation demand and changes using usable GPU selections.
void analyzeReservations(const Requests &previous, const Requests &candidates, State &state) {
  // GPU reservations and usable device selection are evaluated against the
  // same candidate objects later used for CPU validation.
  for (const auto &entry: candidates) {
    const auto &name = entry.first;
    const auto &candidate = entry.second;
    auto saved = previous.find(name);
    const Request *old = saved == previous.end() ? nullptr : &saved->second;
    auto requested = candidate.reservedCores;
    const auto &selectedGPUs = candidate.gpus;
    bool enabled = !selectedGPUs.empty();
    const auto oldGPUs = old ? old->gpus : std::set<string>();
    // Shared helpers consume no reserved cores, but changing their enabled
    // devices still requires validation against other groups' reservations.
    if (selectedGPUs != oldGPUs) state.gpuSelectionChanged = true;
    uint64_t active = (uint64_t)selectedGPUs.size() * requested;
    state.sharedGPURequested |= enabled && !requested;
    state.reservedCores += active;
    uint64_t oldActive = old ? (uint64_t)oldGPUs.size() * old->reservedCores : 0;
    if (requested != (old ? old->reservedCores : 0) || active != oldActive)
      state.reservationPolicyChanged = true;
    if (requested && !enabled && (!old || old->reservedCores != requested))
      reject("Select an available GPU before reserving CPU cores");
  }
  for (const auto &entry: previous)
    if (!candidates.count(entry.first) && !entry.second.gpus.empty() &&
        entry.second.reservedCores)
      state.reservationPolicyChanged = true;
}

// Derive usable capacities after whole-core GPU reservations; never rewrite requests.
void deriveCapacities(const Topology &topology, State &state) {
  const auto &levels = topology.rawClassCapacity;
  for (auto capacity: levels) state.capacity += capacity;
  state.classCapacity = levels;
  // Reservations use current usable capacity; other validation uses raw capacity.
  if (!state.reservedCores || !topology.gpuAffinity) return;
  state.capacity = topology.available;
  state.classCapacity = topology.effectiveClassCapacity;
  if (state.classCapacity.size() != levels.size() || state.classCapacity.empty())
    reject("Inconsistent GPU reservation topology");
  uint64_t consumed = 0;
  const auto &cores = topology.fastCoreWidths;
  for (size_t i = 0; i < cores.size() && i < state.reservedCores; ++i) {
    const auto width = cores[i];
    if (!width || width > state.capacity - consumed ||
        width > state.classCapacity.front() - consumed)
      reject("Inconsistent GPU reservation topology");
    consumed += width;
  }
  state.capacity -= consumed;
  state.classCapacity.front() -= consumed;
}

// Preserve unchanged stale class intent while checking explicit CPU edits.
void validateCPURequests(const Topology &topology, const Requests &previous,
    const Requests &candidates, State &state) {
  const auto &levels = topology.rawClassCapacity;
  for (const auto &entry: previous)
    if (!candidates.count(entry.first)) {
      const auto &old = entry.second;
      if (old.workers) state.cpuIntentChanged = true;
    }

  // CPU class, total-capacity and change detection share one parsed policy.
  for (const auto &entry: candidates) {
    const auto &name = entry.first;
    const auto &candidate = entry.second;
    auto saved = previous.find(name);
    const Request *old = saved == previous.end() ? nullptr : &saved->second;
    const auto &mode = candidate.mode;
    if (mode != "count" && mode != "classes") {
      reject("Invalid CPU mode '", mode, "' for resource group '", name, "'");
    }

    const auto &proposedCounts = candidate.classCounts;
    // Retain wide validation sums rather than Config's saturated class total.
    uint64_t proposedTotal = mode == "classes" ? 0 : candidate.workers;
    if (mode == "classes")
      for (auto count: proposedCounts) proposedTotal += count;

    const bool unchanged = old && old->mode == mode &&
      (mode == "classes" ? old->classCounts == proposedCounts :
       old->workers == proposedTotal);
    if (!old) {
      if (mode == "classes" || proposedTotal) state.cpuIntentChanged = true;
    } else if (!unchanged) state.cpuIntentChanged = true;

    if (mode == "classes") {
      // Saved class policy constrains validation even with zero demand.
      state.hasClassPolicy = true;
      if (!topology.configurableClasses && !unchanged) {
        reject("CPU performance-class allocation is not currently configurable");
      }

      if (!candidate.hasClassCounts) {
        reject("Class CPU mode requires cpu_class_counts");
      }

      if (!unchanged && proposedCounts.size() != levels.size()) {
        reject("CPU class count does not match current performance-level count");
      }

      for (unsigned i = 0; i < proposedCounts.size(); i++) {
        if (i < state.classTotals.size()) state.classTotals[i] += proposedCounts[i];
        else if (proposedCounts[i] && !unchanged) {
          reject("CPU class configuration references a non-existent level");
        }
      }
    }
    state.total += proposedTotal;
  }
}
// Enforce changed allocation limits, retaining legacy General oversubscription.
void validateTotals(const Topology &topology, const State &state) {
  const auto &levels = topology.rawClassCapacity;
  const bool sharedBlocked = state.sharedGPURequested &&
    !state.classCapacity.empty() && !state.classCapacity.front();
  const char *sharedShortage =
    "GPU reservations leave no Performance 1 CPUs for shared GPU resource "
    "groups; reduce a reservation or reserve cores for each GPU group";
  // Enabling shared helpers must be checked even when the reservation count
  // is unchanged. Do not revalidate unrelated saved CPU intent for this edit.
  if (state.gpuSelectionChanged && state.reservedCores && topology.gpuAffinity &&
      sharedBlocked)
    reject(sharedShortage);

  // Unrelated settings updates do not revalidate or destroy saved CPU intent.
  if (!state.cpuIntentChanged && !state.reservationPolicyChanged) {
    return;
  }

  if (state.reservedCores) {
    if (!topology.gpuAffinity)
      reject("GPU CPU reservation requires hard affinity and Performance 1 topology");
    if (state.reservedCores > topology.fastCoreWidths.size())
      reject("GPU CPU reservations exceed available complete Performance 1 cores");
    if (sharedBlocked)
      reject(sharedShortage);
    if (state.total > state.capacity)
      reject("CPU allocations exceed capacity after GPU CPU reservations");
    for (unsigned i = 0; i < state.classTotals.size(); ++i)
      if (state.classTotals[i] > state.classCapacity[i])
        reject("CPU performance-class allocations overlap GPU CPU reservations");
  }

  // Preserve legacy oversubscription semantics when nobody uses class mode.
  if (!state.hasClassPolicy) {
    return;
  }

  // If class configuration cannot currently be edited, unchanged persisted
  // class policy may survive while other RGs are reset/removed.
  if (!topology.configurableClasses) {
    return;
  }

  if (levels.size() < 2) {
    return;
  }

  for (unsigned i = 0; i < state.classTotals.size(); i++)
    if (state.classTotals[i] > state.classCapacity[i]) {
      reject("Configured CPU performance level ", i + 1, " requests ",
        state.classTotals[i], " logical CPUs but capacity is ", state.classCapacity[i]);
    }

  if (state.total > state.capacity) {
    reject("Configured managed CPU total ", state.total,
      " exceeds CPU topology capacity ", state.capacity);
  }
}
}
CPUConfigValidator::Result CPUConfigValidator::validate(const Topology &topology,
    const vector<Request> &current, const vector<Request> &proposed) {
  Requests previous, candidates;
  State state; state.classTotals.resize(topology.rawClassCapacity.size());
  try {
    for (const auto &request: current)
      if (!previous.emplace(request.name, request).second)
        reject("Duplicate current CPU configuration request name: '", request.name, "'");
    for (const auto &request: proposed)
      if (!candidates.emplace(request.name, request).second)
        reject("Duplicate proposed CPU configuration request name: '", request.name, "'");
    analyzeReservations(previous, candidates, state);
    deriveCapacities(topology, state);
    validateCPURequests(topology, previous, candidates, state);
    validateTotals(topology, state);
    return {true, {}};
  } catch (const Rejection &error) {return {false, error.what()};}
}
