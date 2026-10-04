from pathlib import Path
import os,subprocess,tempfile
here=Path(__file__).resolve().parent;source=(here.parents[1]/'src/fah/client/Unit.cpp').read_text()
a=source.index('std::set<unsigned> Unit::getDesiredAffinity()');b=source.index('\n\n\nvoid Unit::run()',a)
body=source[a:b]
harness="""#include <set>
#include <vector>
#include <cassert>
#include <iostream>
struct CPU {bool supported=true;std::vector<std::set<unsigned>> levels={{0,1,2,3,4,5,6,7},{8,9}};bool hasPerformanceClasses()const{return supported;}const auto &getPerformanceLevels()const{return levels;}};
struct App {CPU cpu;const CPU &getCPUResources()const{return cpu;}};
struct Config {bool pin=false;bool getPinToPerfCores()const{return pin;}};
struct Unit {App app;Config config;bool affinityManaged=false,gpu=false;unsigned cpus=4;std::set<unsigned> affinityCPUs;const Config &getConfig()const{return config;}bool hasGPUs()const{return gpu;}unsigned getCPUs()const{return cpus;}std::set<unsigned> getDesiredAffinity()const;};
"""+body+"""
int main(){Unit u;assert(u.getDesiredAffinity().empty());u.config.pin=true;auto pinned=u.getDesiredAffinity();assert(pinned.size()==8);u.config.pin=false;assert(u.getDesiredAffinity()!=pinned);u.config.pin=true;u.cpus=16;assert(u.getDesiredAffinity().empty());u.gpu=true;assert(u.getDesiredAffinity().size()==8);u.app.cpu.levels[0]={0,1};assert(u.getDesiredAffinity()==std::set<unsigned>({0,1}));u.app.cpu.supported=false;assert(u.getDesiredAffinity().empty());u.affinityManaged=true;u.affinityCPUs={9};assert(u.getDesiredAffinity()==std::set<unsigned>({9}));std::cout<<"PASS: pin enable/disable, CPU count, GPU helper, topology refresh, managed mask\\n";}
"""
with tempfile.TemporaryDirectory(prefix='fah-pin-mask-') as d:
 p=Path(d);(p/'test.cpp').write_text(harness)
 if os.name=='nt':cmd=['cl','/nologo','/EHsc','/std:c++17','test.cpp','/Fetest.exe'];exe=p/'test.exe'
 else:cmd=[os.environ.get('CXX','c++'),'-std=c++17','test.cpp','-o','test'];exe=p/'test'
 subprocess.run(cmd,cwd=p,check=True);subprocess.run([str(exe)],check=True)
