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

#include <cbang/os/Subprocess.h>

#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>


namespace FAH {
  namespace Client {
    class AffinityRejected : public std::runtime_error {
    public:
      explicit AffinityRejected(const char *reason =
        "Required FahCore CPU affinity could not be applied exactly") :
        std::runtime_error(reason) {}
    };

    class SchedulingRejected : public std::runtime_error {
    public:
      SchedulingRejected() :
        std::runtime_error("Required FahCore scheduler/nice settings could not be established") {}
    };

    // Own and operate this object as CoreProcess: cbang's Subprocess lifecycle
    // methods and destructor are non-virtual and do not dispatch to CoreProcess.
    class CoreProcess : public cb::Subprocess {
      struct StrictProcess;
      std::unique_ptr<StrictProcess> strictProcess;

      // Hide cbang's independent best-effort affinity state. Required affinity
      // must go through the verified launch path below.
      using cb::Subprocess::setAffinity;
      using cb::Subprocess::getAffinity;

      std::set<unsigned> requiredAffinity;
      void execStrict(const std::vector<std::string> &args);
#ifdef _WIN32
      void execStrictWindows(
        const std::vector<std::string> &args, StrictProcess &child);
#elif defined(__linux__)
      void execStrictLinux(
        const std::vector<std::string> &args, StrictProcess &child);
#endif

      std::string priorityOverride;
      bool priorityMismatchLogged = false;

      const std::string path;
      uint64_t interruptTime  = 0;
      uint64_t lastStop       = 0;
      bool     killedByClient = false;

    public:
      CoreProcess(const std::string &path);
      ~CoreProcess();

      // Stores a non-empty launch mask; exec() verifies exact application.
      // Does not change the affinity of an already-running process.
      void setRequiredAffinity(const std::set<unsigned> &cpus);

      bool isRunning();
      uint64_t getPID() const;
      int wait(bool nonblocking = false);
      bool kill(bool nonblocking = false);
      void interrupt();

      // True once the core has been asked to stop, until it exits
      bool isStopping() const {return interruptTime;}

      // True if we killed the core because it failed to shutdown gracefully
      bool getKilledByClient() const {return killedByClient;}

      void exec(const std::vector<std::string> &args);
      void stop();

      // Values must already be validated by GPUProcessPriority::valid().
      // Stores the selection; does not itself change a running process.
      void setPriorityOverride(const std::string &value) {
        if (priorityOverride != value) priorityMismatchLogged = false;
        priorityOverride = value;
      }

      const std::string &getPriorityOverride() const {
        return priorityOverride;
      }

      // Windows can apply the stored selection live when apply is true.
      // Linux only verifies it; live configuration changes require a new launch.
      // Priority failure is advisory and never weakens required affinity.
      std::string checkPriorityOverride(bool apply);
#ifdef _WIN32
      unsigned long desiredPriorityClass() const;
#endif
    };
  }
}
