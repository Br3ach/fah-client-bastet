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
struct CoreProcess:map<string,string> {
 struct StrictProcess{uint64_t pid=0;};unique_ptr<StrictProcess> strictProcess;
 set<unsigned> requiredAffinity;string path="/bin/sh",wd=".";bool running=false;int returnCode=0,exitFlags=0;
 bool isRunning()const{return running;}void execStrict(const vector<string>&);
};
"""+body+r"""
void run(set<unsigned> mask,bool expected){
 unlink("marker");CoreProcess p;p.requiredAffinity=mask;bool accepted=true;
 try{p.execStrict({"/bin/sh","-c","touch marker; sleep 1"});}catch(...){accepted=false;}
 assert(accepted==expected);
 if(accepted){cpu_set_t actual;CPU_ZERO(&actual);assert(!sched_getaffinity(p.strictProcess->pid,sizeof(actual),&actual));assert(CPU_COUNT(&actual)==(int)mask.size());for(auto cpu:mask)assert(CPU_ISSET(cpu,&actual));while(waitpid(p.strictProcess->pid,0,0)<0&&errno==EINTR){}assert(access("marker",F_OK)==0);}
 else assert(access("marker",F_OK)!=0);
}
int main(){cpu_set_t available;CPU_ZERO(&available);assert(!sched_getaffinity(0,sizeof(available),&available));int good=-1,bad=-1;for(int i=0;i<CPU_SETSIZE;i++){if(CPU_ISSET(i,&available)&&good<0)good=i;if(!CPU_ISSET(i,&available)&&bad<0)bad=i;}assert(good>=0&&bad>=0);run({(unsigned)good},true);run({(unsigned)bad},false);run({(unsigned)good,(unsigned)bad},false);run({numeric_limits<unsigned>::max()},false);
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
