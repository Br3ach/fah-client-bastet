#!/usr/bin/env python3
"""Matched old/current reservation checker benchmark; no live folding launched."""
from pathlib import Path
import argparse, os, subprocess, tempfile
parser=argparse.ArgumentParser()
parser.add_argument('--baseline',default='2ccf747')
args=parser.parse_args()
root=Path(__file__).resolve().parents[2]
old=subprocess.check_output(['git','-c','safe.directory='+str(root).replace('\\','/'),
 '-C',str(root),'show',args.baseline+':src/fah/client/CPUResources.cpp'],text=True)
current=(root/'src/fah/client/CPUResources.cpp').read_text()
a=old.index('        if (capacities.empty())',old.index('auto freshFits'))
b=old.index('        bool result = fits',a)
old_body=old[a:b].replace('        unsigned visits = 0;','')
old_function='bool baseline(vector<unsigned> capacities,const vector<unsigned>& deficits,unsigned& visits) {visits=0;if(deficits.empty())return true;'+old_body+'return fits(0,deficits);}'
a=current.index('bool CPUResources::partitionDeficits(')
b=current.index('\n\n\n// Retain physical-core ownership',a)
new_function=current[a:b].replace('CPUResources::partitionDeficits','current').replace('SMT_SEARCH_LIMIT','50000')
cpp=r"""
#include <algorithm>
#include <cassert>
#include <chrono>
#include <functional>
#include <iostream>
#include <random>
#include <set>
#include <vector>
using namespace std;
"""+old_function+new_function+r"""
int main(){
 mt19937 random(214);vector<pair<vector<unsigned>,vector<unsigned>>> cases;
 for(unsigned i=0;i<128;++i){
  vector<unsigned> capacities,needs;
  if(i<16) {
   for(unsigned j=0;j<32;++j)capacities.push_back(i<8?2:1+random()%4);
   for(unsigned j=0;j<12;++j)needs.push_back(2+random()%8);
  } else {
   unsigned total=0;
   for(unsigned j=0;j<24;++j){unsigned width=2+random()%3;capacities.push_back(width);total+=width;}
   set<unsigned> cuts;while(cuts.size()<11)cuts.insert(1+random()%(total-1));
   unsigned previous=0;for(auto cut:cuts){needs.push_back(cut-previous);previous=cut;}
   needs.push_back(total-previous);
  }
  cases.push_back({capacities,needs});
 }
 unsigned limits=0,maxVisits=0;
 for(auto &c:cases){unsigned a,b;bool x=baseline(c.first,c.second,a);
  bool y=current(c.first,c.second,b);assert(x==y&&a==b);
  limits+=b>50000;maxVisits=max(maxVisits,b);
 }
 cout<<"128 matched workloads; bound hits="<<limits<<" max visits="<<maxVisits<<"\n";
 for(unsigned run=0;run<3;++run)for(unsigned which=0;which<2;++which){
  auto begin=chrono::steady_clock::now();unsigned checksum=0;
  for(auto &c:cases){unsigned visits;
   checksum+=(which?current(c.first,c.second,visits):baseline(c.first,c.second,visits));
   checksum+=visits;
  }
  cout<<(which?"current":"baseline")<<" run="<<run+1<<" ms="
    <<chrono::duration<double,milli>(chrono::steady_clock::now()-begin).count()
    <<" checksum="<<checksum<<"\n";
 }
}
"""
with tempfile.TemporaryDirectory(prefix='fah-reservation-benchmark-') as directory:
 build=Path(directory);source=build/'bench.cpp';source.write_text(cpp)
 if os.name=='nt':
  subprocess.run(['cl','/nologo','/O2','/EHsc','/std:c++17',str(source),'/Febench.exe'],cwd=build,check=True)
  exe=build/'bench.exe'
 else:
  exe=build/'bench';subprocess.run(['g++','-O2','-std=c++17',str(source),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
