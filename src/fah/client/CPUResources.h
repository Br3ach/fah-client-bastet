/******************************************************************************\

                  This file is part of the Folding@home Client.

          The fah-client runs Folding@home protein folding simulations.
                    Copyright (c) 2001-2026, foldingathome.org
                               All rights reserved.

\******************************************************************************/

#pragma once

#include <cstdint>
#include <chrono>
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
      std::vector<CPUSet> rawPerformanceLevels;
      std::vector<CPUSet> performanceLevels; // Raw levels intersected w/ available
      std::vector<CPUSet> coreThreads;

      // Ordered masks determine CPU-WU placement; GPU helpers do not use this pool.
      std::map<std::string, CPUList> allocations;
      std::map<std::string, std::string> loggedAllocationSummaries;
      bool smtSearchLimitLogged = false;
      std::chrono::steady_clock::time_point lastSMTSearchLimitLog;

      static constexpr unsigned SMT_SEARCH_LIMIT = 50000;
      // Pure reservation checker. A bounded-search decline is a preference,
      // not a prohibition on using the candidate in later allocation passes.
      static bool partitionDeficits(std::vector<unsigned> capacities,
                                    const std::vector<unsigned> &deficits,
                                    unsigned &visits);
      bool validateAllocations();

      bool shouldLogSMTSearchLimit(std::chrono::steady_clock::time_point now);


      CPUList orderCPUs(const CPUSet &cpus) const;
      CPUList orderByPerformance(const std::vector<CPUSet> &levels) const;
      void distribute(const std::vector<std::string> &names,
                      const CPUList &pool,
                      std::map<std::string, uint32_t> &requested);
      void allocateGeneral(const Groups &groups,
                           const std::vector<std::string> &names,
                           const CPUList &pool);

    public:
      CPUResources();

      /// Re-read topology from cbang. Returns true when topology/capability
      /// changed. This never mutates persistent resource-group configuration.
      bool refreshTopology(const std::string &reason = "unspecified");

      /// Rebuild disjoint runtime resource-group allocations from current
      /// configuration. Existing valid masks are preserved whenever possible.
      /// If configured class intent cannot currently be satisfied, all class-
      /// mode RGs temporarily participate as general count-mode requests for
      /// this allocator epoch and a warning is logged.
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
