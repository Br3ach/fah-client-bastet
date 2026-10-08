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


"""Check extracted production FahCore launch plumbing against exact arguments."""
from pathlib import Path
from compile_harness import compile_and_run
root=Path(__file__).resolve().parents[2]
source=(root/'src/fah/client/Unit.cpp').read_text()
a=source.index('vector<string> Unit::buildCoreArgs(')
b=source.index('bool Unit::desiredGPUReservation()',a)
methods=source[a:b]
a=source.index('  void addGPUArgs(');b=source.index('  bool existsAndOlderThan(',a)
gpu_args=source[a:b]
harness=r"""
#include <fah/client/RunningCPUAllocation.h>
#include <fah/client/GPUProcessPriority.h>
using FAH::Client::GPUProcessPriority;
#include <cassert>
#include <stdexcept>
#define THROW(x) throw std::runtime_error("invalid launch")
#include <map>
#include <memory>
#include <string>
#include <vector>
#include <iostream>
using namespace std;
using FAH::Client::RunningCPUAllocation;
template<class T>using SmartPointer=shared_ptr<T>;
template<class T>SmartPointer<T> SmartPtr(T*p){return SmartPointer<T>(p);}
#define LOG_INFO(a,b) do{}while(false)
string String(unsigned value){return to_string(value);}
struct SystemUtilities{static unsigned getPID(){return 1234;}};
struct Config{string priority;string getGPUPriority()const{return priority;}};
struct Value {
 vector<string> ids;unsigned platform=0,device=0;
 unsigned size()const{return ids.size();}
 string getString(unsigned i)const{return ids.at(i);}
 unsigned getU32(const string& key)const{return key=="platform"?platform:device;}
};
struct GPUResource {
 bool uuid=true,cuda=false,hip=false;map<string,Value> drivers;
 bool has(const string& key)const{return drivers.count(key);}
 bool hasString(const string&)const{return uuid;}
 string getString(const string&key)const{return key=="uuid"?"GPU-test":"nvidia";}
 const Value* get(const string&key)const{return &drivers.at(key);}
 bool isComputeDeviceSupported(const string&key,const Config&)const{return key=="cuda"?cuda:hip;}
};
struct GPURef{const GPUResource* value;template<class T>const T* cast()const{return value;}};
struct GPUs{GPUResource gpu;GPURef get(const string&id)const{assert(id=="gpu-id");return {&gpu};}};
struct Version{string toString()const{return "8.5.7";}};
struct App{GPUs gpus;const GPUs&getGPUs()const{return gpus;}Version getVersion()const{return {};}};
struct Core{unsigned type=0x28;unsigned getType()const{return type;}string getPath()const{return "core-path";}};
struct CoreProcess {
 string priority;void setPriorityOverride(const string &value){priority=value;}
 string path;set<unsigned>mask;unsigned affinityCalls=0;
 explicit CoreProcess(const string&p):path(p){}
 void setRequiredAffinity(const set<unsigned>&m){mask=m;++affinityCalls;}
};
struct Unit {
 App app;Value gpus;Core coreValue;const Core*core=&coreValue;Config config;
 const Value*get(const string&)const{return &gpus;}
 bool hasGPUs()const{return !gpus.ids.empty();}
 const Config&getConfig()const{return config;}
 string getID()const{return "wu-id";}
 vector<string>buildCoreArgs(const RunningCPUAllocation&)const;
 SmartPointer<CoreProcess>createCoreProcess(const RunningCPUAllocation&)const;
};
"""+gpu_args+methods+r"""
int main(){
 Unit unit;RunningCPUAllocation captured;captured.workers=7;
 unit.gpus.ids={"gpu-id","other"};bool rejected=false;
 try{unit.buildCoreArgs(captured);}catch(const std::runtime_error&){rejected=true;}
 assert(rejected);unit.gpus.ids.clear();
 const vector<string> base={"-dir","wu-id","-suffix","01","-version","8.5.7","-lifeline","1234"};
 auto expected=base;expected.insert(expected.end(),{"-np","7"});
 assert(unit.buildCoreArgs(captured)==expected);
 auto legacy=unit.createCoreProcess(captured);
 assert(legacy->path=="core-path"&&legacy->affinityCalls==0);
 captured.mode=FAH::Client::CPUAllocationMode::ManagedCPU;captured.mask={2,4};
 auto managed=unit.createCoreProcess(captured);
 assert(managed->mask==captured.mask&&managed->affinityCalls==1);
 unit.gpus.ids={"gpu-id"};auto &gpu=unit.app.gpus.gpu;
 for(bool uuid:{false,true})for(bool cuda:{false,true})for(bool hip:{false,true})for(bool opencl:{false,true}){
  gpu.uuid=uuid;gpu.cuda=cuda;gpu.hip=hip;gpu.drivers.clear();
  if(opencl)gpu.drivers["opencl"]={{},1,2};
  gpu.drivers["cuda"]={{},3,4};gpu.drivers["hip"]={{},5,6};
  expected=base;
  if(uuid)expected.insert(expected.end(),{"-gpu-uuid","GPU-test"});
  expected.insert(expected.end(),{"-gpu-platform",cuda?"cuda":"opencl","-gpu-vendor","nvidia"});
  if(opencl)expected.insert(expected.end(),{"-opencl-platform","1","-opencl-device","2"});
  if(cuda)expected.insert(expected.end(),{"-cuda-platform","3","-cuda-device","4"});
  if(hip)expected.insert(expected.end(),{"-hip-platform","5","-hip-device","6"});
  if(opencl)expected.insert(expected.end(),{"-gpu","2"});
  assert(unit.buildCoreArgs(captured)==expected);
 }
#ifdef __linux__
 for(string priority:{"other-low","other-normal"}) {
  unit.config.priority=priority;
  auto actual=unit.buildCoreArgs(captured);
  assert(actual[base.size()]=="--priority");
  assert(actual[base.size()+1]==GPUProcessPriority::coreArgument(priority));
  assert(unit.createCoreProcess(captured)->priority==priority);
  unit.coreValue.type=0x99;
  assert(unit.buildCoreArgs(captured)==expected);
  assert(unit.createCoreProcess(captured)->priority.empty());
  unit.coreValue.type=0x28;
 }
#endif
 unit.config.priority="normal";unit.gpus.ids.clear();
 assert(unit.createCoreProcess(captured)->priority.empty()); // CPU WUs ignore GPU settings.
 cout<<"PASS: captured CPU workers, exact legacy GPU argument ordering, UUID and driver combinations, managed/legacy process setup\n";
}
"""
compile_and_run(harness)
