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

#include "CPUWholeCorePacking.h"
#include "Group.h"
#include "CPUExecutionPlan.h"
#include "WUCPUAllocationPlanner.h"
#include <stdexcept>
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
#include <utility>

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


bool Group::wantsResources() const {
  if (app.shouldQuit() || config->getPaused() || waitForIdle() ||
      waitOnBattery() || waitOnGPU() || waitForRetry()) return false;
  if (!config->getFinish()) return true;
  // Finish permits existing folding work, but uploading results needs no pool.
  for (auto unit: units())
    if (unit->getState() <= UNIT_RUN) return true;
  return false;
}


bool Group::waitForRetry() const {return Time::now() < waitUntil;}


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
    if (unit->isActive()) return true;

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
    if (!app.getGroups()->isConfiguring()) app.reconcileSavedConfiguration();
  }
}


void Group::update() {
  // Local timers also drive backoff expiry and idle/battery/GPU transitions.
  // Reconcile globally before this group consumes its new desired allocation.
  bool demand = wantsResources();
  if (demand != lastResourceDemand) {
    event->add(0.25); // Retry a failed global reconciliation on the next poll.
    app.triggerUpdate();
    lastResourceDemand = demand; // Retain the old value if reconciliation throws.
  }
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

  // Scheduling pauses skip local allocation and work acquisition.
  // Existing WUs were already notified above to reconcile their resources.
  if (config->getPaused() || waitForIdle() || waitOnBattery() || waitOnGPU() ||
      waitForRetry())
    return event->add(0.25); // Check again later

  // Allocate resources
  auto &cpuResources = app.getCPUResources();
  bool managed = cpuResources.isManaged() || config->usesCPUClasses();
  auto groupCPUs = cpuResources.getGroupCPUs(name);
  unsigned remainingCPUs = managed ? cpuResources.getGroupWorkerCount(name) : config->getCPUs();
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
    if (unit->isAssigning() || UNIT_RUN < unit->getState()) continue;

    auto unitGPUs = unit->getGPUs();
    if (unitGPUs.empty()) continue;

    uint32_t minCPUs = unit->getMinCPUs();
    uint32_t baseCPUs = managed ? std::max<uint32_t>(1, minCPUs) : minCPUs;
    // GPU helper counts are scheduling/accounting metadata, not a demand for
    // distinct LPs in the affinity pool. GPU launches do not pass this count as
    // a worker-count argument. Helpers can time-share a non-empty pool, even
    // when min_cpus exceeds its size, without consuming the CPU-folding budget.
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

    unit->setScheduledCPUs(cpus);
    if (!managed) remainingCPUs -= min(remainingCPUs, cpus - baseCPUs);
    LOG_DEBUG(2, "GPU WU " << unit->getID() << " final CPU count="
      << cpus << " remaining RG CPUs=" << remainingCPUs);
  }

  // Allocate remaining CPUs to existing CPU WUs
  if (!managed) for (auto unit: units()) {
    if (unit->isAssigning() || unit->hasGPUs() || remainingCPUs < unit->getMinCPUs() ||
        UNIT_RUN < unit->getState()) continue;

    uint32_t maxCPUs = unit->getMaxCPUs();
    uint32_t cpus    = min(maxCPUs, remainingCPUs);

    unit->setScheduledCPUs(cpus);
    remainingCPUs -= cpus;
    enabledWUs.insert(unit->getID());
    LOG_DEBUG(1, "CPU WU " << unit->getID() << " allocated " << cpus
      << " CPU(s); remaining RG CPUs=" << remainingCPUs);
  }

  // Separate worker counts from exclusive whole-core process pools. Multiple
  // CPU WUs in the same RG must not expand into each other's SMT siblings.
  if (managed) {
    std::vector<WUCPUAllocationPlanner::Request> requests;
    for (auto unit: units())
      if (!unit->isAssigning() && !unit->hasGPUs() && unit->getState() <= UNIT_RUN)
        requests.push_back({unit->getID(), unit->getMinCPUs(), unit->getMaxCPUs(),
          UNIT_RUN == unit->getState()});
    WUCPUAllocationPlanner::Result allocation;
    if (config->usesCPUClasses()) {
      try {
        allocation = WUCPUAllocationPlanner::planClasses(requests,
          cpuResources.getGroupAllocationSlices(name), cpuResources.getCoreThreads());
      } catch (const std::invalid_argument &e) {
        LOG_DEBUG(1, "Class CPU ownership unavailable: " << e.what());
        // Publish empty desired ownership; existing processes stop safely.
      }
    } else allocation = WUCPUAllocationPlanner::plan(requests, remainingCPUs,
      groupCPUs, cpuResources.getCoreThreads());
    remainingCPUs = allocation.remaining;
    const auto &report = allocation.report;
    if (report.outcome != CPUWholeCorePacking::RebalanceReport::Outcome::NotNeeded)
      LOG_DEBUG(2, "CPU WU packing diagnostic RG '"
        << (name.empty() ? "Default" : name) << "': " << report.outcomeName()
        << "; pass-cores=" << report.cores
        << " pass-groups=" << report.groups
        << " pass-visited-states=" << report.visits
        << " pass-improved=" << report.improved);
    for (auto unit: units()) {
      if (unit->isAssigning()) continue;
      if (unit->hasGPUs()) {unit->setCPUAffinity(false, {}); continue;}
      auto workers = allocation.workers.find(unit->getID());
      if (workers != allocation.workers.end()) {
        enabledWUs.insert(unit->getID());
        unit->setScheduledCPUs(workers->second);
        auto pool = allocation.pools.find(unit->getID());
        auto slices = allocation.allocationSlices.find(unit->getID());
        const CPUAllocationSlices empty;
        unit->setCPUAffinity(true, pool == allocation.pools.end() ?
          CPUExecutionPlan::CPUSet{} : pool->second,
          slices == allocation.allocationSlices.end() ? &empty : &slices->second,
          config->usesCPUClasses());
      } else {
        unit->setScheduledCPUs(0);
        const CPUAllocationSlices empty;
        unit->setCPUAffinity(true, {}, &empty, config->usesCPUClasses());
      }
    }
  } else {
    for (auto unit: units()) unit->setCPUAffinity(false, {});
  }

  // Start and stop WUs, based on resource availability
  unsigned wuCount = 0;
  for (auto unit: units()) {
    wuCount++;
    // Accepted CPU work waits before starting further preparation when
    // its minimum cannot be allocated. Existing transfers may complete.
    const auto state = unit->getState();
    const bool cpuPreparation = !unit->hasGPUs() &&
      (state == UNIT_DOWNLOAD || state == UNIT_CORE);
    bool pause = (unit->atRunState() || cpuPreparation) &&
      !enabledWUs.count(unit->getID());
    unit->setPause(pause);
    LOG_DEBUG(3, "Unit " << unit->getID() << (pause ? " paused" : " enabled"));
  }

  // Publish per-WU plans only after CPU budgets, pools and pause state agree.
  app.updateCPUInfo();

  // Report allocation status
  LOG_DEBUG(1, "Remaining CPUs: " << remainingCPUs << ", Remaining GPUs: "
    << remainingGPUs.size() << ", Active WUs: " << enabledWUs.size());

  updateAssignmentOffer(remainingCPUs, std::move(remainingGPUs), enabledWUs, wuCount);
}


void Group::updateAssignmentOffer(unsigned remainingCPUs,
    std::set<string> remainingGPUs, const std::set<string> &enabledWUs,
    unsigned wuCount) {
  auto &cpuResources = app.getCPUResources();
  // Use accepted CPU work before acquiring more. This gates only the offer;
  // existing-WU redistribution and independent GPU acquisition remain intact.
  unsigned assignmentCPUs = remainingCPUs;
  for (auto unit: units()) {
    if (unit->isAssigning() || unit->hasGPUs() ||
        UNIT_RUN < unit->getState()) continue;
    if (!enabledWUs.count(unit->getID())) {
      assignmentCPUs = 0;
      break;
    }
  }

  // Existing WUs keep their assignments; offer only GPUs with usable required
  // helper allocations. Legacy GPU scheduling does not require a helper mask.
  if (cpuResources.supportsGPUAffinity() || config->getGPUReservedCores()) {
    for (auto it = remainingGPUs.begin(); it != remainingGPUs.end();) {
      if (cpuResources.getGPUCPUs(name, *it).empty() ||
          !cpuResources.getGPUAllocationShortage(name, *it).empty())
        it = remainingGPUs.erase(it);
      else ++it;
    }
  }

  // Pending offers consume no existing-WU budget or ownership. Keep an
  // unchanged request (including its backoff), or replace a stale offer.
  for (auto unit: units()) {
    if (!unit->isAssigning()) continue;
    if (unit->matchesAssignmentOffer(assignmentCPUs, remainingGPUs))
      return event->add(0.25);
    unit->abortPendingAssignment();
    --wuCount; // The aborted placeholder was counted above.
    break;
  }

  // Handle finish, don't add any new WUs
  if (config->getFinish()) {
    if (!wuCount) config->setPaused(true);
    return;
  }

  // Add new WU if we don't already have too many and there are some resources
  const unsigned maxWUs = config->getGPUs().size() + config->getCPUs() / 64 + 3;
  bool assignable = assignmentCPUs || remainingGPUs.size();


  if (wuCount < maxWUs && assignable) {
    app.getUnits()->add(
      new Unit(app, name, app.getNextWUID(), assignmentCPUs, remainingGPUs));
    LOG_INFO(1, "Added new work unit: cpus:" << assignmentCPUs << " gpus:"
      << String::join(remainingGPUs, ","));
    triggerUpdate();
  }
}


void Group::setWait(double delay) {
  waitUntil = Time::now() + delay;
  insert("wait", Time(waitUntil).toString());
}
