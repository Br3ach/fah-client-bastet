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


"""Compile the whole priority monitor; exercise Unit progress/publication adapters."""
from pathlib import Path
from compile_harness import compile_and_run
root=Path(__file__).resolve().parents[2]
source=(root/'src/fah/client/Unit.cpp').read_text()
a=source.index('void Unit::monitorGPUPriority()');b=source.index('void Unit::monitorRun()',a)
c=source.index('bool Unit::readCoreProgress(');d=source.index('void Unit::readViewerData(',c)
e=source.index('  uint64_t launchDone = 0, total = 0;',source.index('void Unit::run()'));f=source.index('  const auto args = buildCoreArgs(',e)
harness=r"""
#include <fah/client/GPUProcessPriority.h>
#include <fah/client/GPUProcessPriorityMonitor.h>
#include <fah/client/CoreProcess.h>
#include <cassert>
#include <map>
#include <string>
#include <iostream>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <cstdio>
using namespace std;using FAH::Client::GPUProcessPriority;using FAH::Client::GPUProcessPriorityMonitor;using Process=FAH::Client::CoreProcess;
unsigned logs=0;
#define LOG_INFO(a,b) (++logs)
#define THROW(message) throw runtime_error(message)
#define TRY_CATCH_ERROR(code) try {code;} catch (const exception&) {}
struct SystemUtilities {
 static bool exists(const string&path){return bool(ifstream(path,ios::binary));}
 static unique_ptr<ifstream> iopen(const string&path){return make_unique<ifstream>(path,ios::binary);}
};
struct Time {static unsigned long long clock;static auto now(){return clock;}};
unsigned long long Time::clock=0;
struct Config {string priority;string getGPUPriority()const{return priority;}};
struct Core {unsigned type=0x28;unsigned getType()const{return type;}};
struct Ref {Process value;bool isSet()const{return true;}Process*operator->(){return &value;}Process&operator*(){return value;}};
struct Unit {
 GPUProcessPriorityMonitor gpuPriorityMonitor;bool gpu=true;Ref process;Config config;Core coreValue;Core *core=&coreValue;
 uint64_t lastKnownTotal=1000;
 uint64_t lastKnownDone=100;
 map<string,string> fields;
 bool hasGPUs()const{return gpu;}
 const Config&getConfig()const{return config;}
 double getKnownProgress()const{return lastKnownDone/1000.0;}
 string getString(const string &key,const string &fallback)const{auto it=fields.find(key);return it==fields.end()?fallback:it->second;}
 void insert(const string&key,const string&value){fields[key]=value;}
 string getDirectory()const{return ".";}
 void updateKnownProgress(uint64_t done,uint64_t total){lastKnownDone=done;lastKnownTotal=total;}
 bool readCoreProgress(uint64_t&,uint64_t&);void readInfo();void initializePriorityMonitor();
 Unit(){gpuPriorityMonitor.reset(true,100);}
 void monitorGPUPriority();
};
"""+source[a:b]+source[c:d]+'void Unit::initializePriorityMonitor(){\n'+source[e:f]+'}\n'+r"""
void sharedRecord(unsigned type,unsigned total,unsigned done,bool truncated=false){
 uint32_t words[23]={};words[0]=type;words[21]=total;words[22]=done;
 ofstream f("wuinfo_01.dat",ios::binary|ios::trunc);f.write((char*)words,truncated?12:sizeof(words));
}
int main(){
 remove("wuinfo_01.dat");Unit reader;uint64_t done=123,total=456;
 assert(!reader.readCoreProgress(done,total)&&done==123&&total==456);
 sharedRecord(0x28,1000,340,true);assert(!reader.readCoreProgress(done,total));
 sharedRecord(0x28,0,0);assert(!reader.readCoreProgress(done,total));
 sharedRecord(0x28,1000,1001);assert(!reader.readCoreProgress(done,total));
 sharedRecord(0x27,1000,340);bool threw=false;
 try{reader.readCoreProgress(done,total);}catch(const runtime_error&){threw=true;}assert(threw);
 sharedRecord(0x28,1000,0);assert(reader.readCoreProgress(done,total)&&done==0&&total==1000);
 sharedRecord(0x28,1000,340);assert(reader.readCoreProgress(done,total)&&done==340&&total==1000);
 reader.readInfo();assert(reader.lastKnownDone==340&&reader.lastKnownTotal==1000);
 remove("wuinfo_01.dat");
 Unit cpu;cpu.gpu=false;cpu.config.priority="normal";cpu.lastKnownDone=200;cpu.monitorGPUPriority();assert(!cpu.process.value.calls);
 Unit none;none.lastKnownDone=200;none.monitorGPUPriority();assert(!none.process.value.calls);
 Unit u;
#ifdef _WIN32
 u.config.priority=u.process.value.priority=u.fields["gpu_priority_requested"]="normal";
 u.monitorGPUPriority();assert(u.process.value.calls==1);
 Time::clock=1;u.monitorGPUPriority();assert(u.process.value.calls==1);
 Time::clock=5;u.monitorGPUPriority();assert(u.process.value.calls==2);
 u.lastKnownDone=101;u.monitorGPUPriority();assert(u.process.value.calls==3);
 u.lastKnownDone=140;u.monitorGPUPriority();assert(u.process.value.calls==3);
 u.lastKnownDone=151;u.monitorGPUPriority();assert(u.process.value.calls==4&&u.process.value.apply);
 u.config.priority="high";u.monitorGPUPriority();assert(u.process.value.priority=="high"&&u.process.value.calls==5);
 u.process.value.warning="Permission denied";u.lastKnownDone=202;u.monitorGPUPriority();assert(logs==1&&u.fields["gpu_priority_applied"].empty());
 u.lastKnownDone=253;u.monitorGPUPriority();assert(logs==1);
 u.process.value.warning="";u.lastKnownDone=304;u.monitorGPUPriority();assert(u.fields["gpu_priority_warning"].empty());
 u.config.priority="";u.monitorGPUPriority();assert(u.process.value.priority.empty()&&u.fields["gpu_priority_requested"].empty());
 auto calls=u.process.value.calls;u.lastKnownDone=900;u.monitorGPUPriority();assert(u.process.value.calls==calls);
 // The production launch setup reads a persisted checkpoint without
 // treating it as progress from the new process; processStarted resets counts.
 sharedRecord(0x28,1000,340);Unit resumed;
 resumed.config.priority=resumed.process.value.priority=resumed.fields["gpu_priority_requested"]="high";
 resumed.lastKnownDone=777;resumed.initializePriorityMonitor();

 assert(resumed.lastKnownDone==777); // Baseline capture does not alter accounting.
 resumed.lastKnownDone=0;resumed.lastKnownTotal=0;
 resumed.readInfo();Time::clock=0;resumed.monitorGPUPriority();
 assert(resumed.process.value.calls==1);
 Time::clock=5;resumed.monitorGPUPriority();assert(resumed.process.value.calls==2);
 sharedRecord(0x28,1000,350);resumed.readInfo();resumed.monitorGPUPriority();
 assert(resumed.process.value.calls==3);
 resumed.lastKnownDone=399;Time::clock=100;resumed.monitorGPUPriority();assert(resumed.process.value.calls==3);
 resumed.lastKnownDone=400;resumed.monitorGPUPriority();assert(resumed.process.value.calls==4);
 // Missing/truncated records keep startup checks alive. A late first checkpoint
 // establishes the baseline; only the following advance ends startup checking.
 remove("wuinfo_01.dat");Unit late;late.lastKnownTotal=late.lastKnownDone=0;
 late.config.priority=late.process.value.priority=late.fields["gpu_priority_requested"]="high";
 late.initializePriorityMonitor();
 Time::clock=0;late.monitorGPUPriority();assert(late.process.value.calls==1);
 sharedRecord(0x28,1000,600,true);late.readInfo();Time::clock=5;late.monitorGPUPriority();
 assert(late.process.value.calls==2);
 sharedRecord(0x28,1000,600);late.readInfo();Time::clock=10;late.monitorGPUPriority();

 Time::clock=15;late.monitorGPUPriority();assert(late.process.value.calls==4);
 sharedRecord(0x28,1000,610);late.readInfo();late.monitorGPUPriority();assert(late.process.value.calls==5);
 // A failed read of the pre-launch record conservatively follows the late path.
 sharedRecord(0x27,1000,700);Unit bad;bad.initializePriorityMonitor();
 remove("wuinfo_01.dat");
 Unit restore;restore.lastKnownDone=200;
 restore.process.value.priority=restore.fields["gpu_priority_requested"]="high";
 restore.process.value.warning="Permission denied";
 auto priorLogs=logs;restore.monitorGPUPriority();
 assert(!restore.fields["gpu_priority_warning"].empty());
 auto restoreCalls=restore.process.value.calls;
 restore.lastKnownDone=240;restore.monitorGPUPriority();assert(restore.process.value.calls==restoreCalls);
 restore.lastKnownDone=251;restore.monitorGPUPriority();
 assert(restore.process.value.calls==restoreCalls+1&&logs==priorLogs+1);
 restore.process.value.warning="";restore.lastKnownDone=302;restore.monitorGPUPriority();
 assert(restore.process.value.calls==restoreCalls+2);
 assert(restore.fields["gpu_priority_warning"].empty());
 restore.lastKnownDone=900;restore.monitorGPUPriority();assert(restore.process.value.calls==restoreCalls+2);
 // A new explicit selection supersedes a pending stock restore.
 restore.process.value.priority=restore.fields["gpu_priority_requested"]="high";
 restore.process.value.warning="Permission denied";restore.monitorGPUPriority();
 assert(!restore.fields["gpu_priority_warning"].empty());
 restore.config.priority="normal";restore.process.value.warning="";restore.monitorGPUPriority();
 assert(restore.fields["gpu_priority_applied"]=="normal");
 // Backward wall-clock changes re-arm startup checks without unthrottled polling.
 assert(!GPUProcessPriority::shouldCheckStartup(100,105,false,false));
 assert(GPUProcessPriority::shouldCheckStartup(99,105,false,false));
 assert(!GPUProcessPriority::shouldCheckStartup(99,105,true,false));
 Unit backwards;backwards.config.priority=backwards.process.value.priority="high";
 Time::clock=100;backwards.monitorGPUPriority();assert(backwards.process.value.calls==1);
 Time::clock=90;backwards.monitorGPUPriority();assert(backwards.process.value.calls==2);
 Time::clock=94;backwards.monitorGPUPriority();assert(backwards.process.value.calls==2);
 Time::clock=95;backwards.monitorGPUPriority();assert(backwards.process.value.calls==3);
 // Failed removal retries by time even with no further work progress.
 Unit stalledRestore;stalledRestore.lastKnownDone=200;
 stalledRestore.process.value.priority="high";
 stalledRestore.process.value.warning="Permission denied";
 auto restoreLogs=logs;
 Time::clock=100;stalledRestore.monitorGPUPriority();
 assert(stalledRestore.process.value.calls==1&&!stalledRestore.fields["gpu_priority_warning"].empty());
 Time::clock=104;stalledRestore.monitorGPUPriority();assert(stalledRestore.process.value.calls==1);
 Time::clock=105;stalledRestore.monitorGPUPriority();
 assert(stalledRestore.process.value.calls==2&&logs==restoreLogs+1);
 stalledRestore.process.value.warning="";
 Time::clock=110;stalledRestore.monitorGPUPriority();
 assert(stalledRestore.process.value.calls==3&&stalledRestore.process.value.priority.empty());
 assert(stalledRestore.fields["gpu_priority_warning"].empty());
 Time::clock=200;stalledRestore.monitorGPUPriority();assert(stalledRestore.process.value.calls==3);
 // Reset discards a previous launch's pending restore.
 Unit reused;
 reused.lastKnownDone=200;
 reused.process.value.priority="high";
 reused.process.value.warning="Permission denied";
 Time::clock=100;
 reused.monitorGPUPriority();
 assert(reused.process.value.calls==1);
 assert(!reused.fields["gpu_priority_warning"].empty());
 reused.gpuPriorityMonitor.reset(true,300,"");
 reused.lastKnownDone=300;
 reused.process.value.priority.clear();
 reused.process.value.warning.clear();
 Time::clock=200;
 reused.monitorGPUPriority();
 assert(reused.process.value.calls==1);
 reused.config.priority="normal";
 reused.monitorGPUPriority();
 assert(reused.process.value.calls==2);
 assert(reused.fields["gpu_priority_applied"]=="normal");
 assert(reused.fields["gpu_priority_warning"].empty());
 // Reset also discards the previous requested selection.
 reused.gpuPriorityMonitor.reset(true,400,"");
 reused.process.value.priority.clear();
 auto status=reused.gpuPriorityMonitor.update(
   reused.process.value,0x28,"",400,1000,0.4,300);
 assert(!status);
 assert(reused.process.value.calls==2);
#else
 u.config.priority=u.process.value.priority="other-low";
 u.monitorGPUPriority();assert(!u.process.value.calls);
 u.lastKnownDone=101;u.monitorGPUPriority();assert(u.process.value.calls==1&&!u.process.value.apply);
 u.lastKnownDone=900;u.monitorGPUPriority();assert(u.process.value.calls==1);
 u.config.priority="other-normal";u.monitorGPUPriority();assert(u.process.value.calls==1&&u.fields["gpu_priority_warning"].find("next core launch")!=string::npos);
 u.config.priority="";u.monitorGPUPriority();assert(u.process.value.calls==1&&u.fields["gpu_priority_requested"].empty());
 Unit unknown;unknown.coreValue.type=0x99;unknown.config.priority="other-normal";unknown.lastKnownDone=200;
 unknown.monitorGPUPriority();assert(!unknown.process.value.calls&&unknown.fields["gpu_priority_warning"].find("unsupported")!=string::npos);
 unknown.config.priority="";unknown.monitorGPUPriority();
 assert(unknown.fields["gpu_priority_requested"].empty()&&unknown.fields["gpu_priority_warning"].empty());
 assert(unknown.fields["gpu_priority_applied"].empty()&&!unknown.process.value.calls);
 Unit reverted;reverted.lastKnownDone=200;reverted.config.priority="other-normal";
 reverted.monitorGPUPriority();assert(reverted.fields["gpu_priority_warning"].find("next core launch")!=string::npos);
 // Cancellation must also clear stale metadata before resumed work advances.
 reverted.gpuPriorityMonitor.reset(true,reverted.lastKnownDone,"other-normal");
 reverted.config.priority="";reverted.monitorGPUPriority();
 assert(reverted.fields["gpu_priority_requested"].empty()&&reverted.fields["gpu_priority_warning"].empty());
 assert(reverted.fields["gpu_priority_applied"].empty()&&!reverted.process.value.calls);
 reverted.monitorGPUPriority();assert(!reverted.process.value.calls);
#endif
 // Cancellation before any valid progress must clear the launch selection.
 Unit pending;pending.config.priority="normal";pending.initializePriorityMonitor();
 pending.config.priority="";pending.fields["gpu_priority_requested"]="normal";
 pending.monitorGPUPriority();assert(pending.fields["gpu_priority_requested"].empty());
 cout<<"PASS: production GPU priority monitor, checkpoint baseline/startup/work gating, CPU/default exclusion, live/pending edits and bounded warnings\n";
}
"""
compile_and_run(harness, ['GPUProcessPriorityMonitor.cpp'], cxx='c++',
                files={'fah/client/CoreProcess.h': '#pragma once\n#include <string>\nnamespace FAH { namespace Client { using std::string;\n'+'class CoreProcess {public:\n string priority,warning;unsigned calls=0;bool apply=false;\n string getPriorityOverride()const{return priority;}\n void setPriorityOverride(const string &value){priority=value;}\n string checkPriorityOverride(bool value){++calls;apply=value;return warning;}\n};\n'+'}}'})
