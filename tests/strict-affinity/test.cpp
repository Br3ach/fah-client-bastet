/******************************************************************************\

                  This file is part of the Folding@home Client.

          The fah-client runs Folding@home protein folding simulations.
                    Copyright (c) 2001-2026, foldingathome.org
                               All rights reserved.

       This program is free software; you can redistribute it and/or modify
       it under the terms of the GNU General Public License as published by
        the Free Software Foundation; either version 3 of the License, or
                       (at your option) any later version.

         This program is distributed in the hope that it will be useful,
          but WITHOUT ANY WARRANTY; without even the implied warranty of
          MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
                   GNU General Public License for more details.

     You should have received a copy of the GNU General Public License along
     with this program; if not, write to the Free Software Foundation, Inc.,
           51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

                  For information regarding this software email:
                                 Joseph Coffland
                          joseph@cauldrondevelopment.com

\******************************************************************************/

#include <fah/client/CoreProcess.h>
#include <fah/client/GPUProcessPriority.h>
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
#include <sys/resource.h>
#include <csignal>
#endif
#ifdef FAH_STRICT_LAUNCH_TEST
bool rejectAffinity = false, rejectTermination = false, alreadyExited = false;
HANDLE rejectedHandle = 0;
unsigned affinityCalls = 0;
DWORD injectedWait = WAIT_OBJECT_0;
HANDLE pendingWaitHandle = 0;
unsigned cleanupWaits = 0;
DWORD testWaitForSingleObject(HANDLE process, DWORD timeout) {
  if (pendingWaitHandle == process) {
    assert(timeout == 0); // Launch recovery must never block the event loop.
    if (injectedWait != WAIT_OBJECT_0) {
      const DWORD result = injectedWait;
      injectedWait = WAIT_OBJECT_0;
      if (result == WAIT_FAILED) SetLastError(ERROR_INVALID_HANDLE);
      return result;
    }
    ++cleanupWaits;
    pendingWaitHandle = 0;
  }
  return WaitForSingleObject(process, timeout);
}
BOOL testSetProcessAffinityMask(HANDLE process, DWORD_PTR mask) {
  ++affinityCalls;
  if (rejectAffinity) {SetLastError(ERROR_INVALID_PARAMETER); return FALSE;}
  return SetProcessAffinityMask(process, mask);
}
BOOL testTerminateProcess(HANDLE process, UINT code) {
  if (injectedWait != WAIT_OBJECT_0 && !pendingWaitHandle) {
    assert(TerminateProcess(process, code));
    assert(WaitForSingleObject(process, 5000) == WAIT_OBJECT_0);
    // Inject an unconfirmed API observation after actual termination so no
    // real child is leaked if the assertion fails.
    pendingWaitHandle = process;
    return TRUE;
  }
  if (alreadyExited) {
    assert(TerminateProcess(process, code));
    assert(WaitForSingleObject(process, 5000) == WAIT_OBJECT_0);
    SetLastError(ERROR_ACCESS_DENIED); return FALSE;
  }
  if (rejectTermination) {
    if (!rejectedHandle)
      assert(DuplicateHandle(GetCurrentProcess(), process, GetCurrentProcess(),
        &rejectedHandle, 0, FALSE, DUPLICATE_SAME_ACCESS));
    SetLastError(ERROR_ACCESS_DENIED); return FALSE;
  }
  return TerminateProcess(process, code);
}
#endif
namespace {
#ifdef _WIN32
  volatile LONG interrupted = 0;
  BOOL WINAPI handleInterrupt(DWORD event) {
    if (event != CTRL_BREAK_EVENT) return FALSE;
    InterlockedExchange(&interrupted, 1);
    return TRUE;
  }
#else
  volatile sig_atomic_t interrupted = 0;
  void handleInterrupt(int) {interrupted = 1;}
#endif
}
int main(int argc,char **argv) {
#ifdef _WIN32
  if (argc == 3 && std::string(argv[1]) == "environment") {
    char value[64] = {};
    assert(GetEnvironmentVariableA("FAH_LAUNCH_ENV", value, sizeof(value)));
    assert(std::string(value) == "override");
    std::ofstream(argv[2]) << value;
    return 0;
  }
  if (argc == 4 && std::string(argv[1]) == "priority-high") {
    {
      std::ofstream output(argv[2]);
      output << GetPriorityClass(GetCurrentProcess());
      output.close();
      if (!output) return 1;
    }
    while (!std::ifstream(argv[3]).good())
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    return 0;
  }
  if (argc >= 4 && std::string(argv[1]) == "priority") {
    // Read at child entry, before the parent's post-launch correction can be
    // relied upon. Repeated launches exercise process creation, not WU resume.
    const DWORD actual = GetPriorityClass(GetCurrentProcess());
    {std::ofstream(argv[2]) << actual;}
    assert(actual == std::stoul(argv[3]));
    if (argc == 5)
      while (true) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    return 0;
  }
#endif
  if (argc >= 4) {
    unsigned cpu=std::stoul(argv[3]);
    std::set<unsigned> expected{cpu};
    if (std::string(argv[1]) == "pool") {
      expected.clear();std::string ids=argv[3];size_t start=0;
      do {size_t end=ids.find(',',start);expected.insert(std::stoul(ids.substr(start,end-start)));
        if(end==std::string::npos)break;start=end+1;}while(true);
    }
    auto verify = [&] {
#ifdef _WIN32
      DWORD_PTR mask=0,system=0,wanted=0;
      for(auto lp:expected)wanted|=(DWORD_PTR)1<<lp;
      assert(GetProcessAffinityMask(GetCurrentProcess(),&mask,&system));
      assert(mask==wanted);
      GROUP_AFFINITY thread{};
      assert(GetThreadGroupAffinity(GetCurrentThread(), &thread));
      assert((thread.Mask & mask)==wanted);
#else
      // Child and worker threads must inherit stock idle scheduling as well.
      assert(sched_getscheduler(0) == SCHED_IDLE);
      assert(getpriority(PRIO_PROCESS, 0) == 19);
      cpu_set_t actual; CPU_ZERO(&actual);
      assert(!sched_getaffinity(0,sizeof(actual),&actual));
      assert(CPU_COUNT(&actual)==(int)expected.size());
      for(auto lp:expected)assert(CPU_ISSET(lp,&actual));
#endif
    };
    verify();
    if (std::string(argv[1]) == "pool") {
      std::vector<std::thread> workers;
      for(unsigned i=0;i<4;++i)workers.emplace_back(verify);
      for(auto &worker:workers)worker.join();
    }
    if (std::string(argv[1]) == "graceful") {
#ifdef _WIN32
      assert(SetConsoleCtrlHandler(handleInterrupt, TRUE));
#else
      assert(std::signal(SIGINT, handleInterrupt) != SIG_ERR);
#endif
      {std::ofstream(argv[2]) << "ready";}
      while (!interrupted) std::this_thread::sleep_for(std::chrono::milliseconds(10));
      {std::ofstream(argv[2]) << "interrupted";}
      return 42;
    }
    {std::ofstream(argv[2]) << "executed";}
    if (argc == 5) while (true) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    return 0;
  }
  assert(argc==2);
  using Policy = FAH::Client::GPUProcessPriority;
#ifdef _WIN32
  assert(Policy::shouldCheckStartup(10,10,false,false));
  assert(!Policy::shouldCheckStartup(10,15,false,false));
  assert(!Policy::shouldCheckStartup(20,15,true,false));
  assert(Policy::shouldCheckStartup(10,15,true,true));
#endif
  assert(!Policy::shouldCheck(false,10,10,.1,0,false));
  assert(Policy::shouldCheck(false,11,10,.11,0,false));
  assert(!Policy::shouldCheck(true,14,10,.14,.1,false));
#ifdef _WIN32
  assert(Policy::shouldCheck(true,15,10,.15,.1,false));
#endif
  assert(Policy::shouldCheck(true,14,10,.14,.1,true));
  auto priorities = Policy::options();
#ifdef _WIN32
  assert(priorities == std::vector<std::string>({
    "", "idle", "below-normal", "normal", "above-normal", "high"
  }));
#endif
  for (const auto &value: priorities) assert(Policy::valid(value));
  assert(priorities.front().empty());
  assert(!FAH::Client::GPUProcessPriority::valid("realtime"));
  assert(!FAH::Client::GPUProcessPriority::valid("unknown"));
  auto available=cb::SystemInfo::instance().getAvailableCPUs();
  assert(!available.empty());unsigned cpu=*available.begin();
  std::string marker=argv[1];std::remove(marker.c_str());
#ifdef _WIN32
  // Exercise cbang itself without CoreProcess's post-launch SetPriorityClass.
  for (const auto priority: {cb::Subprocess::PRIORITY_IDLE,
      cb::Subprocess::PRIORITY_LOW, cb::Subprocess::PRIORITY_NORMAL,
      cb::Subprocess::PRIORITY_HIGH}) {
    for (unsigned launch = 0; launch < 2; ++launch) {
      cb::Subprocess child;
      const auto expected = cb::Subprocess::priorityToClass(priority);
      child.exec({argv[0], "priority", marker, std::to_string(expected)},
        cb::Subprocess::CREATE_PROCESS_GROUP | cb::Subprocess::W32_HIDE_WINDOW,
        priority);
      child.wait();
      assert(child.exitedOk());
      DWORD actual = 0; std::ifstream(marker) >> actual;
      assert(actual == expected);
      std::remove(marker.c_str());
    }
  }
  for (const auto &priority: {"", "idle", "below-normal", "normal", "above-normal"}) {
    for (bool pinned: {false, true}) {
      for (unsigned launch = 0; launch < 2; ++launch) {
        FAH::Client::CoreProcess process(argv[0]);
        process.setPriorityOverride(priority);
        if (pinned) process.setRequiredAffinity({cpu});
        process.exec({"priority", marker,
          std::to_string(process.desiredPriorityClass()), "hold"});
        DWORD actual = 0;
        for (unsigned i = 0; i < 500; ++i) {
          std::ifstream input(marker);
          if (input >> actual) break;
          std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        assert(actual == process.desiredPriorityClass());
        assert(process.kill());
        process.wait();
        std::remove(marker.c_str());
      }
    }
  }
  const std::string release = marker + ".release";
  for (bool pinned: {false, true}) {
    for (unsigned launch = 0; launch < 2; ++launch) {
      std::remove(marker.c_str());
      std::remove(release.c_str());
      FAH::Client::CoreProcess process(argv[0]);
      process.setPriorityOverride("high");
      if (pinned) process.setRequiredAffinity({cpu});
      process.exec({"priority-high", marker, release});
      DWORD entryPriority = 0;
      bool recorded = false;
      for (unsigned i = 0; i < 500; ++i) {
        std::ifstream input(marker);
        if (input >> entryPriority) {recorded = true; break;}
        if (!process.isRunning()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
      const bool confirmed = recorded && process.isRunning() &&
        process.checkPriorityOverride(false).empty();
      bool released;
      {
        std::ofstream output(release);
        output << "release";
        output.close();
        released = bool(output);
      }
      if (released)
        for (unsigned i = 0; i < 500 && process.isRunning(); ++i)
          std::this_thread::sleep_for(std::chrono::milliseconds(10));
      const bool exited = !process.isRunning();
      int exitCode = -1;
      // The driver timeout bounds exceptional cleanup failures.
      if (exited) exitCode = process.wait();
      else if (process.kill()) exitCode = process.wait();
      std::remove(marker.c_str());
      std::remove(release.c_str());
      assert(recorded && confirmed && released && exited && exitCode == 0);
      if (pinned) assert(entryPriority == HIGH_PRIORITY_CLASS);
    }
  }
  std::cout << "PASS: Windows entry and eventual High priority on repeated pinned/unpinned launches\n";
#endif
  FAH::Client::CoreProcess valid(argv[0]);valid.setRequiredAffinity({cpu});
  valid.exec(std::vector<std::string>{"child",marker,std::to_string(cpu)});
  valid.wait();assert(valid.exitedOk());assert(std::ifstream(marker).good());
  std::remove(marker.c_str());
  // Every new thread inherits the full process pool. No worker-level masks.
  if (available.size() >= 2) {
    auto second=*std::next(available.begin());
    FAH::Client::CoreProcess pool(argv[0]);pool.setRequiredAffinity({cpu,second});
    pool.exec({"pool",marker,std::to_string(cpu)+","+std::to_string(second)});
    pool.wait();assert(pool.exitedOk());assert(std::ifstream(marker).good());
    std::remove(marker.c_str());
  }
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
    catch (const FAH::Client::AffinityRejected &) {rejected=true;}
    assert(rejected);assert(!std::ifstream(marker).good());
  }
#ifdef FAH_STRICT_LAUNCH_TEST
  assert(SetEnvironmentVariableA("FAH_LAUNCH_ENV", "inherited"));
  {
    FAH::Client::CoreProcess environment(argv[0]);
    environment.setRequiredAffinity({cpu});
    environment.set("fah_launch_env", "override");
    environment.exec({"environment", marker}); environment.wait();
    assert(environment.exitedOk());
    std::string value; std::ifstream(marker) >> value; assert(value == "override");
  }
  assert(SetEnvironmentVariableA("FAH_LAUNCH_ENV", nullptr));
  std::remove(marker.c_str());
  for (DWORD result: {DWORD(WAIT_TIMEOUT), DWORD(WAIT_FAILED)}) {
    rejectAffinity = true;
    injectedWait = result;
    const unsigned before = cleanupWaits;
    bool failed = false;
    try {
      FAH::Client::CoreProcess rejected(argv[0]); rejected.setRequiredAffinity({cpu});
      rejected.exec({"child",marker,std::to_string(cpu)});
    } catch (const FAH::Client::AffinityRejected &) {failed = true;}
    assert(failed && pendingWaitHandle && !std::ifstream(marker).good());
    rejectAffinity = false;
    FAH::Client::CoreProcess recovered(argv[0]); recovered.setRequiredAffinity({cpu});
    recovered.exec({"child",marker,std::to_string(cpu)}); recovered.wait();
    assert(recovered.exitedOk() && cleanupWaits == before + 1 && !pendingWaitHandle);
    std::remove(marker.c_str());
  }
  std::cout << "PASS: mixed-case environment replacement and nonblocking pending/failed-wait cleanup\n";
  rejectAffinity = alreadyExited = true;
  bool failed = false;
  try {
    FAH::Client::CoreProcess exited(argv[0]); exited.setRequiredAffinity({cpu});
    exited.exec({"child",marker,std::to_string(cpu)});
  } catch (const FAH::Client::AffinityRejected &) {failed = true;}
  assert(failed && !rejectedHandle && !std::ifstream(marker).good());
  alreadyExited = false;
  rejectAffinity = rejectTermination = true;
  failed = false;
  try {
    FAH::Client::CoreProcess rejected(argv[0]); rejected.setRequiredAffinity({cpu});
    rejected.exec({"child",marker,std::to_string(cpu)});
  } catch (...) {failed = true;}
  assert(failed && rejectedHandle);
  assert(WaitForSingleObject(rejectedHandle, 0) == WAIT_TIMEOUT);
  assert(!std::ifstream(marker).good());
  rejectAffinity = false;
  {
    FAH::Client::CoreProcess independent(argv[0]); independent.setRequiredAffinity({cpu});
    independent.exec({"child",marker,std::to_string(cpu)});independent.wait();
    assert(independent.exitedOk() && WaitForSingleObject(rejectedHandle,0)==WAIT_TIMEOUT);
  }
  assert(std::ifstream(marker).good());std::remove(marker.c_str());
  rejectAffinity = true;
  for (unsigned i=0;i<7;++i) {
    failed=false;
    try {FAH::Client::CoreProcess rejected(argv[0]);rejected.setRequiredAffinity({cpu});
      rejected.exec({"child",marker,std::to_string(cpu)});}catch(const FAH::Client::AffinityRejected&){failed=true;}
    assert(failed && !std::ifstream(marker).good());
  }
  unsigned calls = affinityCalls;
  failed = false;
  try {
    FAH::Client::CoreProcess blocked(argv[0]); blocked.setRequiredAffinity({cpu});
    blocked.exec({"child",marker,std::to_string(cpu)});
  } catch (const FAH::Client::AffinityRejected &e) {
    failed = true;
    assert(std::string(e.what()) ==
      "FahCore launch blocked: rejected-child cleanup limit reached (8)");
  }
  assert(failed && affinityCalls == calls); // The retained-child bound prevents further creation.
  rejectAffinity = rejectTermination = false;
  for (unsigned i=0;i<100;++i) {
    try {
      FAH::Client::CoreProcess recovered(argv[0]); recovered.setRequiredAffinity({cpu});
      recovered.exec({"child",marker,std::to_string(cpu)}); recovered.wait();
      assert(recovered.exitedOk()); break;
    } catch (...) {
      assert(i < 99); std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }
  assert(WaitForSingleObject(rejectedHandle, 0) == WAIT_OBJECT_0);
  assert(std::ifstream(marker).good()); CloseHandle(rejectedHandle); rejectedHandle = 0;
  std::remove(marker.c_str());
  std::cout << "PASS: rejected suspended child retained through failed cleanup, independent launches proceed, bounded cleanup retry recovers\n";
#endif
  // No console is fabricated: exercise the actual tray/service-style attach
  // path when the driver launches this test with CREATE_NO_WINDOW.
#ifdef _WIN32
  const bool consoleless = !GetConsoleCP();
  const HANDLE savedInput=GetStdHandle(STD_INPUT_HANDLE);
  const HANDLE savedOutput=GetStdHandle(STD_OUTPUT_HANDLE);
  const HANDLE savedError=GetStdHandle(STD_ERROR_HANDLE);
#endif
  FAH::Client::CoreProcess graceful(argv[0]);graceful.setRequiredAffinity({cpu});
  graceful.exec({"graceful",marker,std::to_string(cpu)});
  for(unsigned i=0;i<500 && !std::ifstream(marker).good();++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  assert(std::ifstream(marker).good());assert(graceful.isRunning());
  graceful.stop();assert(graceful.isStopping());
  for(unsigned i=0;i<500 && graceful.isRunning();++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  if(graceful.isRunning()){graceful.kill();assert(false && "graceful interrupt timed out");}
  assert(graceful.wait()==42);assert(!graceful.getWasKilled());
  assert(!graceful.getKilledByClient());
#ifdef _WIN32
  assert(bool(GetConsoleCP()) == !consoleless);
  assert(GetStdHandle(STD_INPUT_HANDLE)==savedInput);
  assert(GetStdHandle(STD_OUTPUT_HANDLE)==savedOutput);
  assert(GetStdHandle(STD_ERROR_HANDLE)==savedError);
#endif
  {std::string received;std::ifstream(marker)>>received;assert(received=="interrupted");}
  std::remove(marker.c_str());
  std::cout << "PASS: native graceful stop delivers interrupt, preserves exit code, never kills\n";
  FAH::Client::CoreProcess live(argv[0]);live.setRequiredAffinity({cpu});
  live.exec({"child",marker,std::to_string(cpu),"hold"});
  for(unsigned i=0;i<100 && !std::ifstream(marker).good();++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  assert(std::ifstream(marker).good());assert(live.getPID());assert(live.isRunning());
#ifdef _WIN32
  for (const auto &priority: priorities) {
    live.setPriorityOverride(priority);
    assert(live.checkPriorityOverride(true).empty());
    HANDLE child = OpenProcess(PROCESS_SET_INFORMATION | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)live.getPID());
    assert(child);
    assert(GetPriorityClass(child) == live.desiredPriorityClass());
    assert(SetPriorityClass(child, IDLE_PRIORITY_CLASS)); // Simulate the core reset.
    CloseHandle(child);
    assert(live.checkPriorityOverride(true).empty());
  }
  std::cout << "PASS: Windows GPU priority choices, readback, core-reset recovery and removal\n";
#endif
  assert(live.kill());assert(!live.isRunning());assert(live.getWasKilled());
  std::remove(marker.c_str());
  std::cout << "PASS: exact child mask, multi-thread full process pool, rejected masks, live PID, kill/wait lifecycle\n";
}
