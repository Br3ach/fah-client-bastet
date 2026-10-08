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
#include "CPUWholeCorePacking.h"
#include "CPUAllocationSlice.h"

namespace FAH { namespace Client {
  // Pure per-WU worker budgets and ownership.
  // General budgeting follows request order; class packing uses it for throughput ties.
  // WU ID spelling has no scheduling priority.
  struct WUCPUAllocationPlanner {
    struct Request {
      std::string id;
      unsigned minimum = 0, maximum = 0;
      // Preparation-stage requests reserve workers without owning cores.
      bool ownsCores = true;
    };
    struct Result {
      CPUWholeCorePacking::Pools pools;
      std::map<std::string, CPUAllocationSlices> allocationSlices;
      std::map<std::string, unsigned> workers;
      // Conservative budget available for new assignments, not final unused capacity.
      unsigned remaining = 0;
      // First search-limit report encountered; otherwise the final packing report.
      CPUWholeCorePacking::RebalanceReport report;
    };
    // Pending requests reserve class budgets without owning cores.
    static Result planClasses(const std::vector<Request> &requests,
      const CPUAllocationSlices &classes,
      const std::vector<CPUWholeCorePacking::CPUSet> &cores);
    static Result plan(const std::vector<Request> &requests, unsigned budget,
      const std::vector<unsigned> &cpus,
      const std::vector<CPUWholeCorePacking::CPUSet> &cores);
  };
}}
