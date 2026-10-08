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
#include "ReferenceExecutionPlan.h"

namespace FAH { namespace Client {
  // Pure per-WU ownership and budgets. Unique-ID requests retain vector priority
  // for worker counts and every partition phase; ID spelling has no priority.
  struct ReferenceWUPlanner {
    struct Request {
      std::string id; unsigned minimum, maximum;
      // Pending assignments/downloads reserve workers without displacing live cores.
      bool ownsCores = true;
    };
    struct Result {
      ReferenceExecutionPlan::Pools pools;
      std::map<std::string, unsigned> workers;
      unsigned remaining = 0;
      ReferenceExecutionPlan::RebalanceReport report;
    };
    static Result plan(const std::vector<Request> &requests, unsigned budget,
      const std::vector<unsigned> &cpus,
      const std::vector<ReferenceExecutionPlan::CPUSet> &cores);
  };
}}
