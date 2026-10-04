#pragma once
#include <set>
#include <vector>
namespace cb { class SystemInfo { public:
  enum cpu_affinity_capability_t {CPU_AFFINITY_NONE,CPU_AFFINITY_HINT,CPU_AFFINITY_HARD};
  std::set<unsigned> available; std::vector<std::set<unsigned>> levels, cores; cpu_affinity_capability_t cap=CPU_AFFINITY_HARD;
  static SystemInfo& instance(){static SystemInfo s;return s;}
  std::set<unsigned> getAvailableCPUs() const{return available;}
  cpu_affinity_capability_t getCPUAffinityCapability() const{return cap;}
  std::vector<std::set<unsigned>> getCPUPerformanceLevels() const{return levels;}
  std::vector<std::set<unsigned>> getCPUCoreThreads() const{return cores;}
}; }
