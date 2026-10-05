/******************************************************************************\

                  This file is part of the Folding@home Client.

          The fah-client runs Folding@home protein folding simulations.
                    Copyright (c) 2001-2026, foldingathome.org
                               All rights reserved.

\******************************************************************************/

#pragma once

#include <cstdint>
#include <map>
#include <set>
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
      typedef std::set<unsigned> CPUSet;
      typedef std::vector<unsigned> CPUList;

    protected:
      bool hardAffinity = false;
      bool homogeneous = false;
      bool configurableClasses = false;
      bool effectiveClasses = false;
      bool managed = false;
      bool runtimeFallback = false;

      uint64_t topologyRefreshCount = 0;
      uint64_t topologyGeneration = 0;
      uint64_t allocationGeneration = 0;

      std::string runtimeFallbackReason;

      CPUSet available;
      CPUSet allocatable;
      CPUSet gpuReservedCPUs;
      std::vector<CPUSet> fastPhysicalCores;
      std::vector<CPUSet> allocatablePerformanceLevels;
      std::vector<CPUSet> rawPerformanceLevels;
      std::vector<CPUSet> performanceLevels; // Raw levels intersected w/ available
      std::vector<CPUSet> coreThreads;

      // Ordered whole-core pools determine CPU-WU ownership. Worker budgets
      // are independent from the logical CPU capacity of those pools.
      std::map<std::string, CPUList> allocations;
      std::map<std::string, std::string> loggedAllocationSummaries;
      std::map<std::string, unsigned> workerBudgets;
      // Positive reservations own exclusive whole cores. Zero-reservation
      // masks intentionally overlap other shared GPUs and CPU-WU pools,
      // but never CPUs held by a positive GPU reservation.
      std::map<std::string, std::map<std::string, CPUSet>> gpuDeviceAllocations;
      std::map<std::string, std::map<std::string, std::string>> gpuAllocationShortages;
      bool validateAllocations();

      void configureGPUAllocations(const Groups &groups);
      void allocateCPUPools(const Groups &groups,
        const std::vector<std::string> &names, bool useClasses);

      CPUList orderCPUs(const CPUSet &cpus) const;
      CPUList orderByPerformance(const std::vector<CPUSet> &levels) const;
    public:
      CPUResources();

      /// Re-read topology from cbang. Returns true when topology/capability
      /// changed. This never mutates persistent resource-group configuration.
      bool refreshTopology(const std::string &reason = "unspecified");

      /// Rebuild exclusive whole-core pools. Worker budgets are distinct from
      /// pool size. Runtime shortages never rewrite saved configuration.
      void update(const Groups &groups,
                  const std::string &reason = "groups-update");

      bool hasHardAffinity() const {return hardAffinity;}
      bool hasPerformanceClasses() const {return configurableClasses;}
      bool hasEffectivePerformanceClasses() const {return effectiveClasses;}
      bool isManaged() const {return managed;}
      bool isRuntimeFallback() const {return runtimeFallback;}
      const std::string &getRuntimeFallbackReason() const
        {return runtimeFallbackReason;}

      uint64_t getTopologyRefreshCount() const {return topologyRefreshCount;}
      uint64_t getTopologyGeneration() const {return topologyGeneration;}
      uint64_t getAllocationGeneration() const {return allocationGeneration;}

      bool supportsGPUAffinity() const {
        return hardAffinity && (effectiveClasses || homogeneous) &&
          !performanceLevels.empty();
      }
      const CPUSet &getGPUCPUs(const std::string &name, const std::string &gpu) const;
      const std::string &getGPUAllocationShortage(
        const std::string &name, const std::string &gpu) const;
      unsigned getGroupWorkerCount(const std::string &name) const;
      const CPUSet &getGPUReservedCPUs() const {return gpuReservedCPUs;}
      const CPUSet &getAllocatableCPUs() const {return allocatable;}
      const std::vector<CPUSet> &getFastPhysicalCores() const
        {return fastPhysicalCores;}
      const std::vector<CPUSet> &getAllocatablePerformanceLevels() const
        {return allocatablePerformanceLevels;}
      const CPUSet &getAvailableCPUs() const {return available;}
      const std::vector<CPUSet> &getRawPerformanceLevels() const
        {return rawPerformanceLevels;}
      const std::vector<CPUSet> &getPerformanceLevels() const
        {return performanceLevels;}
      const std::vector<CPUSet> &getCoreThreads() const {return coreThreads;}

      const CPUList &getGroupCPUs(const std::string &name) const;

      static std::string formatCPUs(const CPUSet &cpus);
      static std::string formatCPUs(const CPUList &cpus);
      static std::string formatCounts(const std::vector<uint32_t> &counts);
    };
  }
}
