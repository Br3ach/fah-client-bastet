"""Compile the shipped per-WU metadata method with small JSON/lifecycle adapters."""
from pathlib import Path
import os,subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'src/fah/client/Unit.cpp').read_text()
a=s.index('cb::JSON::ValuePtr Unit::getCPUExecutionInfo() const {')
b=s.index('\n\n\nuint32_t Unit::getMinCPUs()',a)
method=s[a:b]
harness=r"""
#include <fah/client/CPUExecutionPlan.h>
#include <memory>
#include <map>
#include <string>
#include <vector>
#include <cassert>
#include <algorithm>
#include <iostream>
using namespace std;using FAH::Client::CPUExecutionPlan;
namespace cb{namespace JSON{struct Value{map<string,double> numbers;map<string,string> strings;vector<unsigned> list;
 void insert(const string& k,unsigned n){numbers[k]=n;}void insert(const string& k,uint64_t n){numbers[k]=n;}
 void insert(const string& k,const string& s){strings[k]=s;}void insert(const string&,shared_ptr<Value>){}
 void insertBoolean(const string& k,bool b){numbers[k]=b;}void append(unsigned n){list.push_back(n);}};using ValuePtr=shared_ptr<Value>;}}
struct Config{unsigned getConfiguredCPUTotal()const{return 14;}string getCPUMode()const{return "count";}vector<unsigned>getCPUClassCounts()const{return {};}};
struct Group{string getName()const{return "A";}};
struct Core{unsigned type=0xa8;unsigned getType()const{return type;}};
struct CorePtr{Core value;bool isSet()const{return true;}const Core*operator->()const{return &value;}};
struct Resources{vector<set<unsigned>>cores={{0,1},{2,3},{4,5}};unsigned generation=2;
 const auto&getCoreThreads()const{return cores;}unsigned getAllocationGeneration()const{return generation;}};
struct App{Resources resources;const Resources&getCPUResources()const{return resources;}};
struct Unit{bool affinityManaged=true,gpu=false,paused=false;unsigned affinityAllocationGeneration=2;
 set<unsigned>affinityCPUs={0,1,2,3,4,5};CorePtr core;App app;shared_ptr<Group>group=make_shared<Group>();Config config;unsigned workers=6;
 bool hasGPUs()const{return gpu;}bool atRunState()const{return true;}bool isPaused()const{return paused;}
 unsigned getCPUs()const{return workers;}uint64_t getU64(const string&)const{return 42;}
 const Config&getConfig()const{return config;}cb::JSON::ValuePtr createDict()const{return make_shared<cb::JSON::Value>();}cb::JSON::ValuePtr createList()const{return createDict();}
 cb::JSON::ValuePtr getCPUExecutionInfo()const;
};
METHOD
int main(){Unit u;auto info=u.getCPUExecutionInfo();assert(info&&info->numbers["full_smt"]&&info->numbers["physical_cpus"]==3&&info->numbers["allocated_workers"]==6);
 u.workers=2;info=u.getCPUExecutionInfo();assert(info->numbers["physical_cpus"]==2&&info->numbers["logical_cpus"]==2&&!info->numbers["full_smt"]);
 u.workers=6;u.core.value.type=0xa7;assert(!u.getCPUExecutionInfo()->numbers["full_smt"]);
 u.affinityAllocationGeneration=1;assert(!u.getCPUExecutionInfo());u.affinityAllocationGeneration=2;
 u.paused=true;assert(!u.getCPUExecutionInfo());u.paused=false;u.gpu=true;assert(!u.getCPUExecutionInfo());
 cout<<"PASS: actual WU core policy, mask physical counts, stale-generation exclusion, paused/GPU exclusion\n";}
""".replace('METHOD',method)
with tempfile.TemporaryDirectory(prefix='fah-wu-metadata-') as d:
 p=Path(d);(p/'test.cpp').write_text(harness)
 plan=str(root/'src/fah/client/CPUExecutionPlan.cpp')
 if os.name=='nt':cmd=['cl','/nologo','/EHsc','/std:c++17','/I'+str(root/'src'),'test.cpp',plan,'/Fetest.exe'];exe=p/'test.exe'
 else:cmd=[os.environ.get('CXX','c++'),'-std=c++17','-I'+str(root/'src'),'test.cpp',plan,'-o','test'];exe=p/'test'
 subprocess.run(cmd,cwd=p,check=True);subprocess.run([str(exe)],check=True)
