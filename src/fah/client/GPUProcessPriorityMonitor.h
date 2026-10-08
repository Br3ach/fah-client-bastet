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
#include <cstdint>
#include <optional>
#include <string>
namespace FAH { namespace Client {
class CoreProcess;
// Per-launch monitoring state; Unit owns progress reads, publication and logging.
class GPUProcessPriorityMonitor {
public:
  // Empty strings clear published fields; applied is also empty when a check
  // reports a warning, so it is not a measurement of the actual OS priority.
  struct Status {std::string requested, applied, warning;};
  // Reset monitoring state before each launch; process and published fields
  // remain caller-owned. A captured baseline is pre-launch progress.
  void reset(bool baselineCaptured, uint64_t launchDone,
    const std::string &requested = {});
  // requested is validated; progress is a fraction and now is wall-clock seconds.
  // nullopt leaves published fields unchanged; Status{} clears them.
  std::optional<Status> update(CoreProcess &, unsigned coreType,
    const std::string &requested, uint64_t done, uint64_t total,
    double progress, uint64_t now);
private:
  bool checked = false, baselineCaptured = false, restorePending = false;
  uint64_t launchDone = 0, nextStartupCheck = 0;
  double lastCheck = 0;
  std::string lastRequested;
};
}}
