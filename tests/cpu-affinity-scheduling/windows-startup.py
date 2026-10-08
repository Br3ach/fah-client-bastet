# /******************************************************************************\
#
#                   This file is part of the Folding@home Client.
#
#           The fah-client runs Folding@home protein folding simulations.
#                     Copyright (c) 2001-2026, foldingathome.org
#                                All rights reserved.
#
#        This program is free software; you can redistribute it and/or modify
#        it under the terms of the GNU General Public License as published by
#         the Free Software Foundation; either version 3 of the License, or
#                        (at your option) any later version.
#
#          This program is distributed in the hope that it will be useful,
#           but WITHOUT ANY WARRANTY; without even the implied warranty of
#           MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#                    GNU General Public License for more details.
#
#      You should have received a copy of the GNU General Public License along
#      with this program; if not, write to the Free Software Foundation, Inc.,
#            51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
#
#                   For information regarding this software email:
#                                  Joseph Coffland
#                           joseph@cauldrondevelopment.com
#
# \******************************************************************************/

from pathlib import Path
from compile_harness import compile_and_run
root=Path(__file__).resolve().parents[2]
source=(root/'src/fah/client/CPUResources.cpp').read_text()
start=source.index('#ifdef _WIN32', source.index('CPUResources::CPUResources(bool'))
end=source.index('#endif', start)
body=source[start:end].split('\n',1)[1]
harness=r"""
#include <cassert>
#include <sstream>
#include <string>
#include <iostream>
unsigned groups=1, warnings=0;
std::string message;
unsigned GetActiveProcessorGroupCount(){return groups;}
#define LOG_WARNING(value) do {std::ostringstream out;out<<value;message=out.str();++warnings;}while(0)
void startup(bool hardAffinity){BODY}
int main(){
 startup(false);assert(warnings==0);
 groups=2;startup(true);assert(warnings==0);
 startup(false);assert(warnings==1);
 assert(message.find("multiple processor groups")!=std::string::npos);
 assert(message.find("legacy OS scheduling")!=std::string::npos);
 assert(message.find("reservations are unavailable")!=std::string::npos);
 std::cout<<"PASS: Windows startup diagnostic identifies unsupported processor groups without false positives\n";
}
""".replace('BODY',body)
compile_and_run(harness, includes=())
