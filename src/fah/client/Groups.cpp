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

#include "Groups.h"
#include "App.h"
#include "Config.h"
#include "CPUResources.h"

#include <cbang/Catch.h>
#include <cbang/Exception.h>
#include <cbang/json/Reader.h>
#include <cbang/util/Resource.h>
#include <cbang/db/Transaction.h>
#include <map>
#include <cbang/log/Logger.h>

using namespace std;
using namespace cb;
using namespace FAH::Client;

namespace FAH {namespace Client {extern const DirectoryResource resource0;}}


Groups::Groups(App &app) : app(app) {
  getGroup(""); // Default group

  app.getDB("groups").foreach(
    [this] (const string &name, const string &dataStr) {
      try {
        if (!name.empty()) getGroup(name);
      } CATCH_ERROR;
    }, 1000);
}


const Group &Groups::getGroup(const string &name) const {
  return *get(name).cast<Group>();
}


Group &Groups::getGroup(const string &name) {
  if (!has(name)) {
    LOG_INFO(3, "Loading " << (name.empty() ? "default" : name)
             << " resource group");

    SmartPointer<Group> group = new Group(app, name);
    insert(name, group);
    group->save();
  }

  return *get(name).cast<Group>();
}


void Groups::delGroup(const string &name) {
  if (name.empty()) THROW("Cannot delete default group");

  if (!has(name)) return;

  LOG_INFO(1, "Deleting resource group " << name);

  // Migrate units
  auto &group = getGroup(name);
  auto &root  = getGroup("");

  for (auto unit: group.units())
    unit->setGroup(&root);

  // Remove from DB
  group.remove();

  // Remove
  erase(name);
}


JSON::ValuePtr Groups::proposeConfiguration(const JSON::Value &configs) const {
  auto proposed = SmartPtr(new JSON::Dict);
  auto defaults = JSON::Reader::parse(resource0.get("group.json"));
  std::set<string> names;
  for (auto &name: configs.keys()) names.insert(name);
  // The default RG cannot be removed by omission.
  names.insert("");
  for (auto &name: names) {
    auto merged = JSON::Reader::parse(
      has(name) ? getGroup(name).getConfig().toString() : defaults->toString());
    if (configs.has(name)) {
      auto incoming = configs.get(name);
      if (!incoming->isDict()) THROW("Resource group config must be a dictionary");
      for (auto it = incoming->begin(); it != incoming->end(); ++it) {
        if (!defaults->has(it.key())) continue;
        if (defaults->get(it.key())->getType() != (*it)->getType())
          THROW("Wrong type for resource group setting '" << it.key() << "'");
        // Deep copy: caller-owned nested values never become mutable policy.
        merged->insert(it.key(), JSON::Reader::parse((*it)->toString()));
      }
    }
    if (merged->getString("cpu_mode", "count") == "classes") {
      uint64_t total = 0;
      for (auto value: *merged->get("cpu_class_counts")) total += value->getU32();
      if (total > UINT32_MAX) THROW("CPU class total exceeds uint32 capacity");
      merged->insert("cpus", (uint32_t)total);
    }
    proposed->insert(name, merged);
  }
  return proposed;
}


void Groups::configure(const JSON::Value &configs) {
  // Include validation-time topology reconciliation in the notification batch.
  // Rejected requests still publish the current policy and refreshed topology.
  app.beginGroupConfigNotifications();
  struct ConfigureGuard {
    bool &flag;
    App &app;
    ~ConfigureGuard() {
      flag = false;
      TRY_CATCH_ERROR(app.endGroupConfigNotifications(true));
    }
  } guard{configuring, app};
  auto proposed = proposeConfiguration(configs);
  validateCPUConfiguration(*proposed);

  auto defaults = JSON::Reader::parse(resource0.get("group.json"));
  map<string, SmartPointer<Config>> staged, previousConfigs;
  map<string, SmartPointer<Group>> previousGroups;
  for (auto &name: proposed->keys()) {
    auto next = SmartPointer<Config>(new Config(app, defaults));
    next->load(*proposed->get(name));
    staged[name] = next;
  }
  for (auto &name: keys()) {
    previousGroups[name] = get(name).cast<Group>();
    previousConfigs[name] = getGroup(name).get("config").cast<Config>();
  }
  map<string, SmartPointer<Group>> unitGroups;
  map<string, SmartPointer<Unit>> previousUnits;
  for (auto &entry: previousGroups)
    for (auto unit: entry.second->units()) {
      unitGroups[unit->getID()] = entry.second;
      previousUnits[unit->getID()] = unit;
    }

  // SQLite protects persistence; the snapshots restore live state on failure.
  auto transaction = app.getDatabase().begin();
  configuring = true;
  try {
    for (auto &entry: staged) getGroup(entry.first).replaceConfig(entry.second);
    std::set<string> remove;
    for (auto &name: keys())
      if (!name.empty() && !proposed->has(name)) remove.insert(name);
    for (auto &name: remove) delGroup(name);
    for (auto &name: keys()) getGroup(name).save();
    for (auto &entry: previousUnits)
      if (entry.second->getGroup().getName() != unitGroups[entry.first]->getName())
        entry.second->save();
    transaction->commit();
  } catch (...) {
    // Restore original objects, so migrated WUs keep their original RG identity.
    std::set<string> added;
    for (auto &name: keys())
      if (!previousGroups.count(name)) added.insert(name);
    for (auto &name: added) erase(name);
    for (auto &entry: previousGroups) {
      insert(entry.first, entry.second);
      entry.second->replaceConfig(previousConfigs[entry.first]);
    }
    for (auto &entry: previousUnits)
      entry.second->setGroup(unitGroups[entry.first]);
    app.getDatabase().rollback();
    configuring = false;
    // Reconciliation must not replace the original application exception.
    TRY_CATCH_ERROR(triggerUpdate());
    app.endGroupConfigNotifications(true);
    throw;
  }
  configuring = false;
  triggerUpdate();
  app.endGroupConfigNotifications(true);
}


// Keep saved intent across topology changes, but reject new unsupported edits.
static void validatePerformancePin(bool pin, bool unchanged, bool supported,
                                   bool managed, uint32_t cpus,
                                   uint32_t performanceCPUs) {
  if (!pin || unchanged) return;
  if (!supported)
    THROW("Pin to Perf Cores requires available performance classes and hard affinity");
  if (!managed && performanceCPUs < cpus)
    THROW("Pinned CPU count exceeds performance class 1 logical CPU capacity ("
      << performanceCPUs << ')');
}


void Groups::validateCPUConfiguration(const JSON::Value &configs) const {
  auto &cpu = app.getCPUResources();
  if (cpu.refreshTopology("config-validation")) {
    LOG_INFO(3, "Runtime topology changed during configuration validation; "
      "reconciling existing saved policy independently of request acceptance");
    app.triggerUpdate();
  }

  auto &levels = cpu.getRawPerformanceLevels();
  vector<uint64_t> classTotals(levels.size(), 0);
  uint64_t total = 0;
  uint64_t capacity = 0;
  bool anyClass = false;
  bool oldAnyClass = false;
  bool cpuPolicyChanged = false;

  for (auto &name: keys())
    if (getGroup(name).getConfig().usesCPUClasses()) oldAnyClass = true;
  for (auto &level: levels) capacity += level.size();
  // Pinning affects CPU WUs only when every proposed group uses count mode.
  for (auto &name: configs.keys()) {
    auto config = configs.get(name);
    if (config->isDict() && config->getString("cpu_mode", "count") == "classes")
      anyClass = true;
  }

  LOG_DEBUG(1, "CPU config validation begin: raw-capacity=" << capacity
    << " levels=" << levels.size()
    << " topology-generation=" << cpu.getTopologyGeneration());
  for (unsigned i = 0; i < levels.size(); i++)
    LOG_DEBUG(2, "CPU config validation raw level " << i << " capacity="
      << levels[i].size() << " cpus=" << CPUResources::formatCPUs(levels[i]));

  for (auto &name: keys())
    if (!configs.has(name)) {
      auto &old = getGroup(name).getConfig();
      if (old.getConfiguredCPUTotal()) cpuPolicyChanged = true;
      LOG_DEBUG(1, "CPU config validation: RG '"
        << (name.empty() ? "Default" : name) << "' is being removed; old={"
        << old.getCPUConfigDescription() << "}");
    }

  for (auto &name: configs.keys()) {
    auto config = configs.get(name);
    if (!config->isDict()) {
      LOG_WARNING("Rejecting CPU configuration: RG '" << name
        << "' is not a dictionary");
      THROW("Resource group config must be a dictionary");
    }

    string mode = config->getString("cpu_mode", "count");
    if (mode != "count" && mode != "classes") {
      LOG_WARNING("Rejecting CPU configuration: invalid mode '" << mode
        << "' for RG '" << name << "'");
      THROW("Invalid CPU mode '" << mode << "' for resource group '"
            << name << "'");
    }

    bool pin = config->getBoolean("pin_to_perf_cores", false);
    bool pinUnchanged = oldAnyClass == anyClass && has(name) &&
      getGroup(name).getConfig().getPinToPerfCores() == pin &&
      getGroup(name).getConfig().getCPUMode() == mode &&
      getGroup(name).getConfig().getConfiguredCPUTotal() == config->getU32("cpus", 0);
    validatePerformancePin(pin, pinUnchanged, cpu.hasPerformanceClasses(),
      anyClass, config->getU32("cpus", 0),
      levels.empty() ? 0 : (uint32_t)levels.front().size());

    vector<uint32_t> proposedCounts;
    if (config->hasList("cpu_class_counts"))
      for (auto value: *config->get("cpu_class_counts"))
        proposedCounts.push_back(value->getU32());

    uint64_t proposedTotal = mode == "classes" ? 0 : config->getU32("cpus", 0);
    if (mode == "classes")
      for (auto count: proposedCounts) proposedTotal += count;
    LOG_DEBUG(1, "CPU config validation candidate RG '"
      << (name.empty() ? "Default" : name) << "': mode=" << mode
      << " total=" << proposedTotal
      << " classes=" << CPUResources::formatCounts(proposedCounts));

    if (!has(name)) {
      if (mode == "classes" || config->getU32("cpus", 0)) cpuPolicyChanged = true;
    } else {
      auto &old = getGroup(name).getConfig();
      if (old.getCPUMode() != mode ||
          (mode == "classes" && old.getCPUClassCounts() != proposedCounts) ||
          (mode == "count" && old.getConfiguredCPUTotal() !=
            config->getU32("cpus", 0)))
        cpuPolicyChanged = true;
    }

    if (mode == "classes") {
      anyClass = true;
      bool unchanged = false;
      if (has(name)) {
        auto &old = getGroup(name).getConfig();
        unchanged = old.usesCPUClasses() &&
          old.getCPUClassCounts() == proposedCounts;
      }

      if (!cpu.hasPerformanceClasses() && !unchanged) {
        LOG_WARNING("Rejecting CPU class edit for RG '" << name
          << "': raw performance classes are not currently configurable");
        THROW("CPU performance-class allocation is not currently configurable");
      }

      if (!config->hasList("cpu_class_counts")) {
        LOG_WARNING("Rejecting CPU configuration for RG '" << name
          << "': class mode has no cpu_class_counts");
        THROW("Class CPU mode requires cpu_class_counts");
      }

      if (!unchanged && proposedCounts.size() != levels.size()) {
        LOG_WARNING("Rejecting CPU configuration for RG '" << name
          << "': class-count length=" << proposedCounts.size()
          << " raw-level-count=" << levels.size());
        THROW("CPU class count does not match current performance-level count");
      }

      uint64_t groupTotal = 0;
      for (unsigned i = 0; i < proposedCounts.size(); i++) {
        groupTotal += proposedCounts[i];
        if (i < classTotals.size()) classTotals[i] += proposedCounts[i];
        else if (proposedCounts[i] && !unchanged) {
          LOG_WARNING("Rejecting CPU configuration for RG '" << name
            << "': non-zero request for non-existent level " << i);
          THROW("CPU class configuration references a non-existent level");
        }
      }
      total += groupTotal;

    } else total += config->getU32("cpus", 0);
  }

  LOG_DEBUG(1, "CPU config validation aggregate: cpu-policy-changed="
    << cpuPolicyChanged << " any-class=" << anyClass << " total=" << total
    << "/" << capacity << " class-totals=" << classTotals.size());
  for (unsigned i = 0; i < classTotals.size(); i++)
    LOG_DEBUG(2, "CPU config validation aggregate level " << i << "="
      << classTotals[i] << "/" << levels[i].size());

  // Unrelated settings updates do not revalidate or destroy saved CPU intent.
  if (!cpuPolicyChanged) {
    LOG_DEBUG(1, "CPU config validation: no CPU-policy change; accepted");
    return;
  }

  // Preserve legacy oversubscription semantics when nobody uses class mode.
  if (!anyClass) {
    LOG_DEBUG(1, "CPU config validation: count-only configuration; legacy "
      "oversubscription semantics retained");
    return;
  }

  // If class configuration cannot currently be edited, unchanged persisted
  // class policy may survive while other RGs are reset/removed.
  if (!cpu.hasPerformanceClasses()) {
    LOG_WARNING("CPU class policy is currently unavailable for editing; "
      "preserving unchanged saved class allocations");
    return;
  }

  if (levels.size() < 2) {
    LOG_WARNING("CPU raw performance topology is unavailable; preserving "
      "unchanged saved class allocations");
    return;
  }

  for (unsigned i = 0; i < classTotals.size(); i++)
    if (classTotals[i] > levels[i].size()) {
      LOG_WARNING("Rejecting CPU configuration: level " << i << " requests "
        << classTotals[i] << " logical CPUs, raw capacity="
        << levels[i].size());
      THROW("Configured CPU performance level " << i << " requests "
            << classTotals[i] << " logical CPUs but capacity is "
            << levels[i].size());
    }

  if (total > capacity) {
    LOG_WARNING("Rejecting CPU configuration: managed total=" << total
      << " raw capacity=" << capacity);
    THROW("Configured managed CPU total " << total
          << " exceeds CPU topology capacity " << capacity);
  }

  LOG_DEBUG(1, "CPU config validation accepted transaction: total="
    << total << '/' << capacity);
}


void Groups::triggerUpdate() {
  auto &cpu = app.getCPUResources();
  LOG_DEBUG(2, "Global RG update: rebuilding CPU allocation from current policy");
  cpu.update(*this, "groups-trigger");
  app.updateCPUInfo();

  for (auto &name: keys()) getGroup(name).triggerUpdate();
}


void Groups::setState(const JSON::Value &msg) {
  if (msg.hasString("group")) {
    auto name = msg.getString("group");
    LOG_INFO(1, "Group " << name << " state " << msg.getString("state"));
    getGroup(name).setState(msg);

  } else {
    LOG_INFO(1, "Machine state " << msg.getString("state"));
    for (auto &name: keys())
      getGroup(name).setState(msg);
  }
}


bool Groups::getPaused() const {
  for (auto &name: keys())
    if (!getGroup(name).getConfig().getPaused())
      return false;

  return true;
}


bool Groups::keepAwake() const {
  for (auto &name: keys())
    if (getGroup(name).keepAwake())
      return true;

  return false;
}
