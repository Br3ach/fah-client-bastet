#include "CPUResources.h"
#include "CPUExecutionPlan.h"
#include "Groups.h"
#include <cbang/os/SystemInfo.h>
#include <cassert>
#include <iostream>
#include <sstream>
using namespace FAH::Client;
using namespace std;
struct Corrupt : CPUResources {
 using CPUResources::validateAllocations;
 void split(){managed=true;allocations={{"A",{0}},{"B",{1}}};workerBudgets={{"A",1},{"B",1}};}
};
static void isolated(const CPUResources &r,const Groups &g) {
 set<unsigned> seen;
 for(const auto &name:g.keys()) for(auto cpu:r.getGroupCPUs(name)) {
  assert(seen.insert(cpu).second); assert(!r.getGPUReservedCPUs().count(cpu));
 }
 for(const auto &core:r.getCoreThreads()) {
  unsigned owners=0;
  for(const auto &name:g.keys()) {
   set<unsigned> pool(r.getGroupCPUs(name).begin(),r.getGroupCPUs(name).end());
   for(auto cpu:core) if(pool.count(cpu)){++owners;break;}
  }
  assert(owners<=1);
 }
}
int main() {
 auto &si=cb::SystemInfo::instance();
 si.available.clear();si.cores.clear();
 for(unsigned i=0;i<16;++i)si.available.insert(i);
 for(unsigned i=0;i<8;++i)si.cores.push_back({2*i,2*i+1});
 si.levels={si.available};
 Corrupt corrupt;corrupt.split();assert(!corrupt.validateAllocations());
 assert(corrupt.isManaged() && corrupt.isRuntimeFallback() && corrupt.getGroupWorkerCount("A")==0);
 Groups g;g.set("A",Config::Count(8)); CPUResources r;r.update(g);
 assert(r.isManaged());assert(r.getGroupWorkerCount("A")==8);
 assert(r.getGroupCPUs("A").size()==16);
 auto pool=r.getGroupCPUs("A");r.update(g);assert(pool==r.getGroupCPUs("A"));
 g.set("B",Config::Count(8));r.update(g);isolated(r,g);
 assert(r.getGroupWorkerCount("A")==8 && r.getGroupWorkerCount("B")==8);
 assert(r.getGroupCPUs("A").size()==8 && r.getGroupCPUs("B").size()==8);
 unsigned cases=0;ostringstream quiet;auto output=cout.rdbuf(quiet.rdbuf());
 for(unsigned a=0;a<=20;++a)for(unsigned b=0;b<=20;++b) {
  g.set("A",Config::Count(a));g.set("B",Config::Count(b));r.update(g);isolated(r,g);
  assert(r.getGroupWorkerCount("A")<=a && r.getGroupWorkerCount("B")<=b);
  assert(r.getGroupWorkerCount("A")+r.getGroupWorkerCount("B")<=16);
  assert(g.getGroup("A").getConfig().getConfiguredCPUTotal()==a);++cases;
 }
 cout.rdbuf(output);
 si.levels={{0,1,2,3,4,5,6,7},{8,9,10,11,12,13,14,15}};r.refreshTopology();
 g.set("A",Config::Class({3,0}));g.set("B",Config::Class({0,5}));r.update(g);isolated(r,g);
 for(auto cpu:r.getGroupCPUs("A"))assert(cpu<8);
 for(auto cpu:r.getGroupCPUs("B"))assert(cpu>=8);
 assert(r.getGroupWorkerCount("A")==3 && r.getGroupWorkerCount("B")==5);
 g.set("A",Config::Class({1,1,1}));r.update(g);assert(r.isRuntimeFallback());isolated(r,g);
 assert(g.getGroup("A").getConfig().getCPUClassCounts().size()==3);
 si.cap=cb::SystemInfo::CPU_AFFINITY_NONE;r.refreshTopology();r.update(g);
 assert(!r.isManaged() && r.isRuntimeFallback());
 si.cap=cb::SystemInfo::CPU_AFFINITY_HARD;si.levels.clear();r.refreshTopology();
 g.set("A",Config::Count(8));g.set("B",Config::Count(0));r.update(g);assert(!r.isManaged());
 // Class-specific physical spreading precedes General allocation, independent
 // of RG name order. It must preserve achievable General worker budgets.
 si.available={0,1,2,3,4,5,6,7,8,9,10,11};
 si.cores={{0,1},{2,3},{4,5},{6,7},{8},{9},{10},{11}};
 si.levels={{0,1,2,3,4,5,6,7},{8,9,10,11}};
 for(bool reversed:{false,true})for(unsigned general:{4u,6u,8u}) {
  Groups mixed;string cls=reversed?"B":"A",gen=reversed?"A":"B";
  mixed.set(cls,Config::Class({4,0}));mixed.set(gen,Config::Count(general));
  CPUResources allocation;allocation.update(mixed);isolated(allocation,mixed);
  assert(allocation.getGroupWorkerCount(cls)==4);
  assert(allocation.getGroupWorkerCount(gen)==general);
  assert(!allocation.isRuntimeFallback());
  auto list=allocation.getGroupCPUs(cls);set<unsigned> owned(list.begin(),list.end());
  auto plan=CPUExecutionPlan::create(0xa8,4,owned,allocation.getCoreThreads(),list);
  assert(plan.physical==(general==4?4u:general==6?3u:2u));
  if(general==4)assert(plan.mask==set<unsigned>({0,2,4,6}));
 }
 // Check that class spreading never reduces the single General group's
 // achievable budget over all fitting requests on this mixed topology.
 auto log=cout.rdbuf(quiet.rdbuf());
 unsigned mixedCases=0;
 for(unsigned cls=0;cls<=8;++cls)for(unsigned general=0;general<=12-cls;++general) {
  Groups mixed;mixed.set("A",Config::Class({cls,0}));mixed.set("B",Config::Count(general));
  CPUResources allocation;allocation.update(mixed);isolated(allocation,mixed);
  assert(allocation.getGroupWorkerCount("A")==cls);
  unsigned remaining=12-2*((cls+1)/2);
  assert(allocation.getGroupWorkerCount("B")==min(general,remaining));
  ++mixedCases;
 }
 cout.rdbuf(log);
 cout<<"PASS: "<<mixedCases<<" mixed-class budgets preserve General capacity\n";
 cout<<"PASS: "<<cases<<" worker budgets, whole-core isolation, class fallback, capability loss\n";
}
