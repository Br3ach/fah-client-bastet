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

namespace FAH { namespace Client {
class CPUOwnershipPolicy {
public:
  // Compare allocation execution fields, excluding ownership, generation and
  // diagnostics. CPU workers affect execution; GPU workers are helper accounting.
  // GPU process priority is handled separately.
  // Pool-only changes do not alter process execution. The adapter must check
  // live conflicts before adopting a new pool; stopping processes stay frozen.
  static bool sameExecution(const RunningCPUAllocation &a,
    const RunningCPUAllocation &b, bool gpu) {
    return a.mode == b.mode &&
      a.mask == b.mask && (gpu || a.workers == b.workers);
  }
  // The caller checks process presence and excludes self-comparison.
  // GPU flags describe the corresponding WUs and must agree with managed GPU
  // modes; Unmanaged alone does not distinguish CPU from GPU work.
  static bool blocksLaunch(const RunningCPUAllocation &desired, bool desiredGPU,
    const RunningCPUAllocation &running, bool runningGPU);
};
}}
