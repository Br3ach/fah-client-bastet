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

"""Compile the whole production validator with controlled topology/config fixtures."""
from pathlib import Path
import argparse
from compile_harness import compile_and_run
root=Path(__file__).resolve().parents[2]
parser=argparse.ArgumentParser()
parser.add_argument('--source',type=Path,default=root/'src/fah/client/Groups.cpp')
parser.add_argument('--matrix',action='store_true')
args=parser.parse_args()
source=args.source.read_text()
a=source.index('void Groups::validateCPUConfiguration');b=source.index('void Groups::triggerUpdate()',a)
body=source[a:b]
config=(root/'src/fah/client/Config.cpp').read_text()
a=config.index('string Config::getCPUMode()');b=config.index('string Config::getCPUConfigDescription()',a)
accessors=config[a:b]
harness=r'''
#include <fah/client/CPUConfigValidator.h>
#include <fah/client/GPUProcessPriority.h>
using FAH::Client::GPUProcessPriority;
using FAH::Client::CPUConfigValidator;
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
using namespace std;
#define LOG_DEBUG(a,b) do{}while(false)
#define LOG_WARNING(b) do{}while(false)
#define LOG_INFO(a,b) do{}while(false)
#define THROW(x) do{ostringstream out;out<<x;throw runtime_error(out.str());}while(false)
template<class T> using SmartPointer=shared_ptr<T>;
namespace JSON {
struct Value {
 bool dict=true,list=false,integer=false;double number=0;string text;
 map<string,shared_ptr<Value>> fields;vector<shared_ptr<Value>> items;
 bool isDict()const{return dict;}bool isInteger()const{return integer;}double getNumber()const{return number;}
 bool has(const string &key)const{return fields.count(key);}
 bool hasString(const string &key)const{return has(key)&&!get(key)->dict&&!get(key)->list&&!get(key)->integer;}
 bool hasList(const string &key)const{return has(key)&&get(key)->list;}
 shared_ptr<Value> get(const string &key)const{return fields.at(key);}
 uint32_t getU32()const{return static_cast<uint32_t>(number);}
 uint32_t getU32(const string &key,uint32_t fallback=0)const{return has(key)?get(key)->getU32():fallback;}
 string getString(const string &key,const string &fallback="")const{return has(key)?get(key)->text:fallback;}
 vector<string> keys()const{vector<string> out;for(const auto &e:fields)out.push_back(e.first);return out;}
 auto begin()const{return items.begin();}auto end()const{return items.end();}
};
using ValuePtr=shared_ptr<Value>;
struct Reader{static ValuePtr parse(const string &){return make_shared<Value>();}};
}
struct Resource{string get(const string &)const{return "{}";}}resource0;
struct CPUResources{
 bool configurable=true,supported=true,changed=false;
 vector<set<unsigned>> levels={{0,1,2,3},{4,5,6,7}};
 vector<set<unsigned>> cores={{0,1},{2,3}};
 set<unsigned> available={0,1,2,3,4,5,6,7};
 bool refreshTopology(const string &){return changed;}
 const auto &getRawPerformanceLevels()const{return levels;}
 const auto &getPerformanceLevels()const{return levels;}
 const auto &getAvailableCPUs()const{return available;}
 const auto &getFastPhysicalCores()const{return cores;}
 bool supportsGPUAffinity()const{return supported;}
 bool hasPerformanceClasses()const{return configurable;}
 unsigned getTopologyGeneration()const{return 1;}
};
struct App{CPUResources cpu;unsigned updates=0;set<string> usable={"GPU0","GPU1"};
 CPUResources &getCPUResources(){return cpu;}void triggerUpdate(){++updates;}};
unsigned constructed=0,loaded=0;
struct Config:JSON::Value{
 App &app;
 Config(App &a,const JSON::ValuePtr &):app(a){++constructed;}
 void load(const JSON::Value &v){fields=v.fields;++loaded;}
 uint32_t getGPUReservedCores()const{return getU32("gpu_reserved_cores",0);}
 set<string> getGPUs()const{set<string> result;if(has("gpus"))for(const auto &name:get("gpus")->keys())
   if(get("gpus")->get(name)->number && app.usable.count(name))result.insert(name);return result;}
 string getCPUMode()const;bool usesCPUClasses()const;vector<uint32_t> getCPUClassCounts()const;
 uint32_t getConfiguredCPUTotal()const;string getCPUConfigDescription()const{return "test";}
};
ACCESSORS
struct Group{shared_ptr<Config> config;const Config &getConfig()const{return *config;}};
struct Groups{
 App &app;map<string,Group> groups;mutable unsigned lookups=0;
 explicit Groups(App &a):app(a){}
 bool has(const string &name)const{return groups.count(name);}
 const Group &getGroup(const string &name)const{++lookups;return groups.at(name);}
 vector<string>keys()const{vector<string> result;for(const auto &e:groups)result.push_back(e.first);return result;}
 void save(const string &name,const JSON::Value &v){auto c=make_shared<Config>(app,JSON::ValuePtr());c->load(v);groups[name]={c};}
 void validateCPUConfiguration(const map<string,SmartPointer<Config>> &staged)const;
};
BODY
static shared_ptr<JSON::Value> numeric(uint32_t n){auto v=make_shared<JSON::Value>();v->dict=false;v->integer=true;v->number=n;return v;}
static JSON::Value policy(unsigned workers,const string &mode="count",vector<uint32_t>counts={},unsigned reserved=0,set<string>gpus={}){
 JSON::Value v;v.fields["cpus"]=numeric(workers);v.fields["gpu_reserved_cores"]=numeric(reserved);
 auto m=make_shared<JSON::Value>();m->dict=false;m->text=mode;v.fields["cpu_mode"]=m;
 auto c=make_shared<JSON::Value>();c->dict=false;c->list=true;for(auto n:counts)c->items.push_back(numeric(n));v.fields["cpu_class_counts"]=c;
 auto g=make_shared<JSON::Value>();for(auto &name:gpus)g->fields[name]=numeric(1);v.fields["gpus"]=g;return v;
}
static JSON::Value proposals(initializer_list<pair<string,JSON::Value>>entries){JSON::Value result;
 for(const auto &e:entries)result.fields[e.first]=make_shared<JSON::Value>(e.second);return result;}
static map<string,SmartPointer<Config>> stage(Groups &g,const JSON::Value &v){
 map<string,SmartPointer<Config>> result;
 for(const auto &name:v.keys()){
  auto c=make_shared<Config>(g.app,JSON::ValuePtr());c->load(*v.get(name));result.emplace(name,c);
 }return result;
}
static string decision(Groups &g,const JSON::Value &v){try{g.validateCPUConfiguration(stage(g,v));return "accepted";}catch(const exception &e){return e.what();}}
int main(){
 CPUConfigValidator::Result defaultResult;
 assert(!defaultResult.valid && defaultResult.error.empty());
 if(MATRIX){
  unsigned index=0;
  for(unsigned topology=0;topology<3;++topology)for(unsigned n=0;n<10;++n)for(unsigned m=0;m<10;++m)
   for(unsigned reserve=0;reserve<4;++reserve)for(bool classes:{false,true}){
    App app;if(topology==1)app.cpu.configurable=false;if(topology==2){app.cpu.supported=false;app.cpu.configurable=false;}
    Groups g(app);g.save("A",policy(2,"classes",{1,1}));g.save("GPU",policy(0,"count",{},1,{"GPU0"}));
    auto proposed=proposals({{"A",policy(n,classes?"classes":"count",classes?vector<uint32_t>{n/2,n-n/2}:vector<uint32_t>{})},
      {"B",policy(m)},{"GPU",policy(0,"count",{},reserve,{"GPU0"})},{"Shared",policy(0,"count",{},0,{"GPU1"})}});
    cout<<index++<<'|'<<decision(g,proposed)<<'\n';
   }return 0;
 }
 // Reservation topology must support subtraction from both usable capacities.
 {
  CPUConfigValidator::Topology topology;
  topology.gpuAffinity=topology.configurableClasses=true;
  topology.available=8;topology.rawClassCapacity={5,3};
  topology.effectiveClassCapacity={5,3};topology.fastCoreWidths={2,1,2};
  CPUConfigValidator::Request gpu;gpu.name="GPU";gpu.mode="count";
  gpu.reservedCores=2;gpu.gpus={"GPU0"};
  CPUConfigValidator::Request cpu;cpu.name="CPU";cpu.mode="count";cpu.workers=5;
  assert(CPUConfigValidator::validate(topology,{}, {gpu,cpu}).valid);
  auto malformed=topology;
  for(unsigned variant=0;variant<5;++variant){
   malformed=topology;
   if(variant==0)malformed.effectiveClassCapacity={5};
   if(variant==1)malformed.effectiveClassCapacity.clear();
   if(variant==2)malformed.fastCoreWidths={0,1};
   if(variant==3)malformed.available=2;
   if(variant==4)malformed.effectiveClassCapacity={2,3};
   auto result=CPUConfigValidator::validate(malformed,{}, {gpu,cpu});
   assert(!result.valid && result.error=="Inconsistent GPU reservation topology");
  }
  // Legitimate capability loss retains unchanged intent, including reservations.
  topology.gpuAffinity=topology.configurableClasses=false;
  topology.effectiveClassCapacity.clear();topology.fastCoreWidths.clear();
  assert(CPUConfigValidator::validate(topology,{gpu,cpu},{gpu,cpu}).valid);
 }
 // Zero-demand class policy still constrains saved aggregate CPU totals.
 {
  CPUConfigValidator::Topology topology;
  topology.configurableClasses=true;topology.rawClassCapacity={8,8};
  CPUConfigValidator::Request a;a.name="A";a.mode="count";a.workers=12;
  auto b=a;b.name="B";
  CPUConfigValidator::Request classes;classes.name="Classes";classes.mode="classes";
  classes.hasClassCounts=true;classes.classCounts={0,0};
  assert(CPUConfigValidator::validate(topology,{}, {a,b}).valid);
  auto result=CPUConfigValidator::validate(topology,{}, {a,b,classes});
  assert(!result.valid && result.error=="Configured managed CPU total 24 exceeds CPU topology capacity 16");
 }
 // Both input lists reject duplicates, including the unnamed Default group.
 for (const string name: {string(), string("A")}) {
  CPUConfigValidator::Topology topology;
  CPUConfigValidator::Request first;first.name=name;first.mode="count";
  auto second=first;second.workers=1; // Conflicting policy must not be silently ignored.
  assert(CPUConfigValidator::validate(topology,{first},{first}).valid);
  for (bool proposed: {false,true}) {
   auto result=proposed ? CPUConfigValidator::validate(topology,{first},{first,second}) :
     CPUConfigValidator::validate(topology,{first,second},{first});
   assert(!result.valid);
   assert(result.error == string("Duplicate ") + (proposed ? "proposed" : "current") +
     " CPU configuration request name: '" + name + "'");
  }
 }
 App app;Groups g(app);g.save("A",policy(4,"classes",{2,2,0}));g.save("B",policy(0));
 auto same=proposals({{"A",policy(4,"classes",{2,2,0})},{"B",policy(1)}});
 constructed=loaded=g.lookups=0;assert(decision(g,same)=="accepted");
 assert(constructed==2 && loaded==2 && g.lookups==2); // One staged Config/load per group.
 auto prepared=stage(g,same);
 constructed=loaded=g.lookups=0;
 g.validateCPUConfiguration(prepared);
 assert(constructed==0 && loaded==0 && g.lookups==2); // Validation reuses staged objects.
 assert(prepared.at("A")->getCPUClassCounts()==vector<uint32_t>({2,2,0}));
 auto changed=proposals({{"A",policy(4,"classes",{3,1,0})},{"B",policy(0)}});
 assert(decision(g,changed)=="CPU class count does not match current performance-level count");
 app.cpu.configurable=false;assert(decision(g,same)=="accepted");
 assert(decision(g,changed)=="CPU performance-class allocation is not currently configurable");
 app.cpu.configurable=true;app.cpu.changed=true;assert(decision(g,same)=="accepted" && app.updates==1);app.cpu.changed=false;
 // General-only legacy oversubscription remains valid.
 assert(decision(g,proposals({{"A",policy(100)},{"B",policy(100)}}))=="accepted");
 assert(decision(g,proposals({{"A",policy(4,"classes",{4,0})},{"B",policy(5)}}))!="accepted");
 assert(decision(g,proposals({{"A",policy(0,"invalid")}})).find("Invalid CPU mode")!=string::npos);
 // Usable devices, not raw JSON enable flags, determine reservation demand.
 auto noDevice=proposals({{"GPU",policy(0,"count",{},1,{"missing"})}});
 assert(decision(g,noDevice)=="Select an available GPU before reserving CPU cores");
 auto reserved=proposals({{"GPU",policy(0,"count",{},2,{"GPU0"})},{"Shared",policy(0,"count",{},0,{"GPU1"})}});
 assert(decision(g,reserved).find("no Performance 1 CPUs")!=string::npos);
 auto cpuShort=proposals({{"A",policy(7)},{"GPU",policy(0,"count",{},1,{"GPU0"})}});
 assert(decision(g,cpuShort)=="CPU allocations exceed capacity after GPU CPU reservations");
 g.save("GPU",policy(0,"count",{},1,{"GPU0"}));app.usable.clear();
 assert(decision(g,proposals({{"GPU",policy(0,"count",{},1,{"GPU0"})}}))=="accepted");
 cout<<"PASS: whole-validator Config reuse, unchanged class intent, topology loss, count oversubscription and GPU capacity/device rules\n";
}
'''.replace('ACCESSORS',accessors).replace('BODY',body).replace('MATRIX','true' if args.matrix else 'false')
compile_and_run(harness, ['CPUConfigValidator.cpp'])
