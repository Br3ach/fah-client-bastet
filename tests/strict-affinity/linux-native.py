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

from pathlib import Path
import subprocess,tempfile
here=Path(__file__).resolve().parent;s=(here.parents[1]/'src/fah/client/CoreProcess.cpp').read_text()
a=s.index('void CoreProcess::execStrict(');body=s[a:s.index('#ifdef _WIN32' + chr(10) + 'unsigned long CoreProcess::desiredPriorityClass', a)]
platform=(here.parents[1]/'src/fah/client/lin/CoreProcessLaunch.cpp').read_text()
platform=platform[platform.index('namespace {'):platform.rindex('#endif')]
body += platform
body += s[s.index('string CoreProcess::checkPriorityOverride('):]
stop=s[s.index('void CoreProcess::stop()'):s.index('CoreProcess::~CoreProcess()')]
interrupt=s[s.index('void CoreProcess::interrupt()'):a]
unit_source=(here.parents[1]/'src/fah/client/Unit.cpp').read_text()
a=unit_source.index('void Unit::next() {')
b=unit_source.index('void Unit::processStarted(',a)
unit_next=unit_source[a:b].replace('void Unit::next()', 'void next()').replace(' CATCH_ERROR;', ' catch(const runtime_error&) {}')
a=unit_source.index('  affinityRejections = 0;',b)
b=unit_source.index('  auto pid =',a)
unit_reset=unit_source[a:b]
a=unit_source.index('bool Unit::isActive() const {')
b=unit_source.index('void Unit::setScheduledCPUs',a)
unit_active=unit_source[a:b].replace('bool Unit::isActive()', 'bool isActive()')
unit_header=(here.parents[1]/'src/fah/client/Unit.h').read_text()
a=unit_header.index('      bool hasLaunchFailure() const {')
b=unit_header.index('\n      }',a)+8
unit_failure=unit_header[a:b]
cpp=r"""#include <fah/client/GPUProcessPriority.h>
#include <fah/client/CPUOwnershipPolicy.h>
using FAH::Client::GPUProcessPriority;
#include <dirent.h>
#include <unistd.h>
#include <sched.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <csignal>
#include <cerrno>
#include <cassert>
#include <cstring>
#include <map>
#include <vector>
#include <set>
#include <string>
#include <memory>
#include <limits>
#include <fcntl.h>
#include <stdexcept>
#include <iostream>
#include <fstream>
#include <thread>
#include <chrono>
#include <ctime>
using namespace std;
#define THROW(x) throw runtime_error("Strict launch rejected")
struct SchedulingRejected:runtime_error{SchedulingRejected():runtime_error("scheduling rejected") {}};
struct AffinityRejected:runtime_error {AffinityRejected():runtime_error("affinity rejected") {}};
#define LOG_WARNING(x) ((void)0)
#define LOG_INFO(level,x) ((void)0)
#define LOG_DEBUG(level,x) ((void)0)
#define TRY_CATCH_ERROR(x) try{x;}catch(...){}
struct Time{static uint64_t now(){return std::time(nullptr);}};
struct Subprocess{static void interrupt(){assert(false);}};
struct CoreProcess:map<string,string> {
 struct StrictProcess{uint64_t pid=0;};unique_ptr<StrictProcess> strictProcess;
 string priorityOverride;
 set<unsigned> requiredAffinity;string path="/bin/sh",wd=".";bool running=false;int returnCode=0,exitFlags=0;
 uint64_t interruptTime=0,lastStop=0;bool killedByClient=false;
 uint64_t getPID()const{return strictProcess->pid;}
 bool isStopping()const{return false;}
 bool isRunning(){if(running){int status=0;auto pid=waitpid(getPID(),&status,WNOHANG);if(pid>0){running=false;assert(WIFEXITED(status));returnCode=WEXITSTATUS(status);}}return running;}
 void kill(){::killpg(getPID(),SIGKILL);killedByClient=true;}
 string checkPriorityOverride(bool);
 void stop();void interrupt();void execStrict(const vector<string>&);
 void execStrictLinux(const vector<string>&, StrictProcess &);
};
// Faults apply only to child preparation, not the test process's own syscalls.
int readbackFault=0, readbackEntries=0;
struct dirent *checkedReadDir(DIR *tasks) {
 if(!readbackFault)return readdir(tasks);
 if(readbackFault==1)return nullptr; // Empty successful enumeration.
 if(readbackEntries++){if(readbackFault==3)errno=EIO;return nullptr;}
 static dirent entry{};strcpy(entry.d_name,"123");return &entry;
}
int priorityFault=0;bool restoringPriority=false;
int checkedSetScheduler(pid_t pid,int policy,const sched_param *param) {
 if(priorityFault && policy==SCHED_OTHER){errno=EPERM;return -1;}
 if(priorityFault==1 && policy==SCHED_IDLE){errno=EPERM;return -1;}
 const int result=sched_setscheduler(pid,policy,param);
 if(priorityFault && policy==SCHED_IDLE)restoringPriority=true;
 return result;
}
int checkedSetPriority(__priority_which_t which,id_t who,int value) {
 if(priorityFault==2 && value==19){errno=EPERM;return -1;}
 return setpriority(which,who,value);
}
int checkedGetScheduler(pid_t pid) {
 if(readbackFault==2){errno=ESRCH;return -1;}
 if(readbackFault==4){errno=EPERM;return -1;}
 if(readbackFault)return SCHED_OTHER;
 if(priorityFault==3 && restoringPriority)return SCHED_OTHER;
 return sched_getscheduler(pid);
}
int checkedGetPriority(__priority_which_t which,id_t who) {
 if(readbackFault==5){errno=EIO;return -1;}
 if(readbackFault==7){errno=ESRCH;return -1;}
 if(readbackFault==8)return 19;
 if(readbackFault)return 0;
 if(priorityFault==4 && restoringPriority)return 18;
 if(priorityFault==5 && restoringPriority){errno=EIO;return -1;}
 return getpriority(which,who);
}
#define readdir checkedReadDir
#define sched_setscheduler checkedSetScheduler
#define setpriority checkedSetPriority
#define sched_getscheduler checkedGetScheduler
#define getpriority checkedGetPriority
"""+stop+interrupt+body+r"""
#undef readdir
#undef sched_setscheduler
#undef setpriority
#undef sched_getscheduler
#undef getpriority

const int UNIT_ASSIGN=0,UNIT_DOWNLOAD=1,UNIT_CORE=2,UNIT_RUN=3,UNIT_UPLOAD=4,UNIT_DUMP=5,UNIT_DONE=6;
struct Unit;
struct TestUnits {unsigned size()const{return 0;}shared_ptr<Unit> getUnit(unsigned){return {};}};
struct TestResources {uint64_t getTopologyGeneration()const{return 1;}void refreshTopology(const string&) {}};
struct TestApp {TestUnits units;TestResources resources;auto getUnits(){return &units;}auto &getCPUResources(){return resources;}void triggerUpdate(){}};
struct CPUResources {static string formatCPUs(const set<unsigned>&){return "mask";}};
using FAH::Client::CPUOwnershipPolicy;
struct Unit {
 TestApp app;FAH::Client::RunningCPUAllocation runningAllocation;
 struct EmptyProcess {bool isSet()const{return false;}CoreProcess *operator->()const{return nullptr;}} process,pr;
 uint64_t wait=0,rejectedTopologyGeneration=0;unsigned retries=7,armed=0,attempts=0;
 unsigned affinityRejections=0;bool affinityRejectionLogged=false,running=false;
 set<unsigned> attemptedAffinityCPUs,rejectedAffinityCPUs;map<string,string> fields;
 string overridePriority,launchDirectory=".";
 bool isExpired()const{return false;}bool isPaused()const{return false;}
 bool isRunning()const{return running;}bool isWaiting()const{return wait>Time::now();}
 int getState()const{return UNIT_RUN;}const char *getPauseReason()const{return "";}
 bool has(const char *key)const{return fields.count(key);}bool hasString(const char *key)const{return has(key);}
 template<class T>void insert(const char *key,const T&){fields[key]="present";}
 void erase(const char *key){fields.erase(key);}
 void triggerNext(unsigned seconds=0){armed=seconds;}
 void setWait(unsigned seconds){wait=seconds?Time::now()+seconds:0;}
 auto buildDesiredAllocation()const{return FAH::Client::RunningCPUAllocation{};}
 bool blocksCPULaunch(const Unit&,const FAH::Client::RunningCPUAllocation&)const{return false;}
 bool hasGPUs()const{return false;}unsigned getScheduledCPUs()const{return 1;}
 unsigned getRunTimeDelta()const{return 10;}
 void clean(const string&){}void stopRun(){}void monitorRun(){}void finalizeRun(){}void save(){}void readViewerData(){}
 void assign(){}void download(){}void getCore(){}void upload(){}void dump(){}
 void retry(){++retries;}
 void reset(){
"""+unit_reset+r"""
 }
 void run(){
  ++attempts;CoreProcess child;child.priorityOverride=overridePriority;child.wd=launchDirectory;
  cpu_set_t mask;CPU_ZERO(&mask);assert(!sched_getaffinity(0,sizeof(mask),&mask));
  for(int i=0;i<CPU_SETSIZE;++i)if(CPU_ISSET(i,&mask)){child.requiredAffinity.insert(i);break;}
  child.execStrict({"/bin/sh","-c","touch unit-marker"});
  while(waitpid(child.strictProcess->pid,nullptr,0)<0&&errno==EINTR){}
  running=true;reset();triggerNext();
 }
"""+unit_next+unit_active+unit_failure+r"""
};

void run(set<unsigned> mask,bool expected){
 unlink("marker");CoreProcess p;p.requiredAffinity=mask;bool accepted=true;
 try{p.execStrict({"/bin/sh","-c","touch marker; sleep 1"});}catch(const AffinityRejected &){accepted=false;}
 assert(accepted==expected);
 if(accepted){cpu_set_t actual;CPU_ZERO(&actual);assert(!sched_getaffinity(p.strictProcess->pid,sizeof(actual),&actual));assert(CPU_COUNT(&actual)==(int)mask.size());for(auto cpu:mask)assert(CPU_ISSET(cpu,&actual));while(waitpid(p.strictProcess->pid,0,0)<0&&errno==EINTR){}assert(access("marker",F_OK)==0);}
 else assert(access("marker",F_OK)!=0);
}
volatile sig_atomic_t interrupted=0;
void onInterrupt(int){interrupted=1;}
int main(int argc,char **argv){
 for(int fault=1;fault<=8;++fault) {
  CoreProcess p;p.strictProcess.reset(new CoreProcess::StrictProcess);
  p.strictProcess->pid=getpid();p.running=true;p.priorityOverride="other-normal";
  readbackFault=fault;readbackEntries=0;
  assert(p.checkPriorityOverride(false).empty()==(fault==6));
 }
 readbackFault=0;
 cout<<"PASS: Linux priority readback empty, vanished, query/read errors, mismatch and confirmation\n";
 assert(GPUProcessPriority::valid("other-low") && GPUProcessPriority::valid("other-normal"));
 assert(GPUProcessPriority::options() ==
   std::vector<std::string>({"", "other-low", "other-normal"}));
 assert(GPUProcessPriority::coreArgument("other-low") == "low");
 assert(GPUProcessPriority::coreArgument("other-normal") == "normal");
 assert(GPUProcessPriority::niceValue("other-low") == 10);
 assert(GPUProcessPriority::niceValue("other-normal") == 0);
 assert(!GPUProcessPriority::shouldCheckStartup(10,10,false,true));
 assert(!GPUProcessPriority::valid("high") && !GPUProcessPriority::valid("realtime"));
 assert(!GPUProcessPriority::shouldCheck(false,10,10,.1,0,false));
 assert(GPUProcessPriority::shouldCheck(false,11,10,.11,0,false));
 assert(!GPUProcessPriority::shouldCheck(true,20,10,.2,.1,false));
 assert(GPUProcessPriority::shouldCheck(true,20,10,.2,.1,true));
 if(argc==3 && string(argv[1])=="unit-recovery") {
  priorityFault=stoi(argv[2]);unlink("unit-marker");Unit unit;
  if(priorityFault!=1)unit.overridePriority="other-normal";
  unit.next();assert(unit.retries==7&&unit.armed==30&&unit.isWaiting());
  assert(unit.hasLaunchFailure()&&!unit.isActive()&&!unit.isRunning());
  assert(access("unit-marker",F_OK)!=0);
  int status=0;assert(waitpid(-1,&status,WNOHANG)==-1&&errno==ECHILD);
  unit.next();assert(unit.attempts==1&&unit.retries==7); // Cooldown gates launches.
  unit.wait=0;priorityFault=0;restoringPriority=false;
  unit.next();assert(unit.retries==7&&unit.attempts==2&&unit.isRunning());
  assert(!unit.has("launch_environment_warning")&&!unit.hasLaunchFailure()&&unit.isActive());
  assert(access("unit-marker",F_OK)==0);unlink("unit-marker");
  Unit ordinary;ordinary.launchDirectory="/nonexistent-fah-test-directory";priorityFault=1;
  ordinary.next();assert(ordinary.retries==8&&!ordinary.has("launch_environment_warning"));
  assert(!ordinary.isRunning()&&access("unit-marker",F_OK)!=0);
  cout<<"PASS: full Unit::next body retains retries, arms cooldown, idles and recovers after real Linux launch failure\n";
  return 0;
 }
 if(argc==3 && string(argv[1])=="restore-failure") {
  priorityFault=stoi(argv[2]);unlink("restore-marker");
  CoreProcess p;p.priorityOverride="other-normal";
  bool rejected=false;
  try{p.execStrict({"/bin/sh","-c","touch restore-marker"});}
  catch(const AffinityRejected&){assert(false);}
  catch(const SchedulingRejected&){rejected=true;}
  assert(rejected&&!p.strictProcess&&access("restore-marker",F_OK)!=0);
  int status=0;assert(waitpid(-1,&status,WNOHANG)==-1&&errno==ECHILD);
  cout<<"PASS: failed Linux priority restoration/readback rejects preparation and reaps child\n";
  return 0;
 }
 if(argc==2 && string(argv[1])=="fallback") {
  assert(sched_getscheduler(0)==SCHED_IDLE && getpriority(PRIO_PROCESS,0)==19);
  {ofstream("fallback-marker")<<"executed";}
  this_thread::sleep_for(chrono::milliseconds(500));return 0;
 }
 if(argc==2 && string(argv[1])=="denied") {
  assert(!setpriority(PRIO_PROCESS,0,19));
  unlink("fallback-marker");CoreProcess p;p.path="/proc/self/exe";p.priorityOverride="other-normal";
  p.execStrict({"/proc/self/exe","fallback"});
  for(unsigned i=0;i<100 && access("fallback-marker",F_OK);++i)this_thread::sleep_for(chrono::milliseconds(10));
  assert(access("fallback-marker",F_OK)==0);
  assert(!p.checkPriorityOverride(false).empty());
  int status=0;waitpid(p.getPID(),&status,0);assert(WIFEXITED(status)&&WEXITSTATUS(status)==0);
  cout<<"PASS: denied Linux priority increase retains stock idle scheduling and reports mismatch\n";
  return 0;
 }
 if(argc==3 && string(argv[1])=="priority"){
  assert(sched_getscheduler(0)==SCHED_OTHER);
  assert(getpriority(PRIO_PROCESS,0)==0);
  int nice = GPUProcessPriority::niceValue(argv[2]);
  assert(!setpriority(PRIO_PROCESS,0,nice)); // Simulate the core's supported argument.
  thread worker([nice]{assert(sched_getscheduler(0)==SCHED_OTHER);assert(getpriority(PRIO_PROCESS,0)==nice);});
  worker.join();
  {ofstream("priority-marker")<<"executed";}
  this_thread::sleep_for(chrono::milliseconds(500));
  return 0;
 }
 if(argc==2 && string(argv[1])=="graceful"){
  assert(sched_getscheduler(0)==SCHED_IDLE);
  assert(getpriority(PRIO_PROCESS,0)==19);
  thread worker([]{assert(sched_getscheduler(0)==SCHED_IDLE);assert(getpriority(PRIO_PROCESS,0)==19);});
  worker.join();
  assert(signal(SIGINT,onInterrupt)!=SIG_ERR);
  {ofstream("graceful-marker")<<"ready";}
  while(!interrupted)std::this_thread::sleep_for(std::chrono::milliseconds(10));
  {ofstream("graceful-marker")<<"interrupted";}
  return 42;
 }

 { CoreProcess p; cpu_set_t mask; CPU_ZERO(&mask); assert(!sched_getaffinity(0,sizeof(mask),&mask));
   for(int i=0;i<CPU_SETSIZE;i++) if(CPU_ISSET(i,&mask)){p.requiredAffinity.insert(i);break;}
   priorityFault=1;
   p.wd="/nonexistent-fah-test-directory"; bool ordinary=false;
   try {p.execStrict({"/bin/sh","-c","touch marker"});}
   catch(const AffinityRejected &){assert(false);}
   catch(const SchedulingRejected &){assert(false);}
   catch(const runtime_error &){ordinary=true;}
   assert(ordinary);priorityFault=0;
 }
 cpu_set_t available;CPU_ZERO(&available);assert(!sched_getaffinity(0,sizeof(available),&available));int good=-1,bad=-1;for(int i=0;i<CPU_SETSIZE;i++){if(CPU_ISSET(i,&available)&&good<0)good=i;if(!CPU_ISSET(i,&available)&&bad<0)bad=i;}assert(good>=0&&bad>=0);run({(unsigned)good},true);
 unsigned second=good;for(int i=good+1;i<CPU_SETSIZE;++i)if(CPU_ISSET(i,&available)){second=i;break;}
 run({(unsigned)good,second},true);run({(unsigned)bad},false);run({(unsigned)good,(unsigned)bad},false);run({numeric_limits<unsigned>::max()},false);
 priorityFault=1;run({(unsigned)bad},false);priorityFault=0;
 // Both affinity and unpinned GPU overrides start OTHER/nice 0; the core can lower to 10.
 for (bool managed: {false,true}) {
  for (string priority: {"other-low", "other-normal"}) {
   unlink("priority-marker");CoreProcess p;p.path="/proc/self/exe";p.priorityOverride=priority;
   if(managed)p.requiredAffinity={(unsigned)good};
   p.execStrict({"/proc/self/exe","priority",priority});
   for(unsigned i=0;i<100 && access("priority-marker",F_OK);++i)
    this_thread::sleep_for(chrono::milliseconds(10));
   assert(access("priority-marker",F_OK)==0);
   assert(p.checkPriorityOverride(false).empty());
   int status=0;while(waitpid(p.getPID(),&status,0)<0&&errno==EINTR){}
   assert(WIFEXITED(status)&&WEXITSTATUS(status)==0);
   assert(access("priority-marker",F_OK)==0);
  }
 }
 std::cout<<"PASS: Linux GPU overrides preserve OTHER/nice 0 before core initialization, including unpinned launch\n";
 // Exercise the production stop/interrupt bodies against a real process group.
 {
  unlink("graceful-marker");CoreProcess p;p.path="/proc/self/exe";p.requiredAffinity={(unsigned)good};
  p.execStrict({"/proc/self/exe","graceful"});
  for(unsigned i=0;i<500 && access("graceful-marker",F_OK);++i)
   std::this_thread::sleep_for(std::chrono::milliseconds(10));
  assert(access("graceful-marker",F_OK)==0);assert(p.isRunning());
  p.stop();assert(p.interruptTime);
  for(unsigned i=0;i<500 && p.isRunning();++i)
   std::this_thread::sleep_for(std::chrono::milliseconds(10));
  if(p.isRunning()){p.kill();waitpid(p.getPID(),nullptr,0);assert(false && "graceful interrupt timed out");}
  assert(p.returnCode==42&&!p.killedByClient);
  string received;ifstream("graceful-marker")>>received;assert(received=="interrupted");
  unlink("graceful-marker");
  cout<<"PASS: native Linux graceful stop delivers SIGINT and preserves exit code without kill\n";
 }
 // Change only an isolated test child's live affinity. No cgroup or host
 // topology is modified, and this does not simulate real hardware hot-plug.
 if(second!=(unsigned)good) {
  CoreProcess p;p.path="/bin/sleep";p.requiredAffinity={(unsigned)good,second};
  p.execStrict({"/bin/sleep","30"});
  cpu_set_t changed;CPU_ZERO(&changed);CPU_SET(good,&changed);
  assert(!sched_setaffinity(p.strictProcess->pid,sizeof(changed),&changed));
  cpu_set_t observed;CPU_ZERO(&observed);
  assert(!sched_getaffinity(p.strictProcess->pid,sizeof(observed),&observed));
  assert(CPU_EQUAL(&changed,&observed));
  assert(!killpg(p.strictProcess->pid,SIGKILL));
  while(waitpid(p.strictProcess->pid,0,0)<0&&errno==EINTR){}
  run({(unsigned)good},true);
  std::cout<<"PASS: isolated live Linux affinity shrink, child release and exact relaunch\n";
 } else std::cout<<"SKIP: live affinity shrink requires two usable logical CPUs\n";
 int output=dup(STDOUT_FILENO);assert(output>STDERR_FILENO);
 close(STDIN_FILENO);close(STDOUT_FILENO);close(STDERR_FILENO);
 run({(unsigned)good},true);run({(unsigned)bad},false);
 assert(dup2(output,STDOUT_FILENO)==STDOUT_FILENO);close(output);
std::cout<<"PASS: Linux client launch exact mask, syscall failure, partial application, unrepresentable mask\n";}
"""
with tempfile.TemporaryDirectory(prefix='fah-native-affinity-') as d:
 p=Path(d);(p/'test.cpp').write_text(cpp)
 subprocess.run(['g++','-I'+str(here.parents[1]/'src'),'-std=c++17','-Wall','-Wextra','-Werror',str(p/'test.cpp'),str(here.parents[1]/'src/fah/client/CPUOwnershipPolicy.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],cwd=p,check=True,timeout=60)
 subprocess.run([str(p/'test'),'denied'],cwd=p,check=True,timeout=10)

 for fault in range(1,6):
  subprocess.run([str(p/'test'),'restore-failure',str(fault)],cwd=p,check=True,timeout=10)

 for fault in (1,2):
  subprocess.run([str(p/'test'),'unit-recovery',str(fault)],cwd=p,check=True,timeout=10)
