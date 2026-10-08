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
#include <map>
#include <vector>
#include <string>
#include "Group.h"
namespace FAH { namespace Client { class Groups { std::map<std::string,Group> g; std::vector<std::string> order; public:
  void set(const std::string&n,const Config&c){if(!g.count(n))order.push_back(n);g[n]=Group(c);} std::vector<std::string> keys() const {return order;}
  const Group& getGroup(const std::string&n) const{return g.at(n);} Group& getGroup(const std::string&n){return g.at(n);} }; }}
