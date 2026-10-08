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

#include "GPUProcessPriorityMonitor.h"
#include "GPUProcessPriority.h"
#include <fah/client/CoreProcess.h>
using namespace std;
using namespace FAH::Client;

void GPUProcessPriorityMonitor::reset(bool captured, uint64_t done, const string &requested) {
  checked = restorePending = false;
  baselineCaptured = captured;
  launchDone = done;
  lastCheck = 0;
  nextStartupCheck = 0;
  lastRequested = requested;
}

optional<GPUProcessPriorityMonitor::Status> GPUProcessPriorityMonitor::update(
    CoreProcess &process, unsigned coreType, const string &requested,
    uint64_t done, uint64_t total, double progress, uint64_t now) {
  // If no valid shared record existed before launch, the first valid record
  // may itself be restored checkpoint progress. Wait for a subsequent advance.
  if (!baselineCaptured && total) {
    launchDone = done;
    baselineCaptured = true;
  }
  bool changed = requested != lastRequested;
  // Cancelling a pending override restores the already-running stock policy.
  // Clear diagnostics even before the core reports its first work progress.
  if (changed && requested.empty() && process.getPriorityOverride().empty() &&
      !restorePending) {
    lastRequested = requested;
    return Status{};
  }
  // Failed stock-priority restores keep timed retries even if progress stalls.
  bool check = GPUProcessPriority::shouldCheck(checked, done,
    launchDone, progress, lastCheck, changed) ||
    GPUProcessPriority::shouldCheckStartup(now, nextStartupCheck,
      done > launchDone && !restorePending, changed);
  if (check && (!requested.empty() || !process.getPriorityOverride().empty() ||
      restorePending)) {
    string warning;
#ifdef _WIN32
    process.setPriorityOverride(requested);
    warning = process.checkPriorityOverride(true);
    // A failed stock-priority restore still needs bounded retries after the
    // requested override is cleared. Retire it only after confirmed success.
    restorePending = requested.empty() && !warning.empty();
#elif defined(__linux__)
    if (!GPUProcessPriority::supportsCore(coreType))
      warning = "GPU priority override is unsupported by this folding core; stock behavior is retained.";
    else if (requested != process.getPriorityOverride())
      warning = "GPU priority change will take effect on the next core launch.";
    else warning = process.checkPriorityOverride(false);
#endif
    checked = done > launchDone;
    lastCheck = progress;
    nextStartupCheck = now + GPUProcessPriority::StartupCheckInterval;
    lastRequested = requested;
    return Status{requested, warning.empty() ? process.getPriorityOverride() : "", warning};
  }
  return nullopt;
}
