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

#include "CoreProcessInternal.h"
#include "Core.h"
#include "GPUProcessPriority.h"

#include <cbang/os/SystemUtilities.h>
#include <cbang/log/Logger.h>
#include <cbang/Exception.h>
#include <cbang/Catch.h>
#include <cbang/os/SysError.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#include <signal.h>
#ifdef __linux__
#include <sched.h>
#include <sys/resource.h>
#include <dirent.h>
#include <cerrno>
#include <cstdlib>
#endif
#endif

using namespace FAH::Client;
using namespace cb;
using namespace std;


CoreProcess::CoreProcess(const std::string &path) :
  path(SystemUtilities::absolute(path)) {

  // Set environment library paths
  vector<string> paths;
  paths.push_back(SystemUtilities::dirname(this->path));
  const string &ldPath = SystemUtilities::library_path;
  if (SystemUtilities::getenv(ldPath))
    SystemUtilities::splitPaths(SystemUtilities::getenv(ldPath), paths);
  set(ldPath, SystemUtilities::joinPaths(paths));

  // Set working directory
  setWorkingDirectory("work");
}


void CoreProcess::exec(const vector<string> &_args) {
  vector<string> args;
  args.push_back(path);
  args.insert(args.end(), _args.begin(), _args.end());

  LOG_INFO(3, "Running FahCore: " << Subprocess::assemble(args));
  bool nativeLaunch = !requiredAffinity.empty();
#ifdef __linux__
  nativeLaunch |= !priorityOverride.empty();
#endif
  auto launchPriority = Subprocess::PRIORITY_IDLE;
#ifdef _WIN32
  // cbang applies these classes before child code runs. Its HIGH means
  // Above normal. Windows High has no cbang equivalent: unpinned launches
  // start at Idle and are raised immediately after exec(); strict launches
  // apply High while the child is suspended.
  if (priorityOverride == "below-normal") launchPriority = Subprocess::PRIORITY_LOW;
  else if (priorityOverride == "normal") launchPriority = Subprocess::PRIORITY_NORMAL;
  else if (priorityOverride == "above-normal") launchPriority = Subprocess::PRIORITY_HIGH;
#endif
  if (nativeLaunch) execStrict(args);
  else Subprocess::exec(args, Subprocess::NULL_STDOUT | Subprocess::NULL_STDERR |
    Subprocess::CREATE_PROCESS_GROUP | Subprocess::W32_HIDE_WINDOW,
    launchPriority);
#ifdef _WIN32
  if (!priorityOverride.empty()) checkPriorityOverride(true);
#endif
}


void CoreProcess::stop() {
  static constexpr uint64_t ShutdownGraceSeconds = 60;
  if (killedByClient) return; // Already killed, just waiting for it to exit

  uint64_t now = Time::now();

  // ``stop()`` is called about once a second while stopping.  A long gap
  // means the client was not running, e.g. system suspend, so restart the
  // grace period rather than spend it while asleep.
  if (interruptTime && ShutdownGraceSeconds < now - lastStop) interruptTime = now;
  lastStop = now;

  if (!interruptTime) {
    interruptTime = now;
    interrupt();

  } else if (interruptTime + ShutdownGraceSeconds < now) {
    LOG_WARNING("Core did not shutdown gracefully, killing process");
    kill();
    killedByClient = true;
  }
}


CoreProcess::~CoreProcess() {
  if (strictProcess && running) {
    // Cleanup and diagnostic logging must not escape this destructor.
    try {
      try {
        if (!kill())
          LOG_ERROR("Failed to terminate FahCore PID " << getPID()
            << " during cleanup; it may still be running");
      } CBANG_CATCH_ALL(CBANG_LOG_ERROR_LEVEL, " during FahCore cleanup");
    } catch (...) {}
  }
  // The cbang base owns only legacy launches, never the native strict child.
  if (strictProcess) running = false;
}

void CoreProcess::setRequiredAffinity(const std::set<unsigned> &cpus) {
  if (cpus.empty()) THROW("Required FahCore affinity cannot be empty");
  requiredAffinity = cpus;
}


uint64_t CoreProcess::getPID() const {
  return strictProcess ? strictProcess->pid : Subprocess::getPID();
}


bool CoreProcess::isRunning() {
  bool active;
  if (!strictProcess) active = Subprocess::isRunning();
  else {if (running) wait(true); active = running;}
  return active;
}


int CoreProcess::wait(bool nonblocking) {
  if (!strictProcess) return Subprocess::wait(nonblocking);
  if (!running) return returnCode;
#ifdef _WIN32
  DWORD result = WaitForSingleObject(strictProcess->handle, nonblocking ? 0 : INFINITE);
  if (result == WAIT_TIMEOUT) return returnCode;
  if (result != WAIT_OBJECT_0) THROW("Failed to wait for FahCore: " << SysError());
  DWORD code;
  if (!GetExitCodeProcess(strictProcess->handle, &code))
    THROW("Failed to read FahCore exit code: " << SysError());
  returnCode = (int)code;
  exitFlags |= PROCESS_EXITED;
  running = false;
#else
  running = !SystemUtilities::waitPID(getPID(), &returnCode, nonblocking, &exitFlags);
#endif
  return returnCode;
}


bool CoreProcess::kill(bool nonblocking) {
  if (!strictProcess) return Subprocess::kill(nonblocking);
  if (!isRunning()) return true;
#ifdef _WIN32
  if (!TerminateProcess(strictProcess->handle, (UINT)-1)) return false;
  exitFlags |= PROCESS_SIGNALED;
#else
  if (::killpg((pid_t)getPID(), SIGKILL)) return false;
#endif
  if (!nonblocking) wait();
  return true;
}


void CoreProcess::interrupt() {
  if (!strictProcess) return Subprocess::interrupt();
  if (!isRunning()) return;
#ifdef _WIN32
  // Tray/service callers may have no console; their hidden child gets its own.
  // Attach only while delivering the event and preserve redirected client I/O.
  const bool attach = !GetConsoleCP();
  HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
  HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
  HANDLE error = GetStdHandle(STD_ERROR_HANDLE);
  if (attach && !AttachConsole((DWORD)getPID()))
    THROW("Failed to attach FahCore console: " << SysError());
  const bool sent = GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, (DWORD)getPID());
  const DWORD failure = sent ? ERROR_SUCCESS : GetLastError();
  if (attach) {
    FreeConsole();
    SetStdHandle(STD_INPUT_HANDLE, input);
    SetStdHandle(STD_OUTPUT_HANDLE, output);
    SetStdHandle(STD_ERROR_HANDLE, error);
  }
  if (sent) return;
  SetLastError(failure);
#else
  if (!::killpg((pid_t)getPID(), SIGINT)) return;
#endif
  THROW("Failed to interrupt FahCore: " << SysError());
}


void CoreProcess::execStrict(const vector<string> &args) {
  if (isRunning()) THROW("FahCore already running");
  auto child = unique_ptr<StrictProcess>(new StrictProcess);
#ifdef _WIN32
  execStrictWindows(args, *child);
#elif defined(__linux__)
  execStrictLinux(args, *child);
#else
  // No strict launcher is implemented for macOS/BSD. Ordinary unpinned
  // scheduling remains available when hard affinity is not advertised.
  THROW("Managed FahCore affinity is unsupported on this platform");
#endif
  strictProcess = std::move(child);
  returnCode = exitFlags = 0;
  running = true;
}

#ifdef _WIN32
unsigned long CoreProcess::desiredPriorityClass() const {
  if (priorityOverride == "below-normal") return BELOW_NORMAL_PRIORITY_CLASS;
  if (priorityOverride == "normal") return NORMAL_PRIORITY_CLASS;
  if (priorityOverride == "above-normal") return ABOVE_NORMAL_PRIORITY_CLASS;
  if (priorityOverride == "high") return HIGH_PRIORITY_CLASS;
  return IDLE_PRIORITY_CLASS;
}
#endif

string CoreProcess::checkPriorityOverride(bool apply) {
  if (!isRunning()) return "";
#ifdef _WIN32
  HANDLE handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION |
    (apply ? PROCESS_SET_INFORMATION : 0), FALSE, (DWORD)getPID());
  if (!handle) return "GPU priority could not be checked or applied.";
  DWORD wanted = desiredPriorityClass(), actual = GetPriorityClass(handle);
  bool ok = actual == wanted;
  if (!ok && apply) ok = SetPriorityClass(handle, wanted) && GetPriorityClass(handle) == wanted;
  CloseHandle(handle); // Release before logging, which can throw.
  if (actual && actual != wanted && !priorityMismatchLogged) {
    priorityMismatchLogged = true;
    LOG_INFO(1, "GPU core PID " << getPID() << " CPU priority differs; selected "
      << (priorityOverride.empty() ? "stock idle" : priorityOverride));
  }
  return ok ? "" : "GPU priority override could not be applied; folding continues at the available priority.";
#elif defined(__linux__)
  (void)apply;
  if (priorityOverride.empty()) return "";
  const int wantedNice = GPUProcessPriority::niceValue(priorityOverride);
  string directory = "/proc/" + to_string(getPID()) + "/task";
  DIR *tasks = opendir(directory.c_str());
  if (!tasks) return "GPU priority could not be checked.";

  // Linux scheduling attributes are per-thread.
  bool checked = false;
  bool ok = true;
  while (true) {
    // Thread queries also set errno; reset before every directory read.
    errno = 0;
    auto entry = readdir(tasks);
    if (!entry) {
      if (errno) ok = false;
      break;
    }
    char *end;
    long tid = strtol(entry->d_name, &end, 10);
    if (tid <= 0 || *end) continue;
    int policy = sched_getscheduler((pid_t)tid);
    if (policy < 0) {
      if (errno != ESRCH) ok = false;
      continue; // Ignore threads that exited during inspection.
    }
    errno = 0;
    int nice = getpriority(PRIO_PROCESS, (id_t)tid);
    if (errno) {
      if (errno != ESRCH) ok = false;
      continue;
    }
    checked = true;
    if (policy != SCHED_OTHER || nice != wantedNice) ok = false;
  }
  closedir(tasks);
  // No successfully inspected surviving thread means no confirmation.
  return ok && checked ? "" : "GPU priority override was not confirmed for all helper threads; folding continues at the available priority.";
#else
  (void)apply;
  return "GPU priority override is unsupported on this platform.";
#endif
}
