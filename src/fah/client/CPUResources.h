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

#include "CPUAllocationPlanner.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>


namespace FAH {
  namespace Client {
    class Groups;


    /// Global logical-CPU topology and resource-group affinity allocator.
    ///
    /// Persistent configuration records user intent. Runtime topology is
    /// ephemeral. This class never rewrites class configuration merely because
    /// current CPU availability cannot satisfy it.
    class CPUResources {
    public:
      using CPUSet = FAH::Client::CPUSet;
      using CPUList = FAH::Client::CPUList;

    protected:
      const bool trustedSingleClassSource;
      explicit CPUResources(bool trustedSingleClassSource);
      bool hardAffinity = false;
      bool homogeneous = false;
      bool configurableClasses = false;
      bool effectiveClasses = false;

      // Topology changes advance topologyGeneration. Changed allocations advance
      // allocationGeneration. Desired/running stamps may differ during stop.
      // See ARCHITECTURE.md for publication, rejection and ownership rules.
      uint64_t topologyRefreshCount = 0;
      uint64_t topologyGeneration = 0;
      uint64_t allocationGeneration = 0;

      CPUSet available;
      std::vector<CPUSet> fastPhysicalCores;
      std::vector<CPUSet> rawPerformanceLevels;
      std::vector<CPUSet> performanceLevels; // Raw levels intersected w/ available
      std::vector<CPUSet> coreThreads;

      // One event-loop publication replaces all desired pools, budgets,
      // reservations, shortages and fallback state without throwing.
      CPUAllocationPlanner::Result allocation;
      std::vector<CPUAllocationPlanner::Request> publishedRequests;
      uint64_t publishedTopologyGeneration = UINT64_MAX;
      std::map<std::string, std::string> loggedAllocationSummaries;
      void logAllocationChanges(const Groups &groups,
        const std::vector<CPUAllocationPlanner::Request> &requests);

    public:
      CPUResources();

      /// Re-read topology from cbang. Returns true when topology/capability
      /// changed. This never mutates persistent resource-group configuration.
      /// Allocation reconciliation is separate; callers must arrange update() when
      /// topology changes.
      bool refreshTopology(const std::string &reason = "unspecified");

      /// Rebuild exclusive whole-core pools. Worker budgets are distinct from
      /// pool size. Runtime shortages never rewrite saved configuration.
      /// Diagnostic exceptions may occur after publication; identical-input
      /// retries do not advance the allocation generation again.
      void update(const Groups &groups,
                  const std::string &reason = "groups-update");

      // Discovery capability, not a guarantee that every affinity call succeeds.
      bool hasHardAffinity() const {return hardAffinity;}
      bool hasPerformanceClasses() const {return configurableClasses;}
      bool hasEffectivePerformanceClasses() const {return effectiveClasses;}
      bool isManaged() const {return allocation.managed;}
      bool isRuntimeFallback() const {return allocation.runtimeFallback;}
      const std::string &getRuntimeFallbackReason() const
        {return allocation.runtimeFallbackReason;}

      uint64_t getTopologyGeneration() const {return topologyGeneration;}
      uint64_t getAllocationGeneration() const {return allocationGeneration;}

      bool supportsGPUAffinity() const {
        return CPUAllocationPlanner::supportsGPUAffinity(
          hardAffinity, homogeneous, effectiveClasses, performanceLevels);
      }
      // Exclusive helpers require complete physical cores; shared affinity does not.
      // Capability does not imply that a particular reservation request fits.
      bool supportsGPUReservation() const {
        return supportsGPUAffinity() && !fastPhysicalCores.empty();
      }
      // Reference-returning getters borrow live state, not immutable snapshots.
      // Copy values needed across refreshes or updates.
      const CPUSet &getGPUCPUs(const std::string &name, const std::string &gpu) const;
      const std::string &getGPUAllocationShortage(
        const std::string &name, const std::string &gpu) const;
      unsigned getGroupWorkerCount(const std::string &name) const;
      const CPUAllocationSlices &getGroupAllocationSlices(const std::string &name) const;
      const CPUSet &getAllocatableCPUs() const {return allocation.allocatable;}
      const std::vector<CPUSet> &getFastPhysicalCores() const
        {return fastPhysicalCores;}
      const CPUSet &getAvailableCPUs() const {return available;}
      const std::vector<CPUSet> &getRawPerformanceLevels() const
        {return rawPerformanceLevels;}
      const std::vector<CPUSet> &getPerformanceLevels() const
        {return performanceLevels;}
      const std::vector<CPUSet> &getCoreThreads() const {return coreThreads;}

      // Count distinct physical cores touched, including partial sibling selections.
      // Zero means an empty mask or at least one CPU missing from the core map.
      unsigned physicalCoreCount(const CPUSet &mask) const;

      const CPUList &getGroupCPUs(const std::string &name) const;

      static std::string formatCPUs(const CPUSet &cpus);
      static std::string formatCPUs(const CPUList &cpus);
    };
  }
}
