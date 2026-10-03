#include "CPUResources.h"
#include "Groups.h"
#include <cbang/os/SystemInfo.h>
#include <cassert>
#include <algorithm>
#include <iostream>
#include <functional>
#include <sstream>
#include <set>
using namespace FAH::Client; using namespace std;
static set<unsigned> S(initializer_list<unsigned> x){return set<unsigned>(x);} 
static set<unsigned> U(const CPUResources::CPUList&a,const CPUResources::CPUList&b){set<unsigned> u(a.begin(),a.end());for(auto x:b){assert(!u.count(x));u.insert(x);}return u;}
struct SearchLimitLogging : CPUResources {using CPUResources::shouldLogSMTSearchLimit;};
int main(){
 auto &si=cb::SystemInfo::instance();
 SearchLimitLogging limitLogs;
 auto instant=chrono::steady_clock::time_point{};
 assert(limitLogs.shouldLogSMTSearchLimit(instant));
 assert(!limitLogs.shouldLogSMTSearchLimit(instant));
 assert(!limitLogs.shouldLogSMTSearchLimit(instant+chrono::seconds(299)));
 assert(limitLogs.shouldLogSMTSearchLimit(instant+chrono::seconds(300)));
 assert(!limitLogs.shouldLogSMTSearchLimit(instant+chrono::seconds(301)));
 assert(limitLogs.shouldLogSMTSearchLimit(instant+chrono::seconds(600)));

 si.available=S({0,1,2,3}); si.levels={S({0,1}),S({2,3})}; si.cores={S({0}),S({1}),S({2}),S({3})};
 Groups gs; gs.set("A",Config::Class({1,1})); gs.set("B",Config::Count(1));
 CPUResources r;ostringstream initial;auto savedOutput=cout.rdbuf(initial.rdbuf());
 r.update(gs,"initial");cout.rdbuf(savedOutput);
 assert(initial.str().find("I CPU allocation RG 'A'")!=string::npos);
 assert(initial.str().find("I CPU allocation RG 'B'")!=string::npos);
 assert(initial.str().find("logical-mask=")!=string::npos);
 assert(initial.str().find("class1=")!=string::npos && initial.str().find("class2=")!=string::npos);
 auto a1=r.getGroupCPUs("A"), b1=r.getGroupCPUs("B");
 assert(a1.size()==2 && b1.size()==1); assert(U(a1,b1).size()==3); assert(!r.isRuntimeFallback());
 // Release-visible summaries should not repeat for unchanged allocations.
 ostringstream repeated;auto output=cout.rdbuf(repeated.rdbuf());
 r.update(gs,"same");cout.rdbuf(output);
 assert(repeated.str().find("I CPU allocation RG '")==string::npos);
 assert(a1==r.getGroupCPUs("A")); assert(b1==r.getGroupCPUs("B"));
 // Policy/mode changes also produce summaries when the mask becomes empty.
 Groups general;general.set("",Config::Count(0));CPUResources legacyLogs;
 ostringstream legacyOutput;savedOutput=cout.rdbuf(legacyOutput.rdbuf());
 legacyLogs.update(general);legacyLogs.update(general);cout.rdbuf(savedOutput);
 string legacyText=legacyOutput.str();auto summary=legacyText.find("I CPU allocation RG 'Default'");
 assert(summary!=string::npos);
 assert(legacyText.find("I CPU allocation RG 'Default'",summary+1)==string::npos);
 assert(legacyText.find("OS-scheduled; no managed RG mask")!=string::npos);
 general.getGroup("").getConfig().setCount(1);
 ostringstream changedPolicy;savedOutput=cout.rdbuf(changedPolicy.rdbuf());
 legacyLogs.update(general);cout.rdbuf(savedOutput);
 // Stub policy descriptions contain only the mode, so exercise a mode change.
 general.getGroup("").getConfig().setClass({0,0});
 savedOutput=cout.rdbuf(changedPolicy.rdbuf());legacyLogs.update(general);cout.rdbuf(savedOutput);
 assert(changedPolicy.str().find("I CPU allocation RG 'Default'")!=string::npos);
 // Remove B's CPU if possible; A should remain stable when its CPUs stay available.
 unsigned drop=b1.front(); si.available.erase(drop); r.refreshTopology("cpuset-loss"); r.update(gs,"after-loss");
 auto a2=r.getGroupCPUs("A"), b2=r.getGroupCPUs("B");
 if (find(a1.begin(),a1.end(),drop)==a1.end()) assert(a2==a1);
 assert(U(a2,b2).size()==a2.size()+b2.size());
 // Force a class shortage: only one CPU from class 0 remains while A asks for both.
 gs.getGroup("A").getConfig().setClass({2,1});
 si.available=S({0,2,3}); r.refreshTopology("class-shortage"); r.update(gs,"fallback");
 assert(r.isRuntimeFallback());
 auto af=r.getGroupCPUs("A"), bf=r.getGroupCPUs("B"); U(af,bf);
 // Restore all CPUs. Class-aware mode returns.
 si.available=S({0,1,2,3}); r.refreshTopology("restore"); r.update(gs,"recovery");
 assert(!r.isRuntimeFallback());
 auto ar=r.getGroupCPUs("A"); unsigned c0=0,c1=0; for(auto x:ar){if(si.levels[0].count(x))c0++;if(si.levels[1].count(x))c1++;}
 assert(c0==2 && c1==1);
 // Saved vector is incompatible after a three-to-two-class transition.
 si.available=S({0,1,2,3,4,5});
 si.levels={S({0,1}),S({2,3}),S({4,5})}; si.cores.clear();
 Groups lost; lost.set("A",Config::Class({1,1,1}));
 CPUResources lr; lr.update(lost);
 si.available=S({0,1,2,3}); si.levels={S({0,1}),S({2,3})};
 lr.refreshTopology("lost-class"); lr.update(lost);
 assert(lr.isRuntimeFallback()); assert(lr.getGroupCPUs("A").size()==3);
 si.available=S({0,1,2,3,4,5}); si.levels={S({0,1}),S({2,3}),S({4,5})};
 lr.refreshTopology("restored-class"); lr.update(lost);
 assert(!lr.isRuntimeFallback()); assert(lr.getGroupCPUs("A").size()==3);
 // Fair reduction precedes preservation: neither equal-demand RG starves.
 si.available=S({0,1,2,3}); si.levels={S({0,1}),S({2,3})};
 Groups fair; fair.set("A",Config::Class({1,1})); fair.set("B",Config::Count(2));
 CPUResources fr; fr.update(fair);
 auto prior=fr.getGroupCPUs("A"); si.available=S({prior[0],prior[1]});
 fr.refreshTopology("fair-shrink"); fr.update(fair);
 assert(fr.isRuntimeFallback());
 assert(fr.getGroupCPUs("A").size()==1 && fr.getGroupCPUs("B").size()==1);
 auto fa=fr.getGroupCPUs("A"), fb=fr.getGroupCPUs("B");
 fr.update(fair); assert(fa==fr.getGroupCPUs("A") && fb==fr.getGroupCPUs("B"));
 // Eight SMT P cores plus eight singleton E cores; keep sibling ownership.
 si.available.clear(); si.levels={ {}, {} }; si.cores.clear();
 for(unsigned i=0;i<24;i++) {si.available.insert(i);si.levels[i<16?0:1].insert(i);}
 for(unsigned i=0;i<8;i++) si.cores.push_back(S({i,i+8}));
 for(unsigned i=16;i<24;i++) si.cores.push_back(S({i}));
 Groups siblings; siblings.set("A",Config::Class({8,0}));
 siblings.set("B",Config::Class({4,0}));
 CPUResources sr; sr.update(siblings);
 auto sa=sr.getGroupCPUs("A"), sb=sr.getGroupCPUs("B");
 assert(sa.size()==8 && sb.size()==4); U(sa,sb);
 for(unsigned i=0;i<8;i++) {
   bool a0=find(sa.begin(),sa.end(),i)!=sa.end();
   bool a1=find(sa.begin(),sa.end(),i+8)!=sa.end();
   bool b0=find(sb.begin(),sb.end(),i)!=sb.end();
   bool b1=find(sb.begin(),sb.end(),i+8)!=sb.end();
   assert(!(a0&&b1) && !(a1&&b0));
 }
 sr.update(siblings); assert(sa==sr.getGroupCPUs("A") && sb==sr.getGroupCPUs("B"));
 // Fresh-core distribution must leave enough ownership capacity for a large RG.
 Groups three; three.set("A",Config::Class({10,0}));
 three.set("B",Config::Class({2,0})); three.set("C",Config::Class({2,0}));
 CPUResources tr; tr.update(three);
 assert(tr.getGroupCPUs("A").size()==10);
 assert(tr.getGroupCPUs("B").size()==2 && tr.getGroupCPUs("C").size()==2);
 for(unsigned i=0;i<8;i++) {
   string owner;
   for(auto name: {"A","B","C"})
     for(auto cpu: tr.getGroupCPUs(name))
       if(cpu==i || cpu==i+8) {assert(owner.empty() || owner==name); owner=name;}
 }
 // Odd budgets sometimes require splitting; demand and disjointness still hold.
 siblings.getGroup("A").getConfig().setClass({15,0});
 siblings.getGroup("B").getConfig().setClass({1,0});
 sr.update(siblings); assert(sr.getGroupCPUs("A").size()==15);
 assert(sr.getGroupCPUs("B").size()==1); U(sr.getGroupCPUs("A"),sr.getGroupCPUs("B"));
 // A general RG should use a permitted slower fresh core rather than split
 // a fast physical core already owned by a class-restricted RG.
 Groups generalSibling; generalSibling.set("A",Config::Class({15,0}));
 generalSibling.set("B",Config::Count(1));
 CPUResources gr; gr.update(generalSibling);
 assert(gr.getGroupCPUs("A").size()==15 && gr.getGroupCPUs("B").size()==1);
 assert(gr.getGroupCPUs("B").front()>=16);
 // Exhaust all three-RG budgets fitting eight uniform SMT cores.
 unsigned cases=0;
 for(unsigned x=0;x<=16;x++) for(unsigned y=0;y<=16;y++)
   for(unsigned z=0;z<=16;z++) {
     if(x+y+z>16) continue;
     Groups all; all.set("A",Config::Class({x,0}));
     all.set("B",Config::Class({y,0})); all.set("C",Config::Class({z,0}));
     CPUResources er; er.update(all);
     assert(er.getGroupCPUs("A").size()==x && er.getGroupCPUs("B").size()==y &&
       er.getGroupCPUs("C").size()==z);
     set<unsigned> seen;
     map<unsigned,string> ownership;
     for(auto name: {"A","B","C"})
       for(auto cpu: er.getGroupCPUs(name)) {
         assert(si.available.count(cpu) && seen.insert(cpu).second);
         if((x+1)/2+(y+1)/2+(z+1)/2<=8) {
           unsigned core=cpu%8;
           assert(!ownership.count(core) || ownership[core]==name);
           ownership[core]=name;
         }
       }
     cases++;
   }
 cout << "Exhaustive budgets checked: " << cases << "\n";
 // Partial and overlapping sibling maps must be discarded as a whole.
 si.available=S({0,1,2,3}); si.levels={S({0,1}),S({2,3})};
 si.cores={S({0,1}),S({2})};
 CPUResources partial; assert(partial.getCoreThreads().empty());
 Groups pg; pg.set("A",Config::Class({1,1})); pg.set("B",Config::Count(2));
 partial.update(pg);
 assert(partial.getGroupCPUs("A").size()==2 && partial.getGroupCPUs("B").size()==2);
 U(partial.getGroupCPUs("A"),partial.getGroupCPUs("B"));
 si.cores={S({0,1}),S({1,2,3})};
 partial.refreshTopology("overlap"); assert(partial.getCoreThreads().empty());
 partial.update(pg); U(partial.getGroupCPUs("A"),partial.getGroupCPUs("B"));
 // Unequal SMT widths: exercise all three-RG budgets and safety invariants.
 si.available=S({0,1,2,3,4,5,6,7,8,9,10});
 si.levels={S({0,1,2,3,4,5,6,7,8}),S({9,10})};
 si.cores={S({0}),S({1,2}),S({3,4,5,6}),S({7,8}),S({9}),S({10})};
 unsigned mixedCases=0;
 for(unsigned x=0;x<=9;x++) for(unsigned y=0;y<=9;y++)
   for(unsigned z=0;z<=9;z++) {
     if(x+y+z>9) continue;
     Groups mixed; mixed.set("A",Config::Class({x,0}));
     mixed.set("B",Config::Class({y,0})); mixed.set("C",Config::Class({z,0}));
     CPUResources mr; mr.update(mixed);
     assert(mr.getGroupCPUs("A").size()==x && mr.getGroupCPUs("B").size()==y &&
       mr.getGroupCPUs("C").size()==z);
     set<unsigned> used;
     for(auto name: {"A","B","C"})
       for(auto cpu: mr.getGroupCPUs(name))
         assert(si.levels[0].count(cpu) && used.insert(cpu).second);
     // Exact independent feasibility oracle: a core may belong to one RG,
     // with any unused siblings left idle. Verify no split whenever feasible.
     vector<unsigned> widths={1,2,4,2};
     function<bool(unsigned,vector<unsigned>)> feasible =
       [&] (unsigned core, vector<unsigned> need) {
         if (need==vector<unsigned>{0,0,0}) return true;
         if (core==widths.size()) return false;
         for(unsigned group=0;group<need.size();group++) {
           auto next=need; next[group]-=min(next[group],widths[core]);
           if(feasible(core+1,next)) return true;
         }
         return feasible(core+1,need);
       };
     if(feasible(0,{x,y,z})) {
       map<unsigned,string> owner;
       for(auto name:{"A","B","C"})
         for(auto cpu:mr.getGroupCPUs(name)) {
           unsigned core=cpu==0?0:cpu<=2?1:cpu<=6?2:3;
           assert(!owner.count(core) || owner[core]==name);
           owner[core]=name;
         }
     }
     mixedCases++;
   }
 cout << "Heterogeneous budgets checked: " << mixedCases << "\n";
 si.cap=cb::SystemInfo::CPU_AFFINITY_NONE;
 sr.refreshTopology("lost-hard-affinity"); sr.update(siblings);
 assert(sr.isRuntimeFallback() && !sr.isManaged());
 cout << "PASS: stability, fallback, recovery, fairness, sibling ownership, unavoidable split, capability loss\n";
}
