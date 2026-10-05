"""Compile the production CPU-WU budgeting and whole-core partition block."""
from pathlib import Path
import os, subprocess, tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'src/fah/client/Group.cpp').read_text()
a=s.index('  // Allocate remaining CPUs to existing CPU WUs')
b=s.index('  // Start and stop WUs',a)
body=s[a:b]
harness=r"""
#include <fah/client/CPUExecutionPlan.h>
#include <set>
#include <map>
#include <memory>
#include <vector>
#include <string>
#include <cassert>
#include <iostream>
using namespace std;using FAH::Client::CPUExecutionPlan;
#define LOG_DEBUG(a,b) do{}while(false)
const int UNIT_RUN=4;
struct Unit {string id;bool gpu=false,managed=false;unsigned minimum=1,maximum=64,cpus=0;set<unsigned>pool;
 bool hasGPUs()const{return gpu;}int getState()const{return UNIT_RUN;}
 unsigned getMinCPUs()const{return minimum;}unsigned getMaxCPUs()const{return maximum;}
 unsigned getCPUs()const{return cpus;}string getID()const{return id;}void setCPUs(unsigned n){cpus=n;}
 void setCPUAffinity(bool m,const set<unsigned>&p){managed=m;pool=p;}};
struct CPU {vector<set<unsigned>>cores;const auto &getCoreThreads()const{return cores;}};
struct Group {CPU cpuResources;vector<shared_ptr<Unit>>wus;auto units(){return wus;}
 void schedule(vector<unsigned> groupCPUs,unsigned remainingCPUs,bool managed=true){
 set<string>enabledWUs;
"""+body+r"""
 }
};
int main(){
 Group g;g.cpuResources.cores={{0,1},{2,3},{4,5},{6,7},{8,9},{10,11},{12,13},{14,15}};
 auto a=make_shared<Unit>();a->id="A";g.wus={a};
 vector<unsigned> order={0,2,4,6,8,10,12,14,1,3,5,7,9,11,13,15};
 for(unsigned n=1;n<=16;++n){g.schedule(order,n);assert(a->cpus==n);assert(a->pool.size()==16);
  auto plan=CPUExecutionPlan::create(0xa8,n,a->pool,g.cpuResources.cores,order);
  assert(plan.mask.size()==(n<=8?n:16));}
 auto b=make_shared<Unit>();b->id="B";a->maximum=8;b->maximum=8;g.wus={a,b};
 g.schedule(order,16);assert(a->cpus==8&&b->cpus==8);
 for(auto cpu:a->pool)assert(!b->pool.count(cpu));
 for(auto core:g.cpuResources.cores){unsigned owners=0;for(auto u:g.wus)for(auto cpu:core)if(u->pool.count(cpu)){++owners;break;}assert(owners==1);}
 // Group totals do not reveal every WU's full-SMT condition.
 a->maximum=8;b->maximum=6;g.schedule(order,14);
 auto rg=CPUExecutionPlan::create(0xa8,14,set<unsigned>(order.begin(),order.end()),g.cpuResources.cores,order);
 auto wu=CPUExecutionPlan::create(0xa8,b->cpus,b->pool,g.cpuResources.cores,order);
 assert(!rg.fullSMT && wu.fullSMT && b->cpus==6 && wu.physical==3);
 a->maximum=8;b->maximum=8;
 // Odd simultaneous WU counts cannot split a physical core to match budgets.
 a->maximum=3;b->maximum=3;g.schedule({0,2,4,1,3,5},6);
 assert(a->cpus==3&&b->cpus==2);assert(a->pool.size()==4&&b->pool.size()==2);
 // A CPU WU whose minimum cannot fit gets an empty pool and cannot launch.
 b->minimum=3;g.schedule({0,2,4,1,3,5},6);assert(b->pool.empty());
 // GPU helpers never consume the CPU process pool.
 auto gpu=make_shared<Unit>();gpu->id="GPU";gpu->gpu=true;g.wus={a,gpu};
 g.schedule(order,8);assert(gpu->pool.empty()&&!gpu->managed);
 cout<<"PASS: production CPU-WU worker budgets, expanded pools, simultaneous isolation, minimum constraints, GPU independence\n";
}
"""
with tempfile.TemporaryDirectory(prefix='fah-pool-scheduling-') as d:
 p=Path(d);(p/'test.cpp').write_text(harness)
 if os.name=='nt':cmd=['cl','/nologo','/EHsc','/std:c++17','/I'+str(root/'src'),'test.cpp','/Fetest.exe'];exe=p/'test.exe'
 else:cmd=['g++','-std=c++17','-I'+str(root/'src'),'test.cpp','-o','test'];exe=p/'test'
 cmd.insert(cmd.index("test.cpp")+1,str(Path(__file__).resolve().parents[2]/"src/fah/client/CPUExecutionPlan.cpp"))
 subprocess.run(cmd,cwd=p,check=True);subprocess.run([str(exe)],check=True)
