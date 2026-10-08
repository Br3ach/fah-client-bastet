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
#include <set>
#include <string>
#include <vector>

namespace FAH { namespace Client {
// Pure saved-policy validation. Device usability and JSON parsing belong to the caller.
class CPUConfigValidator {
public:
  struct Topology {
    // Shared GPU placement support; exclusive reservations also need enough fast cores.
    bool gpuAffinity = false, configurableClasses = false;
    uint64_t available = 0;
    // Raw and currently usable logical CPU capacities, indexed by performance level.
    std::vector<uint64_t> rawClassCapacity, effectiveClassCapacity;
    // Logical CPU widths of complete fast cores, in runtime reservation order.
    std::vector<uint64_t> fastCoreWidths;
  };
  struct Request {
    std::string name, mode;
    uint64_t workers = 0;
    std::vector<uint32_t> classCounts;
    bool hasClassCounts = false;
    uint32_t reservedCores = 0;
    // Usable enabled GPU IDs resolved by the caller.
    std::set<std::string> gpus;
  };
  struct Result {bool valid = false; std::string error;};
  // Names must be unique within each list; duplicates return an invalid result.
  static Result validate(const Topology &, const std::vector<Request> &current,
    const std::vector<Request> &proposed);
};
}}
