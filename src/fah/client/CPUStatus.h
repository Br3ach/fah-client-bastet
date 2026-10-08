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
#include <string>
#include <vector>
namespace FAH { namespace Client {
struct CPUClassStatus {uint32_t logical = 0, availableLogical = 0, physical = 0;};
struct TopologyCPUStatus {
  bool hardAffinity = false, classSelection = false, effectiveClasses = false;
  bool smtTopology = false, managed = false, runtimeFallback = false, gpuReservation = false;
  uint32_t available = 0, allocatable = 0, physical = 0;
  uint64_t topologyGeneration = 0, allocationGeneration = 0;
  std::string fallbackReason;
  std::vector<uint32_t> fastCoreWidths;
  std::vector<CPUClassStatus> levels;
};
// Class index matches performance_levels; zero entries retain their index.
// Group logical/physical counts describe the pool; WU counts describe the
// desired process mask. poolLogical/poolPhysical always describe ownership.
// Physical counts may be zero when usable physical-core coverage is unknown.
struct CPUClassAllocationStatus {
  uint32_t workers = 0, logical = 0, physical = 0, poolLogical = 0, poolPhysical = 0;
  bool fullSMT = false, potentialFullSMT = false;
};
struct GroupCPUStatus {
  std::string name, mode;
  uint32_t logical = 0, physical = 0, workers = 0, configured = 0;
  // Topology-based prediction, independent of a particular FahCore type.
  bool hasSMT = false, potentialFullSMT = false;
  std::vector<uint32_t> classCounts;
  std::vector<CPUClassAllocationStatus> allocationSlices;
};
// Generation-checked desired allocation, not OS readback or a stopping
// process's captured allocation.
struct UnitCPUStatus {
  std::string id, group, mode;
  uint64_t number = 0;
  uint32_t workers = 0, logical = 0, physical = 0, poolPhysical = 0, poolLogical = 0, configured = 0;
  // Core-specific policy; true if any slice fully uses its SMT pool.
  bool fullSMT = false;
  std::vector<uint32_t> classCounts;
  std::vector<CPUClassAllocationStatus> allocationSlices;
};
struct GPUHelperStatus {std::string group, reason;};
// Detached value snapshot; fields remain mutable. No JSON or application references.
struct CPUStatusSnapshot {
  TopologyCPUStatus topology;
  std::vector<GroupCPUStatus> groups;
  std::vector<UnitCPUStatus> units;
  std::vector<GPUHelperStatus> gpuHelpers;
};
}}
