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

#include "CoreProcess.h"
#include "Core.h"

#include <cbang/os/SystemUtilities.h>
#include <cbang/log/Logger.h>
#include <cbang/Exception.h>
#include <cbang/Catch.h>
#include <cbang/os/SysError.h>
#include <limits>
#include <cstring>
#include <cctype>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <cerrno>
#ifdef __linux__
#include <sched.h>
#endif
#endif

using namespace FAH::Client;
using namespace cb;
using namespace std;


struct CoreProcess::StrictProcess {
  uint64_t pid = 0;
#ifdef _WIN32
  HANDLE handle = 0;
  ~StrictProcess() {if (handle) CloseHandle(handle);}
#endif
};


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
  if (!requiredAffinity.empty()) execStrict(args);
  else Subprocess::exec(args, Subprocess::NULL_STDOUT | Subprocess::NULL_STDERR |
    Subprocess::CREATE_PROCESS_GROUP | Subprocess::W32_HIDE_WINDOW,
    Subprocess::PRIORITY_IDLE);
}


void CoreProcess::stop() {
  if (killedByClient) return; // Already killed, just waiting for it to exit

  uint64_t now = Time::now();

  // ``stop()`` is called about once a second while stopping.  A long gap
  // means the client was not running, e.g. system suspend, so restart the
  // grace period rather than spend it while asleep.
  if (interruptTime && 60 < now - lastStop) interruptTime = now;
  lastStop = now;

  if (!interruptTime) {
    interruptTime = now;
    interrupt();

  } else if (interruptTime + 60 < now) {
    LOG_WARNING("Core did not shutdown gracefully, killing process");
    kill();
    killedByClient = true;
  }
}


CoreProcess::~CoreProcess() {
  if (strictProcess && running) {
    try {kill();} CATCH_ERROR;
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
  if (GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, (DWORD)getPID())) return;
#else
  if (!::killpg((pid_t)getPID(), SIGINT)) return;
#endif
  THROW("Failed to interrupt FahCore: " << SysError());
}


void CoreProcess::execStrict(const vector<string> &args) {
  if (isRunning()) THROW("FahCore already running");
  auto child = unique_ptr<StrictProcess>(new StrictProcess);
#ifdef _WIN32
  // cbang exposes representable affinity within one Windows processor group.
  // This launcher cannot encode cross-group masks. Reject out-of-range IDs
  // rather than truncate them, and verify the applied mask before resuming.
  DWORD_PTR requested = 0;
  for (auto cpu: requiredAffinity) {
    if (cpu >= sizeof(requested) * 8) throw AffinityRejected();
    requested |= (DWORD_PTR)1 << cpu;
  }
  map<string, string> environment;
  auto strings = GetEnvironmentStringsA();
  if (!strings) THROW("Failed to read process environment");
  for (const char *entry = strings; *entry; entry += strlen(entry) + 1) {
    const char *equal = strchr(entry + 1, '=');
    if (equal) {
      string key(entry, equal);
      for (auto &letter: key) letter = (char)toupper((unsigned char)letter);
      environment[key] = equal + 1;
    }
  }
  FreeEnvironmentStringsA(strings);
  for (const auto &entry: *this) environment[entry.first] = entry.second;
  vector<char> env;
  for (const auto &entry: environment) {
    string value = entry.first + '=' + entry.second;
    env.insert(env.end(), value.begin(), value.end()); env.push_back(0);
  }
  env.push_back(0);
  // Prepare throwing operations before acquiring the raw handle.
  string command = Subprocess::assemble(args);
  string directory = SystemUtilities::absolute(wd);
  SECURITY_ATTRIBUTES attributes = {sizeof(attributes), 0, TRUE};
  HANDLE nullOutput = CreateFileA("NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
    &attributes, OPEN_EXISTING, 0, 0);
  if (nullOutput == INVALID_HANDLE_VALUE) THROW("Failed to open NUL");
  STARTUPINFOA startup = {}; startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
  startup.wShowWindow = SW_HIDE;
  startup.hStdInput = nullOutput;
  startup.hStdOutput = startup.hStdError = nullOutput;
  PROCESS_INFORMATION process = {};
  BOOL created = CreateProcessA(path.c_str(), &command[0], 0, 0, TRUE,
    CREATE_SUSPENDED | CREATE_NEW_PROCESS_GROUP | IDLE_PRIORITY_CLASS,
    env.data(), directory.c_str(), &startup, &process);
  CloseHandle(nullOutput);
  if (!created) THROW("Failed to create managed FahCore: " << SysError());
  child->handle = process.hProcess; child->pid = process.dwProcessId;
  DWORD_PTR actual = 0, system = 0;
  bool exact = SetProcessAffinityMask(process.hProcess, requested) &&
    GetProcessAffinityMask(process.hProcess, &actual, &system) && actual == requested;
  bool resumed = exact && ResumeThread(process.hThread) != (DWORD)-1;
  if (!resumed) {
    CloseHandle(process.hThread);
    if (!TerminateProcess(process.hProcess, 126))
      THROW("Failed to terminate affinity-rejected FahCore: " << SysError());
    WaitForSingleObject(process.hProcess, INFINITE);
    if (!exact) throw AffinityRejected();
    THROW("Failed to resume managed FahCore");
  }
  CloseHandle(process.hThread);
#elif defined(__linux__)
  // Fixed cpu_set_t limits IDs to CPU_SETSIZE. Reject larger IDs explicitly.
  // Exact read-back also rejects CPUs removed by cpuset/topology changes.
  cpu_set_t requested; CPU_ZERO(&requested);
  for (auto cpu: requiredAffinity) {
    if (cpu >= CPU_SETSIZE) throw AffinityRejected();
    CPU_SET(cpu, &requested);
  }
  // Allocate argv/environment before fork; child setup uses native syscalls.
  vector<char *> argv;
  for (auto &arg: args) argv.push_back(const_cast<char *>(arg.c_str()));
  argv.push_back(0);
  map<string, string> environment;
  for (char **entry = environ; *entry; ++entry) {
    const char *equal = strchr(*entry, '=');
    if (equal) environment[string(*entry, equal - *entry)] = equal + 1;
  }
  for (const auto &entry: *this) environment[entry.first] = entry.second;
  vector<string> values; vector<char *> env;
  for (const auto &entry: environment) values.push_back(entry.first + '=' + entry.second);
  for (auto &value: values) env.push_back(const_cast<char *>(value.c_str()));
  env.push_back(0);
  int status[2];
  if (pipe(status)) THROW("Failed to create affinity status pipe: " << SysError());
  // Keep the handshake separate from redirected standard streams, even when
  // the parent started with closed stdin/stdout/stderr.
  for (unsigned i = 0; i < 2; i++) {
    int fd = fcntl(status[i], F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
    if (fd == -1) {
      close(status[0]); close(status[1]);
      THROW("Failed to protect affinity status pipe");
    }
    close(status[i]); status[i] = fd;
  }
  pid_t pid = fork();
  if (pid == -1) {close(status[0]); close(status[1]); THROW("Failed to fork FahCore");}
  if (!pid) {
    close(status[0]);
    cpu_set_t actual; CPU_ZERO(&actual);
    bool groupReady = !setpgid(0, 0);
    bool exact = !sched_setaffinity(0, sizeof(requested), &requested) &&
      !sched_getaffinity(0, sizeof(actual), &actual) && CPU_EQUAL(&actual, &requested);
    int nullOutput = open("/dev/null", O_WRONLY);
    bool ready = exact && groupReady && !chdir(wd.c_str()) && nullOutput >= 0 &&
      dup2(nullOutput, STDOUT_FILENO) >= 0 && dup2(nullOutput, STDERR_FILENO) >= 0;
    if (nullOutput > STDERR_FILENO) close(nullOutput);
    setpriority(PRIO_PROCESS, 0, 19);
    char result = !exact ? 2 : (ready ? 0 : 1);
    ssize_t sent;
    do {sent = write(status[1], &result, 1);} while (sent < 0 && errno == EINTR);
    close(status[1]);
    if (!ready || sent != 1) _exit(126);
    execve(path.c_str(), argv.data(), env.data());
    _exit(127);
  }
  close(status[1]); char result = 1; ssize_t received;
  do {received = read(status[0], &result, 1);} while (received < 0 && errno == EINTR);
  close(status[0]);
  if (received != 1 || result) {
    ::kill(pid, SIGKILL);
    while (waitpid(pid, 0, 0) < 0 && errno == EINTR) {}
    if (received == 1 && result == 2) throw AffinityRejected();
    THROW("Failed to prepare managed FahCore");
  }
  child->pid = pid;
#else
  // No strict launcher is implemented for macOS/BSD. Ordinary unpinned
  // scheduling remains available when hard affinity is not advertised.
  THROW("Managed FahCore affinity is unsupported on this platform");
#endif
  strictProcess = std::move(child);
  returnCode = exitFlags = 0;
  running = true;
}
