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

#include "CPUWholeCorePacking.h"
#include "CPUAllocationSlice.h"

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>
#include <utility>

namespace FAH { namespace Client {
  /// Pure desired-allocation planning. No configuration mutation, OS queries,
  /// process ownership, generation updates or logging occur in this interface.
  class CPUAllocationPlanner {
  public:
    using CPUSet = FAH::Client::CPUSet;
    using CPUList = FAH::Client::CPUList;
    struct Topology {
      bool hardAffinity = false, homogeneous = false, effectiveClasses = false;
      // Discovery supplies the cause without coupling the planner to OS probes.
      std::string affinityUnavailableReason = "hard CPU affinity is not currently available";
      CPUSet available;
      // Effective masks in stable performance order; individual levels may be empty.
      std::vector<CPUSet> performanceLevels;
      std::vector<CPUSet> coreThreads;
      // Complete available physical cores belonging to Performance 1.
      std::vector<CPUSet> fastPhysicalCores;
    };
    struct Request {
      std::string name;
      // Configured worker total; Classes mode uses the class-count total,
      // capped at UINT32_MAX by Config.
      unsigned workers = 0;
      bool classes = false;
      std::vector<uint32_t> classCounts;
      unsigned reservedCores = 0;
      std::set<std::string> gpus;
      // False suppresses runtime demand; plan() leaves the supplied request unchanged.
      bool wantsResources = true;
      bool operator==(const Request &other) const {
        return name == other.name && workers == other.workers && classes == other.classes &&
          classCounts == other.classCounts && reservedCores == other.reservedCores &&
          gpus == other.gpus && wantsResources == other.wantsResources;
      }
    };
    struct Result {
      // Diagnostics only for committed pool assignments, not speculative spread trials.
      std::vector<CPUWholeCorePacking::RebalanceReport> rebalanceReports;
      bool managed = false, runtimeFallback = false;
      std::string runtimeFallbackReason;
      CPUSet allocatable, gpuReservedCPUs;
      std::vector<CPUSet> allocatablePerformanceLevels;
      // Ordered desired ownership pools, not the masks of launched processes.
      std::map<std::string, CPUList> allocations;
      std::map<std::string, unsigned> workerBudgets;
      // Class-indexed fulfilled budgets and whole-core pools; aggregates are derived.
      std::map<std::string, CPUAllocationSlices> allocationSlices;
      // Zero-reservation GPU masks intentionally share CPU and GPU pools.
      std::map<std::string, std::map<std::string, CPUSet>> gpuDeviceAllocations;
      std::map<std::string, std::map<std::string, std::string>> gpuAllocationShortages;
      // Compare published allocation state; per-replan rebalance reports are excluded.
      bool operator==(const Result &other) const {
        return managed == other.managed && runtimeFallback == other.runtimeFallback &&
          runtimeFallbackReason == other.runtimeFallbackReason && allocatable == other.allocatable &&
          gpuReservedCPUs == other.gpuReservedCPUs && allocatablePerformanceLevels == other.allocatablePerformanceLevels &&
          allocations == other.allocations && workerBudgets == other.workerBudgets &&
          allocationSlices == other.allocationSlices &&
          gpuDeviceAllocations == other.gpuDeviceAllocations && gpuAllocationShortages == other.gpuAllocationShortages;
      }
      // Container swaps do not allocate. Generic Result move/swap can allocate
      // map sentinels on MSVC, so publish through explicit member swaps.
      void swap(Result &other) noexcept {
        rebalanceReports.swap(other.rebalanceReports);
        std::swap(managed, other.managed);
        std::swap(runtimeFallback, other.runtimeFallback);
        runtimeFallbackReason.swap(other.runtimeFallbackReason);
        allocatable.swap(other.allocatable);
        gpuReservedCPUs.swap(other.gpuReservedCPUs);
        allocatablePerformanceLevels.swap(other.allocatablePerformanceLevels);
        allocations.swap(other.allocations);
        workerBudgets.swap(other.workerBudgets);
        allocationSlices.swap(other.allocationSlices);
        gpuDeviceAllocations.swap(other.gpuDeviceAllocations);
        gpuAllocationShortages.swap(other.gpuAllocationShortages);
      }

    };
    // Shared helpers need a usable performance mask, not physical-core topology.
    static bool supportsGPUAffinity(bool hard, bool homogeneous, bool classes,
        const std::vector<CPUSet> &levels) {
      return hard && (classes || homogeneous) && !levels.empty();
    }
    // Topology is validated by discovery before planning. Requests have unique
    // names (duplicates throw std::invalid_argument before allocation).
    // GPU reservations use lexical RG names, then lexical GPU IDs.
    // The planner canonicalizes input order. CPU ties also use lexical RG names.
    static Result plan(const Topology &topology, const std::vector<Request> &requests);
    static CPUList orderCPUs(const CPUSet &cpus, const std::vector<CPUSet> &cores);
    // CPU-only checks: ownership, capacity and slice/aggregate consistency.
    // On failure, clears CPU allocations, budgets and slices.
    static bool validate(Result &result, const std::vector<CPUSet> &cores);
    // Complete publication validation adds request bounds, class policy and
    // GPU eligibility/ownership. On failure, clears CPU and GPU launch masks.
    static bool validate(Result &result, const Topology &topology,
      const std::vector<Request> &requests);
  };
}}
