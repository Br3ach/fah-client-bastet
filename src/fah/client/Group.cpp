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

#include "Group.h"
#include "Groups.h"
#include "App.h"
#include "OS.h"
#include "Config.h"
#include "GPUResources.h"
#include "CPUResources.h"

#include <cbang/util/Resource.h>
#include <cbang/log/Logger.h>
#include <cbang/json/Reader.h>

#include <cmath>
#include <algorithm>
#include <map>

using namespace std;
using namespace cb;
using namespace FAH::Client;

namespace FAH {namespace Client {extern const DirectoryResource resource0;}}

#undef CBANG_LOG_PREFIX
#define CBANG_LOG_PREFIX (name.empty() ? "Default" : name) << ":"


Group::Group(App &app, const string &name) :
  app(app), name(name),
  event(app.getEventBase().newEvent([this] {update();}, 0)) {
  auto &r       = FAH::Client::resource0.get("group.json");
  auto defaults = JSON::Reader::parse(r);
  config        = new Config(app, defaults);
  auto &db      = app.getDB("groups");

  if (name.empty()) {
    config->load(app.getOptions());
    config->insert("cpus", app.getOptions()["cpus"].toInteger());
  }

  if (db.has(name)) config->load(*db.getJSON(name));

  LOG_DEBUG(1, "Loaded RG CPU policy: " << config->getCPUConfigDescription());
  insert("config", config);

  triggerUpdate();
}


Group::Units Group::units() const {return Units(*app.getUnits(), name);}


void Group::setState(const JSON::Value &msg) {
  bool wasPaused = config->getPaused();
  config->setState(msg);
  if (wasPaused && !config->getPaused()) clearErrors();
  triggerUpdate();
}


bool Group::waitForIdle() const {
  return config->getOnIdle() && !app.getOS().isSystemIdle();
}


bool Group::waitOnBattery() const {
  return !config->getOnBattery() && app.getOS().isOnBattery();
}


bool Group::waitOnGPU() const {
  // Returns true if this group has enabled any GPUs which have not yet
  // been detected as active and supported.

  auto &gpus = *config->get("gpus");

  for (auto &id: gpus.keys())
    if (config->isGPUEnabled(id) && app.getGPUs().waitOnGPU(id))
      return true;

  return false;
}


bool Group::keepAwake() const {return config->getKeepAwake() && isActive();}


bool Group::isActive() const {
  for (auto unit: units())
    if (!unit->isPaused()) return true;

  return false;
}


bool Group::isAssigning() const {
  for (auto unit: units())
    if (unit->isAssigning()) return true;

  return false;
}


void Group::triggerUpdate() {if (!event->isPending()) event->activate();}


void Group::shutdown(function<void ()> cb) {
  shutdownCB = cb;
  triggerUpdate();
}


void Group::clearErrors() {
  lostWUs  = 0;
  failures = 0;
  setWait(0);
  insert("failed_wus", 0);
  insert("lost_wus",   0);
  insert("failed", "");
}


void Group::unitComplete(const string &reason, bool downloaded) {
  if (reason == "credited") clearErrors();
  else {
    if (reason != "dumped" && reason != "aborted") {
      insert("failed_wus", ++failures);
      setWait(std::pow(2, std::min(failures, 10U)));
    }

    if (downloaded) {
      insert("lost_wus", ++lostWUs);

      if (4 < lostWUs) {
        insert("failed", "Paused due too many failed Work Units.");
        config->setPaused(true);
      }
    }
  }

  triggerUpdate();
}



void Group::save()   {app.getDB("groups").set(name, config->toString());}
void Group::remove() {app.getDB("groups").unset(name);}


void Group::replaceConfig(const SmartPointer<Config> &next) {
  insert("config", next);
  config = get("config").cast<Config>();
}


void Group::notify(const list<JSON::ValuePtr> &change) {
  // Automatically save changes to config
  bool isConfig = 2 < change.size() && change.front()->getString() == "config";

  if (isConfig) {
    if (!app.getGroups()->isConfiguring()) save();
    LOG_DEBUG(2, "RG config observable changed; cpu-policy={"
      << config->getCPUConfigDescription() << "}");
    if (!app.getGroups()->isConfiguring()) app.triggerUpdate();
  }
}


void Group::update() {
  // Trigger unit updates
  for (auto unit: units())
    unit->triggerNext();

  // Remove completed units
  std::set<string> completed;
  for (auto unit: units())
    if (unit->getState() == UnitState::UNIT_DONE)
      completed.insert(unit->getID());

  for (auto &id: completed)
    app.getUnits()->removeUnit(id);

  // Handle graceful shutdown
  if (app.shouldQuit()) {
    for (auto unit: units())
      if (unit->isRunning())
        return event->add(0.25); // Check again later

    if (shutdownCB) {
      // Save state to DB
      for (auto unit: units())
        unit->save();

      shutdownCB();
      shutdownCB = 0;
    }

    return;
  }

  // No further action if waiting
  if (config->getPaused() || waitForIdle() || waitOnBattery() || waitOnGPU() ||
      isAssigning() || Time::now() < waitUntil)
    return event->add(0.25); // Check again later

  // Allocate resources
  auto &cpuResources = app.getCPUResources();
  bool managed = cpuResources.isManaged();
  auto groupCPUs = cpuResources.getGroupCPUs(name);
  unsigned remainingCPUs = managed ? groupCPUs.size() : config->getCPUs();
  std::set<string> remainingGPUs = config->getGPUs();
  std::set<string> enabledWUs;
  std::map<string, uint32_t> gpuBaseCPUs;

  LOG_DEBUG(1, "RG CPU scheduling begin: config={"
    << config->getCPUConfigDescription() << "} managed=" << managed
    << " allocation-generation=" << cpuResources.getAllocationGeneration()
    << " runtime-fallback=" << cpuResources.isRuntimeFallback()
    << " fallback-reason='" << cpuResources.getRuntimeFallbackReason()
    << "' rg-mask=" << CPUResources::formatCPUs(groupCPUs));

  // Allocate GPUs with minimum CPU requirements
  for (auto unit: units()) {
    if (UNIT_RUN < unit->getState()) continue;

    auto unitGPUs = unit->getGPUs();
    if (unitGPUs.empty()) continue;

    uint32_t minCPUs = unit->getMinCPUs();
    uint32_t baseCPUs = managed ? std::max<uint32_t>(1, minCPUs) : minCPUs;
    // GPU helper threads are not part of the exclusive CPU-folding budget.
    bool runnable = managed || minCPUs <= remainingCPUs || minCPUs < 2;

    LOG_DEBUG(2, "GPU WU candidate " << unit->getID()
      << ": min=" << minCPUs << " managed-base=" << baseCPUs
      << " remaining-rg-cpus=" << remainingCPUs
      << " runnable-before-gpu-check=" << runnable);

    std::set<string> gpusWithWU = remainingGPUs;
    for (auto id: unitGPUs) runnable &= gpusWithWU.erase(id) != 0;

    if (runnable) {
      remainingGPUs = gpusWithWU;
      if (!managed) remainingCPUs -= min(remainingCPUs, baseCPUs);
      gpuBaseCPUs[unit->getID()] = baseCPUs;
      enabledWUs.insert(unit->getID());
      LOG_DEBUG(1, "GPU WU " << unit->getID() << " helper CPUs=" << baseCPUs
        << " shared=" << managed << "; remaining RG CPUs=" << remainingCPUs);
    }
  }

  // Allocate extra CPUs to enabled GPU WUs
  for (auto unit: units()) {
    // GPU WUs that were enabled above
    if (!enabledWUs.count(unit->getID())) continue;

    uint32_t baseCPUs = gpuBaseCPUs[unit->getID()];
    uint32_t maxCPUs = std::max(baseCPUs, unit->getMaxCPUs());
    uint32_t cpus = managed ? baseCPUs : min(maxCPUs, remainingCPUs + baseCPUs);

    unit->setCPUs(cpus);
    if (!managed) remainingCPUs -= min(remainingCPUs, cpus - baseCPUs);
    LOG_DEBUG(2, "GPU WU " << unit->getID() << " final CPU count="
      << cpus << " remaining RG CPUs=" << remainingCPUs);
  }

  // Allocate remaining CPUs to existing CPU WUs
  for (auto unit: units()) {
    if (unit->hasGPUs() || remainingCPUs < unit->getMinCPUs() ||
        UNIT_RUN < unit->getState()) continue;

    uint32_t maxCPUs = unit->getMaxCPUs();
    uint32_t cpus    = min(maxCPUs, remainingCPUs);

    unit->setCPUs(cpus);
    remainingCPUs -= cpus;
    enabledWUs.insert(unit->getID());
    LOG_DEBUG(1, "CPU WU " << unit->getID() << " allocated " << cpus
      << " CPU(s); remaining RG CPUs=" << remainingCPUs);
  }

  // Convert the RG pool into concrete CPU-WU masks. Preserve an existing
  // mask when it still fits the RG allocation and CPU count. GPU helper
  // threads run outside this exclusive CPU-folding pool.
  if (managed) {
    std::set<unsigned> freeCPUs(groupCPUs.begin(), groupCPUs.end());
    std::set<string> affinityDone;

    auto preserveAffinity = [&] (const SmartPointer<Unit> &unit) {
      if (!enabledWUs.count(unit->getID()) ||
          !unit->isCPUAffinityManaged() ||
          unit->getCPUAffinity().size() != unit->getCPUs()) return false;

      for (auto cpu: unit->getCPUAffinity())
        if (!freeCPUs.count(cpu)) {
          LOG_DEBUG(2, "WU " << unit->getID()
            << " affinity not preserved: CPU " << cpu
            << " is no longer free in RG mask");
          return false;
        }

      for (auto cpu: unit->getCPUAffinity()) freeCPUs.erase(cpu);
      affinityDone.insert(unit->getID());
      LOG_DEBUG(1, "WU " << unit->getID() << " preserved affinity="
        << CPUResources::formatCPUs(unit->getCPUAffinity()));
      return true;
    };

    auto allocateAffinity = [&] (const SmartPointer<Unit> &unit) {
      if (!enabledWUs.count(unit->getID()) || affinityDone.count(unit->getID()))
        return;

      std::set<unsigned> cpus;
      for (auto cpu: groupCPUs) {
        if (!freeCPUs.count(cpu)) continue;
        cpus.insert(cpu);
        freeCPUs.erase(cpu);
        if (cpus.size() == unit->getCPUs()) break;
      }

      affinityDone.insert(unit->getID());
      LOG_DEBUG(1, "WU " << unit->getID()
        << (unit->hasGPUs() ? " GPU" : " CPU")
        << " affinity rebuilt=" << CPUResources::formatCPUs(cpus));
      unit->setCPUAffinity(true, cpus);
    };

    // GPU helpers use ordinary OS scheduling; only CPU WUs consume RG masks.
    for (auto unit: units())
      if (unit->hasGPUs()) unit->setCPUAffinity(false, {});
    for (auto unit: units()) if (!unit->hasGPUs()) preserveAffinity(unit);
    for (auto unit: units()) if (!unit->hasGPUs()) allocateAffinity(unit);

    for (auto unit: units())
      if (!unit->hasGPUs() && !enabledWUs.count(unit->getID()))
        unit->setCPUAffinity(true, {});

    LOG_DEBUG(2, "RG CPU scheduling affinity complete: unused-rg-cpus="
      << CPUResources::formatCPUs(freeCPUs));
  } else {
    for (auto unit: units()) unit->setCPUAffinity(false, {});
  }

  // Start and stop WUs, based on resource availability
  unsigned wuCount = 0;
  for (auto unit: units()) {
    wuCount++;
    bool pause = unit->atRunState() && !enabledWUs.count(unit->getID());
    unit->setPause(pause);
    LOG_DEBUG(3, "Unit " << unit->getID() << (pause ? " paused" : " enabled"));
  }

  // Report allocation status
  LOG_DEBUG(1, "Remaining CPUs: " << remainingCPUs << ", Remaining GPUs: "
    << remainingGPUs.size() << ", Active WUs: " << enabledWUs.size());

  // Handle finish, don't add any new WUs
  if (config->getFinish()) {
    if (!wuCount) config->setPaused(true);
    return;
  }

  // Add new WU if we don't already have too many and there are some resources
  const unsigned maxWUs = config->getGPUs().size() + config->getCPUs() / 64 + 3;
  bool assignable = remainingCPUs || remainingGPUs.size();


  if (wuCount < maxWUs && assignable) {
    app.getUnits()->add(
      new Unit(app, name, app.getNextWUID(), remainingCPUs, remainingGPUs));
    LOG_INFO(1, "Added new work unit: cpus:" << remainingCPUs << " gpus:"
      << String::join(remainingGPUs, ","));
    triggerUpdate();
  }
}


void Group::setWait(double delay) {
  waitUntil = Time::now() + delay;
  insert("wait", Time(waitUntil).toString());
}
