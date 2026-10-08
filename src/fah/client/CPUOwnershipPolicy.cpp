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

#include "CPUOwnershipPolicy.h"
using namespace FAH::Client;

bool CPUOwnershipPolicy::blocksLaunch(const RunningCPUAllocation &desired, bool desiredGPU,
    const RunningCPUAllocation &running, bool runningGPU) {
  // Shared GPU helpers may overlap CPU work. Exclusive GPU masks must wait
  // for both old CPU processes and old GPU processes to release their masks.
  if ((desiredGPU || runningGPU) &&
      !desired.isReservedGPU() && !running.isReservedGPU()) return false;
  if (!desired.isManaged() && !running.isManaged()) return false;
  // Unrestricted execution cannot be proven disjoint from exclusive ownership.
  if (!desired.isManaged() || !running.isManaged()) return true;
  // Hold whole-core ownership until the old process exits, including siblings
  // omitted from its smaller process mask at N <= physical capacity.
  for (auto cpu: desired.resourcePool)
    if (running.resourcePool.count(cpu)) return true;
  return false;
}
