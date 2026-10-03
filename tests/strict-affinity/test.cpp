#include <fah/client/CoreProcess.h>
#include <cbang/os/SystemInfo.h>
#include <cassert>
#include <fstream>
#include <cstdio>
#include <iostream>
#include <limits>
#include <thread>
#include <chrono>
#ifdef _WIN32
#include <windows.h>
#else
#include <sched.h>
#endif
int main(int argc,char **argv) {
  if (argc >= 4) {
    unsigned cpu=std::stoul(argv[3]);
#ifdef _WIN32
    DWORD_PTR mask=0,system=0;
    assert(GetProcessAffinityMask(GetCurrentProcess(),&mask,&system));
    assert(mask==((DWORD_PTR)1<<cpu));
#else
    cpu_set_t actual; CPU_ZERO(&actual);
    assert(!sched_getaffinity(0,sizeof(actual),&actual));
    assert(CPU_COUNT(&actual)==1 && CPU_ISSET(cpu,&actual));
#endif
    {std::ofstream(argv[2]) << "executed";}
    if (argc == 5) while (true) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    return 0;
  }
  assert(argc==2);
  auto available=cb::SystemInfo::instance().getAvailableCPUs();
  assert(!available.empty());unsigned cpu=*available.begin();
  std::string marker=argv[1];std::remove(marker.c_str());
  FAH::Client::CoreProcess valid(argv[0]);valid.setRequiredAffinity({cpu});
  valid.exec(std::vector<std::string>{"child",marker,std::to_string(cpu)});
  valid.wait();assert(valid.exitedOk());assert(std::ifstream(marker).good());
  std::remove(marker.c_str());
  std::vector<std::set<unsigned>> rejectedMasks = {{std::numeric_limits<unsigned>::max()},{cpu,std::numeric_limits<unsigned>::max()}};
#ifdef _WIN32
  DWORD_PTR processMask=0,systemMask=0;
  assert(GetProcessAffinityMask(GetCurrentProcess(),&processMask,&systemMask));
  for(unsigned bad=0;bad<sizeof(systemMask)*8;++bad) if(!(systemMask&((DWORD_PTR)1<<bad))){
    rejectedMasks.push_back({bad});rejectedMasks.push_back({cpu,bad});break;
  }
#endif
  for(auto mask:rejectedMasks) {
    bool rejected=false;
    try {FAH::Client::CoreProcess invalid(argv[0]);invalid.setRequiredAffinity(mask);invalid.exec(std::vector<std::string>{"child",marker,std::to_string(cpu)});invalid.wait();rejected=!invalid.exitedOk();}
    catch (...) {rejected=true;}
    assert(rejected);assert(!std::ifstream(marker).good());
  }
  FAH::Client::CoreProcess live(argv[0]);live.setRequiredAffinity({cpu});
  live.exec({"child",marker,std::to_string(cpu),"hold"});
  for(unsigned i=0;i<100 && !std::ifstream(marker).good();++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  assert(std::ifstream(marker).good());assert(live.getPID());assert(live.isRunning());
  assert(live.kill());assert(!live.isRunning());assert(live.getWasKilled());
  std::remove(marker.c_str());
  std::cout << "PASS: exact child mask, rejected masks, live PID, kill/wait lifecycle\n";
}
