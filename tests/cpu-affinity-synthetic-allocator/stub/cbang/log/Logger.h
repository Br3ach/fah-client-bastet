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
#include <iostream>
#include <sstream>
#include <stdexcept>
inline unsigned testDebugLevel = 1;
inline bool testFailRebalanceLog = false;
#define LOG_DEBUG(a,b) do { if ((a)<=testDebugLevel) {std::ostringstream _x; _x << b; if (testFailRebalanceLog && _x.str().find("CPU pool rebalance:")==0) throw std::runtime_error("Injected rebalance logging failure"); std::cout << "D " << _x.str() << "\n";} } while(0)
#define LOG_INFO(a,b) do { std::ostringstream _x; _x << b; std::cout << "I " << _x.str() << "\n"; } while(0)
#define LOG_WARNING(b) do { std::ostringstream _x; _x << b; std::cout << "W " << _x.str() << "\n"; } while(0)
#define LOG_ERROR(b) do { std::ostringstream _x; _x << b; std::cout << "E " << _x.str() << "\n"; } while(0)
namespace cb {}
