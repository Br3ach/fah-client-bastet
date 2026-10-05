"""Exercise production reservation-capacity and API numeric rejection bodies."""
from pathlib import Path
import os, subprocess, tempfile
source=(Path(__file__).resolve().parents[2]/'src/fah/client/Groups.cpp').read_text()
a=source.index('  if (reservedCores && cpu.supportsGPUAffinity()) {')
b=source.index('  LOG_DEBUG(1, "CPU config validation begin',a)
capacity=source[a:b]
a=source.index('  if (reservedCores && (reservationChanged || cpuPolicyChanged)) {')
b=source.index('  // Preserve legacy oversubscription',a)
validate=source[a:b]
a=source.index('    if (proposed->has("gpu_reserved_cores")) {')
b=source.index('    Config candidate',a)
numeric=source[a:b]
start=source.index('    uint64_t active =')
expression=source[start:source.index(';',start)].split(' = ',1)[1]
expression=expression.replace('selectedGPUs.size()', 'selected')
a=source.index('  // Enabling shared helpers must be checked')
b=source.index('  if (reservedCores && (reservationChanged || cpuPolicyChanged)) {',a)
unchanged=source[a:b].replace('return;', 'return true;')
a=source.index('    if (selectedGPUs != oldGPUs) gpuPolicyChanged = true;')
selection=source[a:source.index('\n',a)]
harness=r'''
#include <set>
#include <vector>
#include <memory>
#include <cassert>
#include <cstdint>
#include <stdexcept>
#include <iostream>
#include <string>
using namespace std;
#define LOG_DEBUG(a,b) do {} while(false)
#define THROW(x) throw runtime_error("Rejected reservation")
struct CPU {bool supported=true;set<unsigned> available={0,1,2,3,4,5,6,7,8,9};
 vector<set<unsigned>> levels={{0,1,2,3,4,5,6,7},{8,9}},cores={{0,4},{1,5},{2,6},{3,7}};
 bool supportsGPUAffinity()const{return supported;}const auto &getAvailableCPUs()const{return available;}
 const auto &getPerformanceLevels()const{return levels;}const auto &getFastPhysicalCores()const{return cores;}};
bool accept(CPU cpu,uint64_t reservedCores,uint64_t total,vector<uint64_t> classTotals,bool sharedGPURequested=false,bool changed=true,bool gpuPolicyChanged=false) {
 uint64_t capacity=10;vector<uint64_t> classCapacity={8,2};bool reservationChanged=changed,cpuPolicyChanged=changed;
 try {
''' + capacity + unchanged + validate + r'''
 return true;}catch(const runtime_error &){return false;}
}
bool selectionChanged(set<string> selectedGPUs,set<string> oldGPUs) {bool gpuPolicyChanged=false;
''' + selection + r'''
 return gpuPolicyChanged;}
struct Value {bool present=true,integer=true;double number=0;
 bool has(const char *)const{return present;}shared_ptr<Value>get(const char *)const{return make_shared<Value>(*this);}
 bool isInteger()const{return integer;}double getNumber()const{return number;}};
bool numberValid(Value input) {auto proposed=&input;try {
''' + numeric + r'''
 return true;}catch(const runtime_error &){return false;}}
uint64_t reservationCount(uint64_t selected,uint32_t requested){return ""PLACEHOLDER"";}
int main() {
 assert(reservationCount(0,3)==0);assert(reservationCount(2,1)==2);
 assert(reservationCount(3,2)==6);
 assert(reservationCount(2,4294967295u)==8589934590ULL);
 assert(selectionChanged({"GPU2"},{}));assert(selectionChanged({"GPU2"},{"GPU1"}));
 assert(!selectionChanged({"GPU1"},{"GPU1"}));
 CPU c;
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
 assert(numberValid({true,true,0}));assert(numberValid({true,true,4}));
 assert(!numberValid({true,true,-1}));assert(!numberValid({true,false,1.5}));
 assert(!numberValid({true,true,4294967296.0}));
 cout<<"PASS: prospective GPU-first capacity, class/total rejection, all-core maximum, shared-GPU starvation rejection, shortages, mixed widths, strict API integers\n";
}
'''
harness=harness.replace('""PLACEHOLDER""',expression)
with tempfile.TemporaryDirectory(prefix='fah-gpu-validation-qa-') as d:
 p=Path(d);(p/'test.cpp').write_text(harness)
 if os.name=='nt':cmd=['cl','/nologo','/EHsc','/std:c++17','test.cpp','/Fetest.exe'];exe=p/'test.exe'
 else:cmd=[os.environ.get('CXX','c++'),'-std=c++17','test.cpp','-o','test'];exe=p/'test'
 subprocess.run(cmd,cwd=p,check=True);subprocess.run([str(exe)],check=True)
