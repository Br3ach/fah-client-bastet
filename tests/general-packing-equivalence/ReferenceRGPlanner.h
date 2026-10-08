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

#include "ReferenceExecutionPlan.h"

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>
#include <utility>

namespace FAH { namespace Client {
  /// Pure desired-allocation planning. No configuration mutation, OS queries,
  /// process ownership, generation updates or logging occur in this interface.
  class ReferenceRGPlanner {
  public:
    using CPUSet = std::set<unsigned>;
    using CPUList = std::vector<unsigned>;
    struct Topology {
      bool hardAffinity = false, homogeneous = false, effectiveClasses = false;
      // Discovery supplies the cause without coupling the planner to OS probes.
      std::string affinityUnavailableReason = "hard CPU affinity is not currently available";
      CPUSet available;
      std::vector<CPUSet> rawPerformanceLevels, performanceLevels;
      std::vector<CPUSet> coreThreads, fastPhysicalCores;
    };
    struct Request {
      std::string name;
      unsigned workers = 0;
      bool classes = false;
      std::vector<uint32_t> classCounts;
      unsigned reservedCores = 0;
      std::set<std::string> gpus;
      // Saved entitlement remains intact; inactive runtime demand owns nothing.
      bool wantsResources = true;
      bool operator==(const Request &other) const {
        return name == other.name && workers == other.workers && classes == other.classes &&
          classCounts == other.classCounts && reservedCores == other.reservedCores &&
          gpus == other.gpus && wantsResources == other.wantsResources;
      }
    };
    struct Result {
      // Diagnostics only for committed pool assignments, not speculative spread trials.
      std::vector<ReferenceExecutionPlan::RebalanceReport> rebalanceReports;
      bool managed = false, runtimeFallback = false;
      std::string runtimeFallbackReason;
      CPUSet allocatable, gpuReservedCPUs;
      std::vector<CPUSet> allocatablePerformanceLevels;
      std::map<std::string, CPUList> allocations;
      std::map<std::string, unsigned> workerBudgets;
      // Zero-reservation GPU masks intentionally share CPU and GPU pools.
      std::map<std::string, std::map<std::string, CPUSet>> gpuDeviceAllocations;
      std::map<std::string, std::map<std::string, std::string>> gpuAllocationShortages;
      bool operator==(const Result &other) const {
        return managed == other.managed && runtimeFallback == other.runtimeFallback &&
          runtimeFallbackReason == other.runtimeFallbackReason && allocatable == other.allocatable &&
          gpuReservedCPUs == other.gpuReservedCPUs && allocatablePerformanceLevels == other.allocatablePerformanceLevels &&
          allocations == other.allocations && workerBudgets == other.workerBudgets &&
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
    // Fail closed on LP overlap, split physical ownership, reserved GPU CPUs
    // or worker budgets exceeding owned pool capacity.
    static bool validate(Result &result, const std::vector<CPUSet> &cores);
    // Final publication also checks exclusive GPU ownership against requests
    // and fast-core topology. Shared masks intentionally may overlap.
    static bool validate(Result &result, const Topology &topology,
      const std::vector<Request> &requests);
  };
}}
