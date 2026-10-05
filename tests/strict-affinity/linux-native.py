from pathlib import Path
import subprocess,tempfile
here=Path(__file__).resolve().parent;s=(here.parents[1]/'src/fah/client/CoreProcess.cpp').read_text()
a=s.index('void CoreProcess::execStrict(');body=s[a:]
cpp=r"""#include <unistd.h>
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
using namespace std;
#define THROW(x) throw runtime_error("Strict launch rejected")
struct AffinityRejected:runtime_error {AffinityRejected():runtime_error("affinity rejected") {}};
struct CoreProcess:map<string,string> {
 struct StrictProcess{uint64_t pid=0;};unique_ptr<StrictProcess> strictProcess;
 set<unsigned> requiredAffinity;string path="/bin/sh",wd=".";bool running=false;int returnCode=0,exitFlags=0;
 bool isRunning()const{return running;}void execStrict(const vector<string>&);
};
"""+body+r"""
void run(set<unsigned> mask,bool expected){
 unlink("marker");CoreProcess p;p.requiredAffinity=mask;bool accepted=true;
 try{p.execStrict({"/bin/sh","-c","touch marker; sleep 1"});}catch(const AffinityRejected &){accepted=false;}
 assert(accepted==expected);
 if(accepted){cpu_set_t actual;CPU_ZERO(&actual);assert(!sched_getaffinity(p.strictProcess->pid,sizeof(actual),&actual));assert(CPU_COUNT(&actual)==(int)mask.size());for(auto cpu:mask)assert(CPU_ISSET(cpu,&actual));while(waitpid(p.strictProcess->pid,0,0)<0&&errno==EINTR){}assert(access("marker",F_OK)==0);}
 else assert(access("marker",F_OK)!=0);
}
int main(){
 { CoreProcess p; cpu_set_t mask; CPU_ZERO(&mask); assert(!sched_getaffinity(0,sizeof(mask),&mask));
   for(int i=0;i<CPU_SETSIZE;i++) if(CPU_ISSET(i,&mask)){p.requiredAffinity.insert(i);break;}
   p.wd="/nonexistent-fah-test-directory"; bool ordinary=false;
   try {p.execStrict({"/bin/sh","-c","touch marker"});}
   catch(const AffinityRejected &){assert(false);}
   catch(const runtime_error &){ordinary=true;}
   assert(ordinary);
 }
 cpu_set_t available;CPU_ZERO(&available);assert(!sched_getaffinity(0,sizeof(available),&available));int good=-1,bad=-1;for(int i=0;i<CPU_SETSIZE;i++){if(CPU_ISSET(i,&available)&&good<0)good=i;if(!CPU_ISSET(i,&available)&&bad<0)bad=i;}assert(good>=0&&bad>=0);run({(unsigned)good},true);
 unsigned second=good;for(int i=good+1;i<CPU_SETSIZE;++i)if(CPU_ISSET(i,&available)){second=i;break;}
 run({(unsigned)good,second},true);run({(unsigned)bad},false);run({(unsigned)good,(unsigned)bad},false);run({numeric_limits<unsigned>::max()},false);
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
 subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror',str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],cwd=p,check=True)
