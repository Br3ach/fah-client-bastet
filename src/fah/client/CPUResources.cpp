/******************************************************************************\

                  This file is part of the Folding@home Client.

          The fah-client runs Folding@home protein folding simulations.
                    Copyright (c) 2001-2026, foldingathome.org
                               All rights reserved.

\******************************************************************************/

#include "CPUResources.h"
#include "Groups.h"
#include "Group.h"
#include "Config.h"

#include <cbang/log/Logger.h>
#include <cbang/os/SystemInfo.h>

#include <algorithm>
#include <map>
#include <functional>
#include <sstream>
#include <utility>


using namespace std;
using namespace cb;
using namespace FAH::Client;


namespace {
  CPUResources::CPUSet intersect(const CPUResources::CPUSet &a,
                                 const CPUResources::CPUSet &b) {
    CPUResources::CPUSet result;
    set_intersection(a.begin(), a.end(), b.begin(), b.end(),
                     inserter(result, result.end()));
    return result;
  }


  template <class C>
  string joinValues(const C &values) {
    ostringstream out;
    out << '[';
    bool first = true;
    for (auto value: values) {
      if (!first) out << ',';
      first = false;
      out << value;
    }
    out << ']';
    return out.str();
  }


  bool containsAll(const CPUResources::CPUSet &haystack,
                   const CPUResources::CPUList &needles) {
    for (auto cpu: needles)
      if (!haystack.count(cpu)) return false;
    return true;
  }


  CPUResources::CPUList without(const CPUResources::CPUList &cpus,
                                const CPUResources::CPUSet &remove) {
    CPUResources::CPUList result;
    for (auto cpu: cpus)
      if (!remove.count(cpu)) result.push_back(cpu);
    return result;
  }
}


string CPUResources::formatCPUs(const CPUSet &cpus) {return joinValues(cpus);}
string CPUResources::formatCPUs(const CPUList &cpus) {return joinValues(cpus);}
string CPUResources::formatCounts(const vector<uint32_t> &counts) {
  return joinValues(counts);
}


CPUResources::CPUResources() {refreshTopology("startup");}


bool CPUResources::refreshTopology(const string &reason) {
  topologyRefreshCount++;
  auto &sys = SystemInfo::instance();

  LOG_DEBUG(1, "CPU topology refresh #" << topologyRefreshCount
    << " requested: reason=" << reason);

  CPUSet newAvailable = sys.getAvailableCPUs();
  bool newHard =
    sys.getCPUAffinityCapability() == SystemInfo::CPU_AFFINITY_HARD;

  // An empty availability set means cbang could not represent it reliably.
  if (newAvailable.empty()) newHard = false;

  // Keep raw classes separate from current process availability. Persistent
  // class configuration is validated against raw topology, not a temporary
  // cpuset/process restriction.
  auto newRawLevels = sys.getCPUPerformanceLevels();
  CPUSet rawCovered;
  bool validRawLevels = newRawLevels.size() > 1;

  for (auto &raw: newRawLevels) {
    if (raw.empty()) validRawLevels = false;
    for (auto cpu: raw)
      if (!rawCovered.insert(cpu).second) validRawLevels = false;
  }

  // Effective classes are what this process can use right now.
  vector<CPUSet> newLevels;
  CPUSet effectiveCovered;
  bool validEffectiveLevels = validRawLevels;

  for (auto &raw: newRawLevels) {
    CPUSet level = intersect(raw, newAvailable);
    newLevels.push_back(level); // Preserve indexes even when currently empty.
    for (auto cpu: level)
      if (!effectiveCovered.insert(cpu).second) validEffectiveLevels = false;
  }

  if (effectiveCovered != newAvailable) validEffectiveLevels = false;

  // SMT/core topology is optional. If invalid after intersecting with the
  // current available mask, disable only the SMT preference, not affinity.
  vector<CPUSet> newCores;
  CPUSet coreCovered;
  bool validCores = true;

  for (auto &raw: sys.getCPUCoreThreads()) {
    CPUSet core = intersect(raw, newAvailable);
    if (core.empty()) continue;

    for (auto cpu: core)
      if (!coreCovered.insert(cpu).second) validCores = false;
    newCores.push_back(core);
  }

  if (coreCovered != newAvailable) validCores = false;
  if (!validCores) newCores.clear();

  // cbang reports one complete class for homogeneous topology and an empty
  // map for unknown classification. Require a valid physical-core map too.
  bool newHomogeneous = newHard && !newCores.empty() &&
    newRawLevels.size() == 1 && effectiveCovered == newAvailable;
  bool newConfigurable = newHard && validRawLevels;
  bool newEffective = newHard && validEffectiveLevels;
  bool changed = hardAffinity != newHard || homogeneous != newHomogeneous ||
    configurableClasses != newConfigurable || effectiveClasses != newEffective ||
    available != newAvailable ||
    rawPerformanceLevels != newRawLevels ||
    performanceLevels != newLevels ||
    coreThreads != newCores;

  if (changed) {
    LOG_DEBUG(1, "CPU topology previous: generation=" << topologyGeneration
      << " hard-affinity=" << hardAffinity
      << " configurable-classes=" << configurableClasses
      << " effective-classes=" << effectiveClasses
      << " available=" << formatCPUs(available));
  }

  hardAffinity = newHard;
  homogeneous = newHomogeneous;
  configurableClasses = newConfigurable;
  effectiveClasses = newEffective;
  available.swap(newAvailable);
  rawPerformanceLevels.swap(newRawLevels);
  performanceLevels.swap(newLevels);
  coreThreads.swap(newCores);

  if (changed) topologyGeneration++;

  LOG_DEBUG(1, "CPU topology current: generation=" << topologyGeneration
    << " changed=" << changed
    << " hard-affinity=" << hardAffinity
    << " configurable-classes=" << configurableClasses
    << " effective-classes=" << effectiveClasses
    << " available=" << formatCPUs(available));

  for (unsigned i = 0; i < rawPerformanceLevels.size(); i++) {
    LOG_DEBUG(1, "CPU topology class " << i
      << ": raw=" << formatCPUs(rawPerformanceLevels[i])
      << " raw-count=" << rawPerformanceLevels[i].size()
      << " effective=" << formatCPUs(performanceLevels[i])
      << " effective-count=" << performanceLevels[i].size());
  }

  if (!validRawLevels && !rawPerformanceLevels.empty())
    LOG_DEBUG(1, "CPU topology: raw performance-class map is not usable for "
      "class configuration");

  if (!validEffectiveLevels && validRawLevels)
    LOG_DEBUG(1, "CPU topology: effective CPU set does not currently form a "
      "complete performance-class map");

  if (coreThreads.empty())
    LOG_DEBUG(1, "CPU topology: SMT/core sibling map unavailable; allocator "
      "will use deterministic logical-CPU ordering");
  else
    for (unsigned i = 0; i < coreThreads.size(); i++)
      LOG_DEBUG(2, "CPU topology physical core " << i
        << ": threads=" << formatCPUs(coreThreads[i]));

  if (changed)
    LOG_INFO(3, "CPU topology/capability changed (generation "
      << topologyGeneration << "); saved CPU-class configuration is unchanged");
  else
    LOG_DEBUG(2, "CPU topology refresh #" << topologyRefreshCount
      << " completed with no change");

  return changed;
}


CPUResources::CPUList CPUResources::orderCPUs(const CPUSet &cpus) const {
  CPUList result;
  if (cpus.empty()) return result;

  if (coreThreads.empty()) {
    result.insert(result.end(), cpus.begin(), cpus.end());
    return result;
  }

  CPUSet emitted;
  vector<CPUList> siblings;

  // Pass 1: one logical CPU from every physical core represented in the set.
  for (auto &core: coreThreads) {
    CPUList inCore;
    for (auto cpu: core)
      if (cpus.count(cpu)) inCore.push_back(cpu);
    if (inCore.empty()) continue;

    sort(inCore.begin(), inCore.end());
    result.push_back(inCore.front());
    emitted.insert(inCore.front());

    if (1 < inCore.size())
      siblings.push_back(CPUList(inCore.begin() + 1, inCore.end()));
  }

  // Defensive fallback for a topology transition. Do not lose a usable CPU
  // because the optional core map changed between discovery and allocation.
  for (auto cpu: cpus)
    if (!emitted.count(cpu)) {
      bool isSibling = false;
      for (auto &list: siblings)
        if (find(list.begin(), list.end(), cpu) != list.end()) {
          isSibling = true;
          break;
        }
      if (!isSibling) result.push_back(cpu);
    }

  // Pass 2: SMT siblings only after every physical core got one thread.
  for (auto &list: siblings)
    for (auto cpu: list) result.push_back(cpu);

  return result;
}


CPUResources::CPUList CPUResources::orderByPerformance(
    const vector<CPUSet> &levels) const {
  CPUList result;
  CPUSet emitted;

  for (auto &level: levels)
    for (auto cpu: orderCPUs(level))
      if (emitted.insert(cpu).second) result.push_back(cpu);

  // If the current class map is incomplete, preserve correctness by appending
  // any usable CPUs not represented by a performance class.
  for (auto cpu: orderCPUs(available))
    if (emitted.insert(cpu).second) result.push_back(cpu);

  return result;
}


bool CPUResources::shouldLogSMTSearchLimit(chrono::steady_clock::time_point now) {
  if (smtSearchLimitLogged && now - lastSMTSearchLimitLog < chrono::minutes(5))
    return false;
  smtSearchLimitLogged = true;
  lastSMTSearchLimitLog = now;
  return true;
}


bool CPUResources::validateAllocations() {
  CPUSet seen;
  for (const auto &entry: allocations)
    for (auto cpu: entry.second)
      if (!seen.insert(cpu).second) {
        // Keep managed mode with empty masks: CPU work must wait, rather than
        // silently reverting to unrestricted legacy launches. GPU work is shared.
        allocations.clear();
        runtimeFallback = true;
        runtimeFallbackReason = "internal CPU allocation overlap; CPU folding suspended";
        LOG_ERROR(runtimeFallbackReason << " (logical CPU " << cpu << ')');
        return false;
      }
  return true;
}


bool CPUResources::partitionDeficits(vector<unsigned> capacities,
                                     const vector<unsigned> &deficits,
                                     unsigned &visits) {
  visits = 0;
  if (deficits.empty()) return true;
  if (capacities.empty()) return false;
  sort(capacities.rbegin(), capacities.rend());
  // Uniform SMT width needs only a core-count calculation.
  if (capacities.front() == capacities.back()) {
    unsigned required = 0;
    for (auto deficit: deficits)
      required += (deficit + capacities.front() - 1) / capacities.front();
    return required <= capacities.size();
  }
  // Mixed widths need actual capacity partitioning; using only the
  // largest width can accept an impossible reservation. Bound search
  // work and conservatively decline a candidate if that bound is hit.
  set<pair<unsigned, vector<unsigned>>> failed;
  function<bool(unsigned, vector<unsigned>)> fits =
    [&] (unsigned index, vector<unsigned> need) {
      sort(need.rbegin(), need.rend());
      while (!need.empty() && !need.back()) need.pop_back();
      if (need.empty()) return true;
      if (index == capacities.size() || ++visits > SMT_SEARCH_LIMIT) return false;
      unsigned demandLeft = 0, supplyLeft = 0;
      for (auto n: need) demandLeft += n;
      for (unsigned i = index; i < capacities.size(); i++) supplyLeft += capacities[i];
      if (demandLeft > supplyLeft) return false;
      auto state = make_pair(index, need);
      if (failed.count(state)) return false;
      for (unsigned i = 0; i < need.size(); i++) {
        if (i && need[i] == need[i - 1]) continue;
        auto next = need;
        next[i] -= min(next[i], capacities[index]);
        if (fits(index + 1, next)) return true;
      }
      if (fits(index + 1, need)) return true;
      failed.insert(state);
      return false;
    };
  return fits(0, deficits);
}


// Retain physical-core ownership across the first-thread and sibling passes.
// The ordered pool still determines class preference and deterministic ties.
void CPUResources::distribute(const vector<string> &names, const CPUList &pool,
                              map<string, uint32_t> &requested) {
  map<unsigned, unsigned> physical;
  for (unsigned i = 0; i < coreThreads.size(); i++)
    for (auto cpu: coreThreads[i]) physical[cpu] = i;
  // Unknown SMT topology uses independent singleton IDs, never an early
  // success that bypasses reservation for the rest of the pool.
  unsigned nextCore = coreThreads.size();
  for (auto cpu: available)
    if (!physical.count(cpu)) physical[cpu] = nextCore++;
  map<unsigned, set<string>> owners;
  CPUSet used;
  for (auto &entry: allocations)
    for (auto cpu: entry.second) {
      used.insert(cpu);
      auto it = physical.find(cpu);
      if (it != physical.end()) owners[it->second].insert(entry.first);
    }
  CPUList freePool = without(pool, used);
  bool progress = true;
  while (!freePool.empty() && progress) {
    progress = false;
    for (auto &name: names) {
      if (!requested[name] || freePool.empty()) continue;
      auto rank = [&] (unsigned cpu) {
        for (unsigned i = 0; i < performanceLevels.size(); i++)
          if (performanceLevels[i].count(cpu)) return i;
        return (unsigned)performanceLevels.size();
      };
      unsigned preferred = rank(freePool.front());
      auto freshFits = [&] (unsigned candidate) {
        map<unsigned, unsigned> freshCapacity;
        map<string, unsigned> privateCapacity;
        unsigned preferredCapacity = 0;
        for (auto cpu: freePool) {
          if (rank(cpu) != preferred) continue;
          preferredCapacity++;
          auto core = physical.find(cpu);
          if (core == physical.end()) continue;
          auto &owner = owners[core->second];
          if (owner.empty()) freshCapacity[core->second]++;
          else if (owner.size() == 1) privateCapacity[*owner.begin()]++;
        }
        uint64_t demand = 0;
        for (auto &entry: requested) demand += entry.second;
        // General allocations may use slower classes after this class fills.
        if (demand > preferredCapacity) return true;
        auto core = physical.find(candidate);
        if (core == physical.end()) return true;
        auto candidateCapacity = freshCapacity[core->second];
        freshCapacity.erase(core->second);
        privateCapacity[name] += candidateCapacity ? candidateCapacity - 1 : 0;
        vector<unsigned> deficits, capacities;
        for (auto &entry: requested) {
          unsigned target = entry.second - (entry.first == name ? 1 : 0);
          if (target > privateCapacity[entry.first])
            deficits.push_back(target - privateCapacity[entry.first]);
        }
        for (auto &entry: freshCapacity) capacities.push_back(entry.second);
        unsigned visits = 0;
        bool result = partitionDeficits(std::move(capacities), deficits, visits);
        if (visits > SMT_SEARCH_LIMIT && shouldLogSMTSearchLimit(chrono::steady_clock::now()))
          LOG_INFO(3, "SMT capacity reservation search limit reached (50000 visits); "
            "declining preferred fresh CPU " << candidate
            << "; later allocation passes may still use it. "
            << "Further search-limit messages suppressed for 300s");
        return result;
      };
      auto chosen = freePool.end();
      // Prefer a fresh physical core. Once SMT is needed, retain its owner.
      for (auto it = freePool.begin(); it != freePool.end(); ++it) {
        if (rank(*it) != preferred) continue;
        auto core = physical.find(*it);
        if ((core == physical.end() || owners[core->second].empty()) && freshFits(*it)) {
          chosen = it;
          break;
        }
      }
      if (chosen == freePool.end())
        for (auto it = freePool.begin(); it != freePool.end(); ++it) {
          if (rank(*it) != preferred) continue;
          auto core = physical.find(*it);
          if (core != physical.end() && owners[core->second] == set<string>{name}) {
            chosen = it;
            break;
          }
        }
      // General pools can include slower classes. Avoid splitting another
      // RG's physical core when an allowed fresh/owned CPU is available there.
      // Explicit class pools contain only the requested class.
      if (chosen == freePool.end())
        for (auto it = freePool.begin(); it != freePool.end(); ++it) {
          auto core = physical.find(*it);
          if (core != physical.end() && owners[core->second].empty() &&
              (rank(*it) != preferred || freshFits(*it))) {
            chosen = it;
            break;
          }
        }
      if (chosen == freePool.end())
        for (auto it = freePool.begin(); it != freePool.end(); ++it) {
          auto core = physical.find(*it);
          if (core != physical.end() && owners[core->second] == set<string>{name}) {
            chosen = it;
            break;
          }
        }
      if (chosen == freePool.end()) {
        // Do not consume another RG's sibling while its owner still needs it.
        for (auto it = freePool.begin(); it != freePool.end(); ++it) {
          if (rank(*it) != preferred) continue;
          auto core = physical.find(*it);
          bool reserved = false;
          if (core != physical.end())
            for (auto &owner: owners[core->second])
              if (owner != name && requested[owner]) reserved = true;
          if (!reserved) {chosen = it; break;}
        }
      }
      if (chosen == freePool.end()) continue;
      unsigned cpu = *chosen;
      allocations[name].push_back(cpu);
      auto core = physical.find(cpu);
      if (core != physical.end()) {
        if (!owners[core->second].empty() && !owners[core->second].count(name))
          LOG_DEBUG(1, "CPU sibling split selected for RG '" << name
            << "' CPU=" << cpu);
        owners[core->second].insert(name);
      }
      freePool.erase(chosen);
      requested[name]--;
      progress = true;
    }
  }
}


void CPUResources::allocateGeneral(const Groups &groups,
                                   const vector<string> &names,
                                   const CPUList &pool) {
  map<string, uint32_t> requested;
  CPUSet alreadyAllocated;

  for (auto &p: allocations)
    for (auto cpu: p.second) alreadyAllocated.insert(cpu);

  CPUList freePool = without(pool, alreadyAllocated);

  for (auto &name: names) {
    auto &config = groups.getGroup(name).getConfig();
    if (config.usesCPUClasses()) continue;
    uint32_t target = config.getCPUs();

    uint32_t have = allocations[name].size();
    if (target > have) requested[name] = target - have;
  }

  distribute(names, freePool, requested);

  for (auto &p: requested)
    if (p.second)
      LOG_WARNING("CPU runtime allocation is short by " << p.second
        << " CPUs for resource group '" << (p.first.empty() ? "Default" : p.first)
        << "'");
}


void CPUResources::update(const Groups &groups, const string &reason) {
  allocationGeneration++;
  auto oldAllocations = allocations;
  allocations.clear();
  managed = false;
  const bool previousFallback = runtimeFallback;
  const string previousFallbackReason = runtimeFallbackReason;
  runtimeFallback = false;
  runtimeFallbackReason.clear();

  vector<string> names;
  for (auto &name: groups.keys()) names.push_back(name);
  sort(names.begin(), names.end());

  // Informational summaries remain available in release builds. Publish only
  // changed RG allocations/policies, including transitions to OS scheduling.
  auto logAllocations = [&] {
    map<string, string> summaries;
    for (auto &name: names) {
      const auto &mask = getGroupCPUs(name);
      CPUSet cpus(mask.begin(), mask.end());
      ostringstream out;
      out << "managed=" << managed << " fallback=" << runtimeFallback
          << " logical-mask=" << formatCPUs(mask);
      for (unsigned i = 0; i < rawPerformanceLevels.size(); ++i)
        out << " class" << i + 1 << '='
            << formatCPUs(intersect(cpus, rawPerformanceLevels[i]));
      out << " policy={" << groups.getGroup(name).getConfig().getCPUConfigDescription()
          << '}';
      if (!managed) out << " (OS-scheduled; no managed RG mask)";
      if (runtimeFallback) out << " reason=" << runtimeFallbackReason;
      summaries[name] = out.str();
      auto previous = loggedAllocationSummaries.find(name);
      if (previous == loggedAllocationSummaries.end() || previous->second != out.str())
        LOG_INFO(3, "CPU allocation RG '" << (name.empty() ? "Default" : name)
          << "': " << out.str());
    }
    loggedAllocationSummaries.swap(summaries);
  };


  LOG_DEBUG(1, "CPU allocation generation " << allocationGeneration
    << " begin: reason=" << reason << " topology-generation="
    << topologyGeneration << " available=" << formatCPUs(available));

  for (auto &name: names) {
    auto it = oldAllocations.find(name);
    LOG_DEBUG(2, "CPU allocation previous RG '"
      << (name.empty() ? "Default" : name) << "': "
      << (it == oldAllocations.end() ? "[]" : formatCPUs(it->second)));
  }

  bool anyClass = false;
  uint64_t configuredTotal = 0;
  vector<uint64_t> classRequested(rawPerformanceLevels.size(), 0);
  bool compatibleCounts = true;

  for (auto &name: names) {
    auto &config = groups.getGroup(name).getConfig();
    anyClass |= config.usesCPUClasses();
    configuredTotal += config.getConfiguredCPUTotal();

    if (config.usesCPUClasses()) {
      auto counts = config.getCPUClassCounts();
      if (counts.size() != classRequested.size()) compatibleCounts = false;
      for (unsigned i = 0; i < counts.size() && i < classRequested.size(); i++)
        classRequested[i] += counts[i];
    }

    LOG_DEBUG(1, "CPU RG configured policy '"
      << (name.empty() ? "Default" : name) << "': "
      << config.getCPUConfigDescription());
  }

  LOG_DEBUG(1, "CPU allocator demand: total=" << configuredTotal
    << " available=" << available.size()
    << " class-totals=" << joinValues(classRequested));

  // Count-only hybrid or unclassified systems retain legacy scheduling.
  // Confirmed homogeneous systems can use SMT allocation without class mode.
  if (!anyClass && !homogeneous) {
    LOG_DEBUG(1, "CPU allocator generation " << allocationGeneration
      << ": legacy count mode; no managed affinity; previous managed masks "
      "are discarded");
    logAllocations();
    return;
  }

  // Without hard affinity we cannot guarantee non-overlap. Preserve class
  // intent and fall back to the legacy OS-scheduled general count behaviour.
  if (!hardAffinity) {
    runtimeFallback = true;
    runtimeFallbackReason = "hard CPU affinity is not currently available";
    if (!previousFallback || previousFallbackReason != runtimeFallbackReason)
      LOG_WARNING("CPU class configuration cannot currently be enforced: "
      << runtimeFallbackReason << ". Using runtime general allocation without "
      "managed affinity; saved CPU-class configuration is unchanged.");
    logAllocations();
    return;
  }

  managed = true;

  // Determine whether all configured class constraints can be honoured by the
  // effective runtime topology. Any mismatch causes one deterministic global
  // fallback epoch: all class-mode groups become general-count requests while
  // remaining globally managed and non-overlapping.
  bool canUseClasses = effectiveClasses && compatibleCounts;
  if (anyClass && !canUseClasses)
    runtimeFallbackReason =
      "current available CPUs cannot be mapped to the configured performance classes";

  if (canUseClasses && configuredTotal > available.size()) {
    canUseClasses = false;
    runtimeFallbackReason = "current CPU availability is below the configured total";
  }

  if (canUseClasses) {
    for (unsigned i = 0; i < classRequested.size(); i++)
      if (classRequested[i] > performanceLevels[i].size()) {
        canUseClasses = false;
        ostringstream reasonOut;
        reasonOut << "performance level " << i << " currently provides "
          << performanceLevels[i].size() << " CPUs but " << classRequested[i]
          << " are configured";
        runtimeFallbackReason = reasonOut.str();
        break;
      }
  }

  if (anyClass && !canUseClasses) {
    runtimeFallback = true;
    if (!previousFallback || previousFallbackReason != runtimeFallbackReason)
      LOG_WARNING("CPU class configuration cannot currently be satisfied: "
      << runtimeFallbackReason << ". Global runtime fallback is active: all "
      "class-mode resource groups are temporarily treated as general CPU "
      "requests; saved configuration is unchanged.");
  }

  if (!anyClass || !canUseClasses) {
    // Compute max-min fair runtime budgets before preserving old assignments.
    map<string, uint32_t> targets;
    unsigned capacity = available.size();
    bool progress = true;
    while (capacity && progress) {
      progress = false;
      for (auto &name: names) {
        auto demand = groups.getGroup(name).getConfig().getConfiguredCPUTotal();
        if (capacity && targets[name] < demand) {
          targets[name]++;
          capacity--;
          progress = true;
        }
      }
    }
    CPUSet free = available;

    // Keep valid CPUs only within each fair target; preserve partial masks too.
    for (auto &name: names) {
      auto it = oldAllocations.find(name);
      if (it == oldAllocations.end()) continue;
      for (auto cpu: it->second)
        if (allocations[name].size() < targets[name] && free.erase(cpu))
          allocations[name].push_back(cpu);
      LOG_DEBUG(1, "CPU count allocation RG '" << name << "' fair-target="
        << targets[name] << " retained=" << formatCPUs(allocations[name]));
    }

    CPUList pool = effectiveClasses ? orderByPerformance(performanceLevels) :
      orderCPUs(available);
    map<string, uint32_t> deficits;
    for (auto &name: names) deficits[name] = targets[name] - allocations[name].size();
    distribute(names, pool, deficits);

  } else {
    vector<CPUSet> remaining = performanceLevels;

    // First preserve explicit class-mode RG masks that are still an exact match
    // for their configured per-class counts. This cannot steal capacity from
    // another class request because validation/runtime checks already proved
    // aggregate class demand fits.
    for (auto &name: names) {
      auto &config = groups.getGroup(name).getConfig();
      if (!config.usesCPUClasses()) continue;

      auto counts = config.getCPUClassCounts();
      auto it = oldAllocations.find(name);
      if (it == oldAllocations.end() ||
          it->second.size() != config.getConfiguredCPUTotal()) {
        LOG_DEBUG(2, "CPU allocator class mode: RG '"
          << (name.empty() ? "Default" : name)
          << "' mask not preserved because CPU count changed");
        continue;
      }

      vector<uint32_t> observed(performanceLevels.size(), 0);
      bool valid = true;
      for (auto cpu: it->second) {
        bool found = false;
        for (unsigned level = 0; level < remaining.size(); level++)
          if (remaining[level].count(cpu)) {
            observed[level]++;
            found = true;
            break;
          }
        if (!found) {valid = false; break;}
      }

      if (observed != counts) valid = false;

      if (!valid) {
        LOG_DEBUG(1, "CPU allocator class mode: RG '"
          << (name.empty() ? "Default" : name)
          << "' old mask cannot be preserved: old=" << formatCPUs(it->second)
          << " observed-class-counts=" << formatCounts(observed)
          << " configured=" << formatCounts(counts));
        continue;
      }

      allocations[name] = it->second;
      for (unsigned level = 0; level < remaining.size(); level++)
        for (auto cpu: it->second) remaining[level].erase(cpu);

      LOG_DEBUG(1, "CPU allocator class mode: preserved RG '"
        << (name.empty() ? "Default" : name) << "' mask="
        << formatCPUs(it->second));
    }

    // Allocate deficits for explicit class groups, fastest class first and one
    // CPU per RG per pass for deterministic fairness.
    for (unsigned level = 0; level < remaining.size(); level++) {
      map<string, uint32_t> requested;
      for (auto &name: names) {
        auto &config = groups.getGroup(name).getConfig();
        if (!config.usesCPUClasses()) continue;

        auto counts = config.getCPUClassCounts();
        uint32_t target = level < counts.size() ? counts[level] : 0;
        uint32_t have = 0;
        for (auto cpu: allocations[name])
          if (performanceLevels[level].count(cpu)) have++;
        if (target > have) requested[name] = target - have;
      }

      CPUList cpus = orderCPUs(remaining[level]);
      distribute(names, cpus, requested);
      for (auto &entry: allocations)
        for (auto cpu: entry.second) remaining[level].erase(cpu);
    }

    // Only after explicit class requests are satisfied may general RGs preserve
    // old masks. This prevents stability from stealing a CPU needed to honour a
    // user's explicit class constraint.
    CPUSet generalFree;
    for (auto &level: remaining)
      generalFree.insert(level.begin(), level.end());

    for (auto &name: names) {
      auto &config = groups.getGroup(name).getConfig();
      if (config.usesCPUClasses()) continue;

      uint32_t target = config.getCPUs();
      auto it = oldAllocations.find(name);
      if (it == oldAllocations.end() || it->second.size() != target) continue;
      if (!containsAll(generalFree, it->second)) {
        LOG_DEBUG(2, "CPU allocator class mode: general RG '"
          << (name.empty() ? "Default" : name)
          << "' old mask not preserved because it conflicts with explicit "
          "class reservations or current availability");
        continue;
      }

      allocations[name] = it->second;
      for (auto cpu: it->second) {
        generalFree.erase(cpu);
        for (auto &level: remaining) level.erase(cpu);
      }
      LOG_DEBUG(1, "CPU allocator class mode: preserved general RG '"
        << (name.empty() ? "Default" : name) << "' mask="
        << formatCPUs(it->second));
    }

    allocateGeneral(groups, names, orderByPerformance(remaining));
  }

  validateAllocations();
  CPUSet allAllocated;
  for (auto &name: names) {
    auto &mask = allocations[name];
    for (auto cpu: mask)
      allAllocated.insert(cpu);

    LOG_DEBUG(1, "CPU allocation generation " << allocationGeneration
      << " result RG '" << (name.empty() ? "Default" : name)
      << "': managed=" << managed << " fallback=" << runtimeFallback
      << " cpus=" << formatCPUs(mask));
  }

  logAllocations();

  LOG_DEBUG(1, "CPU allocation generation " << allocationGeneration
    << " complete: allocated=" << allAllocated.size()
    << " available=" << available.size()
    << " fallback-reason='" << runtimeFallbackReason << "'");
}


const CPUResources::CPUList &CPUResources::getGroupCPUs(
    const string &name) const {
  static const CPUList empty;
  auto it = allocations.find(name);
  return it == allocations.end() ? empty : it->second;
}
