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

#include "CPUTypes.h"

#include <cstdint>

namespace FAH { namespace Client {
// Execution settings are captured at launch. Pool-only changes may be adopted
// after conflict checks while execution remains unchanged. Once stopping, the
// pool stays frozen until exit, including siblings omitted from the process mask.
// CPU placement mode, independent of whether an unmanaged WU uses a GPU.
enum class CPUAllocationMode {Unmanaged, ManagedCPU, SharedGPU, ReservedGPU};
// Also represents desired allocations; an empty managed mask prevents launch.
struct RunningCPUAllocation {
  // CPU worker count passed through -np; GPU helper accounting only.
  uint32_t workers = 0;
  CPUAllocationMode mode = CPUAllocationMode::Unmanaged;
  bool isManaged() const {return mode != CPUAllocationMode::Unmanaged;}
  bool isReservedGPU() const {return mode == CPUAllocationMode::ReservedGPU;}
  CPUSet mask;
  CPUSet resourcePool;
  uint64_t generation = 0;
  // Execution-plan diagnostics describe the owned pool, not just the mask.
  unsigned physical = 0;
  bool fullSMT = false;
};
}}
