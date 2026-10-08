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
#include "GPUProcessPriority.h"
#include "CPUResources.h"
#include "CPUConfigValidator.h"

#include <cbang/Catch.h>
#include <cbang/Exception.h>
#include <cbang/json/Reader.h>
#include <cbang/util/Resource.h>
#include <cbang/db/Transaction.h>
#include <map>
#include <utility>
#include <vector>
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
    // Serialization removes observable parent links; copy(true) preserves them.
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
    Config::validateCPUCount(*merged->get("cpus"));
    for (const auto key :
        {"cpu_mode", "cpu_class_counts", "gpu_reserved_cores"})
      Config::validateCPUSetting(key, *merged->get(key));
    if (merged->has("gpu_priority") && (!merged->hasString("gpu_priority") ||
        !GPUProcessPriority::valid(merged->getString("gpu_priority"))))
      THROW("Unsupported GPU process priority for this platform");
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
      try {
        try {
          app.endGroupConfigNotifications(true);
        } CBANG_CATCH_ALL(CBANG_LOG_ERROR_LEVEL,
            " while publishing resource-group configuration")
      } catch (...) {
        // Logging must not escape the destructor.
      }
    }
  } guard{configuring, app};
  auto proposed = proposeConfiguration(configs);

  auto defaults = JSON::Reader::parse(resource0.get("group.json"));
  map<string, SmartPointer<Config>> staged;
  for (auto &name: proposed->keys()) {
    auto next = SmartPointer<Config>(new Config(app, defaults));
    next->load(*proposed->get(name));
    staged.emplace(name, next);
  }
  validateCPUConfiguration(staged);

  map<string, SmartPointer<Config>> previousConfigs;
  map<string, SmartPointer<Group>> previousGroups;
  for (auto &name: keys()) {
    previousGroups[name] = get(name).cast<Group>();
    previousConfigs[name] = getGroup(name).get("config").cast<Config>();
  }
  vector<pair<SmartPointer<Unit>, SmartPointer<Group>>> previousUnits;
  for (const auto &entry: previousGroups)
    for (auto unit: entry.second->units())
      previousUnits.emplace_back(unit, entry.second);

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
      if (entry.first->getGroup().getName() != entry.second->getName())
        entry.first->save();
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
      entry.first->setGroup(entry.second);
    app.getDatabase().rollback();
    configuring = false;
    // Reconciliation must not replace the original application exception.
    TRY_CATCH_ERROR(triggerUpdate());
    throw;
  }
  configuring = false;
  // Cancel old-policy assignments only after commit. Rollback must preserve
  // their active requests; already-assigned/running WUs migrate normally.
  for (const auto &entry: previousUnits) {
    const auto &oldName = entry.second->getName();
    if (!oldName.empty() && !proposed->has(oldName))
      TRY_CATCH_ERROR(entry.first->abortPendingAssignment());
  }
  app.reconcileSavedConfiguration();
}


void Groups::validateCPUConfiguration(
    const map<string, SmartPointer<Config>> &staged) const {
  auto &cpu = app.getCPUResources();
  if (cpu.refreshTopology("config-validation")) {
    LOG_INFO(3, "Runtime topology changed during configuration validation; "
      "reconciling existing saved policy independently of request acceptance");
    app.triggerUpdate();
  }

  // Validate the same staged Config objects that configure() will publish.
  // Raw numeric/type checks are performed before Config loading.
  CPUConfigValidator::Topology topology;
  topology.gpuAffinity = cpu.supportsGPUAffinity();
  topology.configurableClasses = cpu.hasPerformanceClasses();
  topology.available = cpu.getAvailableCPUs().size();
  for (const auto &level: cpu.getRawPerformanceLevels())
    topology.rawClassCapacity.push_back(level.size());
  for (const auto &level: cpu.getPerformanceLevels())
    topology.effectiveClassCapacity.push_back(level.size());
  for (const auto &core: cpu.getFastPhysicalCores())
    topology.fastCoreWidths.push_back(core.size());
  auto request = [] (const string &name, const Config &config) {
    CPUConfigValidator::Request result;
    result.name = name; result.mode = config.getCPUMode();
    result.workers = config.getConfiguredCPUTotal();
    result.classCounts = config.getCPUClassCounts();
    result.hasClassCounts = config.hasList("cpu_class_counts");
    result.reservedCores = config.getGPUReservedCores(); result.gpus = config.getGPUs();
    return result;
  };
  vector<CPUConfigValidator::Request> current, proposed;
  for (const auto &name: keys())
    current.push_back(request(name, getGroup(name).getConfig()));
  for (const auto &entry: staged) proposed.push_back(request(entry.first, *entry.second));
  const auto result = CPUConfigValidator::validate(topology, current, proposed);
  if (!result.valid) {
    LOG_WARNING("Rejecting CPU configuration: " << result.error);
    THROW(result.error);
  }
}


void Groups::triggerUpdate() {
  if (app.isGroupReconciliationDeferred()) return;
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
