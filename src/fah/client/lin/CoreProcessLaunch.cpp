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

#ifdef __linux__
#include "../CoreProcessInternal.h"
#include <cbang/os/SysError.h>
#include <cbang/Exception.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <sched.h>
#include <cerrno>
#include <cstring>

using namespace FAH::Client;
using namespace cb;
using namespace std;

namespace {
class FileDescriptor {
  int value;
public:
  explicit FileDescriptor(int value) : value(value) {}
  ~FileDescriptor() {reset();}
  FileDescriptor(const FileDescriptor &) = delete;
  FileDescriptor &operator=(const FileDescriptor &) = delete;
  int get() const {return value;}
  // close is deliberately not retried after EINTR: the descriptor may be reused.
  void reset(int replacement = -1) {
    if (value >= 0) close(value);
    value = replacement;
  }
};
}

void CoreProcess::execStrictLinux(const vector<string> &args, StrictProcess &child) {
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
  int descriptors[2];
  if (pipe(descriptors)) THROW("Failed to create affinity status pipe: " << SysError());
  FileDescriptor status[2] = {FileDescriptor(descriptors[0]), FileDescriptor(descriptors[1])};
  // Keep the handshake separate from redirected standard streams, even when
  // the parent started with closed stdin/stdout/stderr.
  for (unsigned i = 0; i < 2; i++) {
    int fd = fcntl(status[i].get(), F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
    if (fd == -1)
      THROW("Failed to protect affinity status pipe: " << SysError());
    status[i].reset(fd);
  }
  pid_t pid = fork();
  if (pid == -1) THROW("Failed to fork FahCore: " << SysError());
  if (!pid) {
    status[0].reset();
    cpu_set_t actual; CPU_ZERO(&actual);
    bool groupReady = !setpgid(0, 0);
    // An empty mask is valid for a priority-only launch.
    // Managed affinity launches verify the requested non-empty mask exactly.
    bool exact = requiredAffinity.empty() || (!sched_setaffinity(0, sizeof(requested), &requested) &&
      !sched_getaffinity(0, sizeof(actual), &actual) && CPU_EQUAL(&actual, &requested));
    int nullOutput = open("/dev/null", O_WRONLY);
    bool ready = exact && groupReady && !chdir(wd.c_str()) && nullOutput >= 0 &&
      dup2(nullOutput, STDOUT_FILENO) >= 0 && dup2(nullOutput, STDERR_FILENO) >= 0;
    if (nullOutput > STDERR_FILENO) close(nullOutput);
    // Match cbang PRIORITY_IDLE before exec; affinity must not change scheduling.
    sched_param priority{};
    bool priorityReady = !priorityOverride.empty() || (!sched_setscheduler(0, SCHED_IDLE, &priority) &&
      sched_getscheduler(0) == SCHED_IDLE && !setpriority(PRIO_PROCESS, 0, 19));
    errno = 0;
    if (priorityOverride.empty())
      priorityReady = priorityReady && getpriority(PRIO_PROCESS, 0) == 19 && !errno;
    if (!priorityOverride.empty()) {
      // Start at OTHER/nice 0: the core's explicit Low/Normal argument only
      // sets nice (10/0), leaving the inherited scheduler unchanged. Starting
      // at nice 19 would normally prevent an unprivileged core raising it to 0.
      bool applied = !sched_setscheduler(0, SCHED_OTHER, &priority) &&
        !setpriority(PRIO_PROCESS, 0, 0);
      if (!applied) {
        // The override is optional, but a fallback must establish stock
        // scheduling. Reject preparation if restoration cannot be verified.
        const bool schedulerRestored = !sched_setscheduler(0, SCHED_IDLE, &priority);
        const bool niceRestored = !setpriority(PRIO_PROCESS, 0, 19);
        errno = 0;
        const int restoredNice = getpriority(PRIO_PROCESS, 0);
        const bool niceVerified = restoredNice == 19 && !errno;
        priorityReady = schedulerRestored && niceRestored && niceVerified &&
          sched_getscheduler(0) == SCHED_IDLE;
      }
    }
    char result = !exact ? 2 :
      (!ready ? 1 : (!priorityReady ? 3 : 0));
    ready = ready && priorityReady;
    ssize_t sent;
    do {sent = write(status[1].get(), &result, 1);} while (sent < 0 && errno == EINTR);
    status[1].reset();
    if (!ready || sent != 1) _exit(126);
    execve(path.c_str(), argv.data(), env.data());
    _exit(127);
  }
  status[1].reset(); char result = 1; ssize_t received;
  do {received = read(status[0].get(), &result, 1);} while (received < 0 && errno == EINTR);
  status[0].reset();
  if (received != 1 || result) {
    ::kill(pid, SIGKILL);
    while (waitpid(pid, 0, 0) < 0 && errno == EINTR) {}
    if (received == 1 && result == 2) throw AffinityRejected();
    if (received == 1 && result == 3) throw SchedulingRejected();
    THROW("Failed to prepare FahCore");
  }
  child.pid = pid;
}
#endif
