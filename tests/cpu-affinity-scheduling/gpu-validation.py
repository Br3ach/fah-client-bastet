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

"""Exercise production reservation-capacity validation."""
from pathlib import Path
from compile_harness import compile_and_run
root=Path(__file__).resolve().parents[2]
harness=r'''
#include <fah/client/CPUConfigValidator.h>
#include <set>
#include <vector>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <string>
using namespace std;using V=FAH::Client::CPUConfigValidator;
struct CPU {bool supported=true;set<unsigned> available={0,1,2,3,4,5,6,7,8,9};
 vector<set<unsigned>> levels={{0,1,2,3,4,5,6,7},{8,9}},cores={{0,4},{1,5},{2,6},{3,7}};};
bool selectionChanged(const set<string>&a,const set<string>&b){return a!=b;}
bool accept(const CPU &cpu,uint64_t reservedCores,uint64_t total,vector<uint64_t> counts,bool shared=false,bool changed=true,bool gpuChanged=false) {
 V::Topology t;t.gpuAffinity=cpu.supported;t.configurableClasses=true;t.available=cpu.available.size();t.rawClassCapacity={8,2};
 for(const auto &level:cpu.levels)t.effectiveClassCapacity.push_back(level.size());
 for(const auto &core:cpu.cores)t.fastCoreWidths.push_back(core.size());
 V::Request gpu;gpu.name="GPU";gpu.mode="count";gpu.gpus={"gpu"};gpu.reservedCores=reservedCores;
 V::Request cpus;cpus.name="CPU";cpus.mode="classes";cpus.hasClassCounts=true;
 uint64_t sum=0;for(auto count:counts){cpus.classCounts.push_back(count);sum+=count;}cpus.workers=sum;
 V::Request extra;extra.name="Extra";extra.mode="count";extra.workers=total>sum?total-sum:0;
 V::Request helper;helper.name="Shared";helper.mode="count";if(shared)helper.gpus={"helper"};
 vector<V::Request> proposed={gpu,cpus,extra,helper}, current=proposed;
 if(changed)current.clear();else if(gpuChanged)current.back().gpus.clear();
 return V::validate(t,current,proposed).valid;
}
int main() {
 // Two large GPU reservations must not wrap their active demand to zero.
 const V::Topology topology={true,true,10,{8,2},{8,2},{2,2,2,2}};
 V::Request huge;huge.name="GPU";huge.mode="count";huge.reservedCores=2147483648u;huge.gpus={"one","two"};
 const vector<V::Request> proposed={huge},current;
 auto result=V::validate(topology,current,proposed);
 assert(!result.valid && result.error=="GPU CPU reservations exceed available complete Performance 1 cores");
 assert(proposed[0].reservedCores==2147483648u && proposed[0].gpus.size()==2);
 assert(topology.available==10 && topology.fastCoreWidths==vector<uint64_t>({2,2,2,2}));
 // Equal aggregate demand does not make per-device policy unchanged.
 V::Request saved;saved.name="GPU";saved.mode="count";
 saved.gpus={"GPU0"};saved.reservedCores=2;
 auto edited=saved;edited.gpus.insert("GPU1");edited.reservedCores=1;
 auto unavailable=topology;unavailable.gpuAffinity=false;
 assert(!V::validate(unavailable,{saved},{edited}).valid);
 assert(V::validate(topology,{saved},{edited}).valid);
 assert(V::validate(unavailable,{saved},{saved}).valid);
 auto cleared=saved;cleared.reservedCores=0;
 assert(V::validate(unavailable,{saved},{cleared}).valid);
 // Device selection still changes active demand with an unchanged per-device setting.
 auto extra=saved;extra.gpus.insert("GPU1");
 assert(!V::validate(unavailable,{saved},{extra}).valid);
 V::Request wide;wide.name="CPU";wide.mode="classes";wide.hasClassCounts=true;wide.classCounts={4294967295u,4294967295u};
 result=V::validate(topology,{}, {wide});
 assert(!result.valid && result.error.find("4294967295")!=string::npos);
'''+r''' CPU c;
 assert(!accept(c,4,0,{0,0},true,false,selectionChanged({"GPU2"},{})));
 assert(accept(c,4,0,{0,0},true,false,selectionChanged({"GPU2"},{"GPU2"})));
 assert(accept(c,3,0,{0,0},true,false,selectionChanged({"GPU2"},{})));
 assert(accept(c,1,8,{6,2}));assert(!accept(c,1,9,{6,2}));
 assert(!accept(c,1,8,{8,0}));assert(accept(c,4,2,{0,2}));assert(!accept(c,5,0,{0,0}));
 assert(!accept(c,4,0,{0,0},true));assert(accept(c,3,0,{0,0},true));
 assert(accept(c,4,0,{0,0},false));
 CPU partial=c;partial.available.insert(10);partial.levels[0].insert(10);
 assert(accept(partial,4,0,{0,0},true)); // An unreserved SMT fragment can serve shared helpers.
 c.supported=false;assert(!accept(c,1,0,{0,0}));assert(accept(c,0,10,{8,2}));
 c.supported=true;c.cores={{0},{1,5},{2}};assert(accept(c,2,7,{5,2}));assert(!accept(c,2,8,{5,2}));
 cout<<"PASS: real pure validator GPU capacity, shared starvation, mixed widths\n";
}
'''
compile_and_run(harness, ['CPUConfigValidator.cpp'])
