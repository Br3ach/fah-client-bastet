#include "CPUExecutionPlan.h"
#include <cassert>
#include <iostream>
using P=FAH::Client::CPUExecutionPlan;
// Exhaust small mixed-width topologies and three competing worker requests.
// Under-allocation can be necessary at whole-core granularity. Check safety
// and execution-policy invariants without requiring the greedy repair to be optimal.
static unsigned partitionProperties() {
 unsigned cases=0;
 for(unsigned x:{1u,2u,3u,6u})for(unsigned y:{1u,2u,3u,6u})for(unsigned z:{1u,2u,3u,6u}) {
  std::vector<P::CPUSet> cores;unsigned next=0;
  for(unsigned width:{x,y,z}) {P::CPUSet core;while(width--)core.insert(next++);cores.push_back(core);}
  // Include an unavailable/reserved whole core and different physical-core orders.
  for(unsigned removed:{0u,1u})for(unsigned rotation=0;rotation<3;++rotation) {
   std::vector<unsigned> order;
   for(unsigned i=0;i<3;++i) {
    unsigned index=(i+rotation)%3;
    if(removed && index==0)continue;
    order.insert(order.end(),cores[index].begin(),cores[index].end());
   }
   P::CPUSet available(order.begin(),order.end());unsigned capacity=available.size();
   for(unsigned a=0;a<=capacity;++a)for(unsigned b=0;b<=capacity-a;++b)for(unsigned c=0;c<=capacity-a-b;++c)
    for(bool fill:{false,true}) {
     std::map<std::string,unsigned> requests={{"A",a},{"B",b},{"C",c}};
     auto pools=P::partition(requests,order,cores,fill);P::CPUSet occupied;
     for(const auto &entry:pools) {
      assert(requests.at(entry.first)>0 || entry.second.empty());
      for(auto cpu:entry.second)assert(available.count(cpu)&&occupied.insert(cpu).second);
      for(const auto &core:cores) {
       unsigned owned=0;for(auto cpu:core)owned+=entry.second.count(cpu);
       assert(owned==0||owned==core.size());
      }
      unsigned workers=std::min<unsigned>(requests.at(entry.first),entry.second.size());
      assert(workers<=entry.second.size());
      for(unsigned type:{0xa8u,0xa9u,0x99u}) {
       auto plan=P::create(type,workers,entry.second,cores,order);
       assert(!plan.oversubscribed);
       for(auto cpu:plan.mask)assert(entry.second.count(cpu));
       if(!workers){assert(plan.mask.empty());continue;}
       assert(plan.mask.size()==((type==0xa8||type==0xa9)&&workers>plan.physical?entry.second.size():workers));
       if(workers<=plan.physical)for(const auto &core:cores) {
        unsigned used=0;for(auto cpu:core)used+=plan.mask.count(cpu);assert(used<=1);
       }
      }
     }
     ++cases;
    }
  }
 }
 return cases;
}
int main(){
 auto properties=partitionProperties();
 for(unsigned physical:{7u,8u}) {
  P::CPUSet pool;std::vector<P::CPUSet> cores;
  for(unsigned i=0;i<physical;++i){cores.push_back({2*i,2*i+1});pool.insert(2*i);pool.insert(2*i+1);}
  for(unsigned type:{0xa8u,0xa9u})for(unsigned n=1;n<=2*physical;++n){
   auto p=P::create(type,n,pool,cores);assert(p.physical==physical);
   assert(p.mask.size()==(n<=physical?n:2*physical));assert(p.fullSMT==(n==2*physical));
   if(n<=physical)for(auto cpu:p.mask)assert(cpu%2==0);
  }
  assert(P::create(0xa8,2*physical+1,pool,cores).oversubscribed);
  assert(P::create(0xa8,2*physical+1,pool,cores).mask.empty());
  assert(P::create(0x99,physical+1,pool,cores).mask.size()==physical+1);
  auto fallback=P::create(0xa8,physical+1,pool,{});assert(fallback.mask.size()==physical+1 && !fallback.fullSMT);
  std::vector<unsigned> order(pool.begin(),pool.end());
  auto split=P::partition({{"A",physical},{"B",physical}},order,cores);
  for(auto cpu:split["A"])assert(!split["B"].count(cpu));
  for(auto core:cores){unsigned owners=0;for(auto entry:split)for(auto cpu:core)if(entry.second.count(cpu)){++owners;break;}assert(owners<=1);}
 }
 auto mixed=P::create(0xa8,7,{0,1,2,3,4,5,6,7},{{0,1,2,3,4,5},{6,7}});
 assert(mixed.mask.size()==8 && mixed.physical==2 && !mixed.fullSMT);
 auto partial=P::create(0xa8,3,{0,1,2,3},{{0,1}});assert(partial.mask.size()==3 && !partial.hasSMT);
 auto overlap=P::create(0xa8,3,{0,1,2},{{0,1},{1,2}});assert(overlap.mask.size()==3 && !overlap.hasSMT);
 auto preferred=P::create(0xa8,1,{0,1,2,3},{{0,1},{2,3}},{2,0,3,1});assert(preferred.mask==P::CPUSet({2}));
 std::vector<P::CPUSet> hybrid;std::vector<unsigned> ordered;
 for(unsigned i=0;i<7;++i)hybrid.push_back({2*i,2*i+1});
 for(unsigned i=14;i<22;++i)hybrid.push_back({i});
 for(const auto &core:hybrid)for(auto cpu:core)ordered.push_back(cpu);
 for(unsigned a=1;a<22;++a)for(unsigned b=1;a+b<=22;++b){
  auto pools=P::partition({{"A",a},{"B",b}},ordered,hybrid,false);
  assert(pools["A"].size()>=a && pools["B"].size()>=b);
  for(auto cpu:pools["A"])assert(!pools["B"].count(cpu));
  for(const auto &core:hybrid)for(const auto &entry:pools){
   unsigned n=0;for(auto cpu:core)n+=entry.second.count(cpu);
   assert(n==0||n==core.size());
  }
 }
 std::cout<<"PASS: "<<properties<<" mixed-width partition/property cases\n";
 std::cout<<"PASS: a8/a9 physical-first/full-pool policy, full-SMT warning, mixed widths, fallback, per-WU isolation\n";
}
