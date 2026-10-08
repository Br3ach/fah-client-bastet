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

#include "RunningCPUAllocation.h"
#include "CPUAllocationSlice.h"
#include "CPUExecutionPlan.h"
#include "GPUProcessPriorityMonitor.h"
#include "CPUStatus.h"
#include <optional>

#include "UnitState.h"

#include <cbang/json/Observable.h>
#include <cbang/event/Event.h>
#include <cbang/http/Enum.h>
#include <cbang/http/Client.h>
#include <cbang/http/Request.h>
#include <cbang/net/URI.h>


namespace cb {class TailFileToLog;}


namespace FAH {
  namespace Client {
    class App;
    class Group;
    class GPUResource;
    class Core;
    class CoreProcess;
    class Config;

    class Unit :
      public cb::JSON::ObservableDict, public cb::HTTP::Enum,
      public UnitState::Enum {
      App &app;
      cb::SmartPointer<Group> group;

      cb::Event::EventPtr          event;
      cb::HTTP::Client::RequestPtr pr;

      uint64_t    wu;
      std::string id;

      cb::JSON::ValuePtr              data;
      cb::JSON::ValuePtr              topology;
      std::vector<cb::JSON::ValuePtr> frames;
      cb::SmartPointer<Core>          core;

      unsigned viewerFrame = 0;
      int      viewerFail  = 0;
      uint64_t viewerBytes = 0;

      cb::SmartPointer<CoreProcess> process;
      cb::SmartPointer<cb::TailFileToLog> logCopier;
      unsigned bytesCopiedToLog = 0;

      unsigned retries     = 0;
      double   wait        = 0;
      int      cs          = -1;
      RunningCPUAllocation runningAllocation;
      std::optional<CPUExecutionPlan::SliceExecution> buildCPUExecutionPlan() const;
      RunningCPUAllocation buildDesiredAllocation() const;
      std::vector<std::string> buildCoreArgs(const RunningCPUAllocation &allocation) const;
      cb::SmartPointer<CoreProcess> createCoreProcess(const RunningCPUAllocation &allocation) const;
      bool blocksCPULaunch(const Unit &running,
        const RunningCPUAllocation &desired) const;
      bool desiredAffinityManaged() const;
      bool desiredGPUReservation() const;
      bool affinityManaged = false;
      uint64_t affinityAllocationGeneration = 0;
      bool allocationBlockedLogged = false;
      std::set<unsigned> affinityCPUs;
      bool affinityClassMode = false;
      CPUAllocationSlices affinitySlices;
      // Failed launches have an attempted mask but no live ownership record.
      std::set<unsigned> attemptedAffinityCPUs;
      std::set<unsigned> rejectedAffinityCPUs;
      uint64_t rejectedTopologyGeneration = 0;
      unsigned affinityRejections = 0;
      bool affinityRejectionLogged = false;

      GPUProcessPriorityMonitor gpuPriorityMonitor;
      uint64_t processStartTime = 0; // Core process start time
      uint64_t lastSkewTimer    = 0; // For detecting clock skew
      int64_t  clockSkew        = 0; // Due to sleeping or clock changes

      uint64_t lastKnownDone                  = 0;
      uint64_t lastKnownTotal                 = 0;
      uint64_t lastKnownProgressUpdate        = 0;
      uint64_t lastKnownProgressUpdateRunTime = 0;

      unsigned stalls    = 0; // Consecutive stalls without progress
      uint64_t stallDone = 0; // Progress at the last detected stall

      Unit(App &app);

    public:
      Unit(App &app, const std::string &group, uint64_t wu, uint32_t cpus,
           const std::set<std::string> &gpus);
      Unit(App &app, const cb::JSON::ValuePtr &data);
      ~Unit();

      void setGroup(const cb::SmartPointer<Group> &group);
      Group &getGroup() const {return *group;}

      const Config &getConfig() const;

      const std::string &getID() const {return id;}
      std::string getClientID() const;
      cb::JSON::ValuePtr getTopology() const {return topology;}
      const std::vector<cb::JSON::ValuePtr> &getFrames() const {return frames;}
      unsigned getRetries() const {return retries;}

      UnitState getState() const;
      bool atRunState() const;
      bool isAssigning() const;
      void abortPendingAssignment();
      bool matchesAssignmentOffer(
        uint32_t cpus, const std::set<std::string> &gpus) const;
      bool isWaiting() const;
      bool isPaused() const;
      void setPause(bool pause);
      const char *getPauseReason() const;
      bool isRunning() const;
      bool isActive() const;
      // A RUN unit without a process has a recorded launch rejection.
      bool hasLaunchFailure() const {
        return getState() == UNIT_RUN && !isRunning() &&
          (affinityRejections != 0 || has("launch_environment_warning"));
      }

      void setScheduledCPUs(uint32_t cpus);
      // cpus is the owned resource pool, not the final execution mask.
      // Supplied slices are copied; empty slices block a managed class-mode CPU launch.
      void setCPUAffinity(bool managed, const std::set<unsigned> &cpus,
        const CPUAllocationSlices *slices = nullptr, bool classMode = false);
      // Offer count before assignment; scheduler target afterward.
      // May differ from runningAllocation.workers until restart completes.
      // Accepted assignment bounds come from signed assignment data.
      uint32_t getScheduledCPUs() const {return getU32("cpus");}
      std::optional<UnitCPUStatus> getDesiredCPUStatus() const;
      uint32_t getMinCPUs() const;
      uint32_t getMaxCPUs() const;
      void setGPUs(const std::set<std::string> &gpus);
      std::set<std::string> getGPUs() const;
      bool hasGPU(const std::string &id) const;
      bool hasGPUs() const {return !getGPUs().empty();}

      uint64_t getRunTimeDelta() const;
      uint64_t getRunTime() const;
      uint64_t getRunTimeEstimate() const;
      double   getEstimatedProgress() const;
      uint64_t getCreditEstimate() const;
      uint64_t getETA() const;
      uint64_t getPPD() const;

      std::string getLogPrefix() const;
      std::string getDirectory() const;
      cb::URI getWSURL(const std::string &path) const;
      uint64_t getDeadline() const;
      bool isFinished() const;
      bool isExpired() const;

      void triggerNext(double secs = 0);
      void dumpWU();
      void save();

    protected:
      void cancelRequest();
      void setState(UnitState state);
      void next();

      void processStarted(const cb::SmartPointer<CoreProcess> &process);
      void processEnded();
      void skewTimer();
      double getKnownProgress() const;
      void updateKnownProgress(uint64_t done, uint64_t total);
      bool isStalled() const;

      void clearProgress() {setProgress(0, 0);}
      void setProgress(double done, double total, bool wu = false);
      void getCore();
      void run();
      bool readCoreProgress(uint64_t &done, uint64_t &total);
      void readInfo();
      void readViewerData();
      void readViewerTop();
      void readViewerFrame();
      void setResults(const std::string &status, const std::string &dataHash);
      void finalizeRun();
      void stopRun();
      void monitorRun();
      void monitorGPUPriority();
      void clean(const std::string &result);
      void setWait(double delay);
      void retry();

      void resolveAssignmentResources(const cb::JSON::ValuePtr &assign);
      void assignResponse(const cb::JSON::ValuePtr &data);
      void writeProjectRestrictions(cb::JSON::Sink &sink,
                                    const cb::JSON::ValuePtr &project);
      void writeRequest(cb::JSON::Sink &sink) const;
      void assign();
      void downloadResponse(const cb::JSON::ValuePtr &data);
      void download();
      void uploadResponse(const cb::JSON::ValuePtr &data);
      void upload();
      void dumpResponse(const cb::JSON::ValuePtr &data);
      void dump();
      void response(cb::HTTP::Request &req);
      void logCredit(const cb::JSON::ValuePtr &data);
      void startLogCopy(const std::string &filename);
      void endLogCopy();
    };
  }
}
