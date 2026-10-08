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
#include <string>
#include <vector>
#include <algorithm>

namespace FAH { namespace Client {
// Platform-native names intentionally avoid mapping unlike scheduling policies.
struct GPUProcessPriority {
  static std::vector<std::string> options() {
#ifdef _WIN32
    // Realtime is deliberately excluded: it can impair system responsiveness.
    return {"", "idle", "below-normal", "normal", "above-normal", "high"};
#elif defined(__linux__)
    // Core High/Realtime mean negative nice values, not realtime scheduling.
    // Raising priority above nice 0 normally needs additional privileges;
    // offer only Low (10) and Normal (0) for ordinary user accounts.
    return {"", "other-low", "other-normal"};
#else
    return {""};
#endif
  }
  static bool valid(const std::string &value) {
    auto values = options();
    return std::find(values.begin(), values.end(), value) != values.end();
  }
  static constexpr unsigned StartupCheckInterval = 5;
  static bool shouldCheckStartup(unsigned long long now, unsigned long long next,
      bool advanced, bool changed) {
#ifdef _WIN32
    // A deadline more than one interval ahead indicates a backward clock change.
    return changed || (!advanced &&
      (now >= next || next - now > StartupCheckInterval));
#else
    (void)now; (void)next; (void)advanced; (void)changed;
    return false;
#endif
  }
  static bool shouldCheck(bool checked, unsigned long long done,
      unsigned long long launchDone, double progress, double lastCheck, bool changed) {
    if (done <= launchDone) return false;
#ifdef _WIN32
    return !checked || changed || progress + 1e-9 >= lastCheck + 0.05;
#else
    (void)progress; (void)lastCheck;
    return !checked || changed;
#endif
  }
  static bool supportsCore(unsigned type) {return type == 0x27 || type == 0x28;}
  // Linux conversions require a validated, non-empty override:
  // "other-low" or "other-normal".
  static std::string coreArgument(const std::string &value) {
    return value == "other-low" ? "low" : "normal";
  }
  static int niceValue(const std::string &value) {return value == "other-low" ? 10 : 0;}
};
}}
