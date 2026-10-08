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
#include <vector>

namespace FAH { namespace Client {
// Worker budget and CPU ownership for one resource slice.
// Preparation-stage WUs may have a budget before acquiring ownership.
struct CPUAllocationSlice {
  unsigned workers = 0;
  CPUSet pool;
  bool operator==(const CPUAllocationSlice &other) const {
    return workers == other.workers && pool == other.pool;
  }
};
// Slice indexes identify caller-defined resources; class-mode callers preserve
// performance-level indexes, including empty slices. General mode uses one slice.
using CPUAllocationSlices = std::vector<CPUAllocationSlice>;
}}
