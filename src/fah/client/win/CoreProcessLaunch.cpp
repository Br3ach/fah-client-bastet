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

#include "../CoreProcessInternal.h"
#include <cbang/os/SystemUtilities.h>
#include <cbang/os/SysError.h>
#include <cbang/Exception.h>
#include <cstring>
#include <array>
#include <cctype>
#include <cbang/log/Logger.h>

#ifdef FAH_STRICT_LAUNCH_TEST
extern BOOL testSetProcessAffinityMask(HANDLE, DWORD_PTR);
extern BOOL testTerminateProcess(HANDLE, UINT);
extern DWORD testWaitForSingleObject(HANDLE, DWORD);
#define SetProcessAffinityMask testSetProcessAffinityMask
#define TerminateProcess testTerminateProcess
#define WaitForSingleObject testWaitForSingleObject
#endif

using namespace FAH::Client;
using namespace cb;
using namespace std;

namespace {
// Retain failed suspended children independently. A single unkillable child
// does not disable unrelated launches; the bound prevents unbounded orphaning.
struct RejectedChildren {
  static constexpr size_t MaxChildren = 8;
  struct Child {HANDLE handle = 0; DWORD pid = 0;};
  array<Child, MaxChildren> children{};
  ~RejectedChildren() {
    for (auto &child: children)
      if (child.handle) {TerminateProcess(child.handle, 126); CloseHandle(child.handle);}
  }
  void clean() {
    bool room = false;
    for (auto &child: children) {
      if (child.handle) {
        DWORD state = WaitForSingleObject(child.handle, 0);
        if (state != WAIT_OBJECT_0) {
          TerminateProcess(child.handle, 126);
          state = WaitForSingleObject(child.handle, 0);
        }
        if (state == WAIT_OBJECT_0) {
          CloseHandle(child.handle); child = {};
        }
      }
      room |= !child.handle;
    }
    // Reserve cleanup capacity before creating another potentially failed child.
    if (!room) throw AffinityRejected(
      "FahCore launch blocked: rejected-child cleanup limit reached (8)");
  }
  void retain(HANDLE handle, DWORD pid) noexcept {
    for (auto &child: children) if (!child.handle) {child = {handle, pid}; return;}
  }
};
RejectedChildren rejectedChildren;

// Temporary launch resources never outlive this setup operation.
class Handle {
  HANDLE value;
public:
  explicit Handle(HANDLE value) : value(value) {}
  ~Handle() {if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value);}
  Handle(const Handle &) = delete;
  Handle &operator=(const Handle &) = delete;
  HANDLE get() const {return value;}
};

class EnvironmentStrings {
  LPCH value = GetEnvironmentStringsA();
public:
  ~EnvironmentStrings() {if (value) FreeEnvironmentStringsA(value);}
  EnvironmentStrings() = default;
  EnvironmentStrings(const EnvironmentStrings &) = delete;
  EnvironmentStrings &operator=(const EnvironmentStrings &) = delete;
  LPCH get() const {return value;}
};
}

void CoreProcess::execStrictWindows(const vector<string> &args, StrictProcess &child) {
  rejectedChildren.clean();
  // cbang exposes representable affinity within one Windows processor group.
  // This launcher cannot encode cross-group masks. Reject out-of-range IDs
  // rather than truncate them, and verify the applied mask before resuming.
  DWORD_PTR requested = 0;
  for (auto cpu: requiredAffinity) {
    if (cpu >= sizeof(requested) * 8) throw AffinityRejected();
    requested |= (DWORD_PTR)1 << cpu;
  }
  map<string, string> environment;
  EnvironmentStrings ownedStrings;
  auto strings = ownedStrings.get();
  if (!strings) THROW("Failed to read process environment");
  for (const char *entry = strings; *entry; entry += strlen(entry) + 1) {
    const char *equal = strchr(entry + 1, '=');
    if (equal) {
      string key(entry, equal);
      for (auto &letter: key) letter = (char)toupper((unsigned char)letter);
      environment[key] = equal + 1;
    }
  }
  for (const auto &entry: *this) {
    string key = entry.first;
    for (auto &letter: key) letter = (char)toupper((unsigned char)letter);
    environment[key] = entry.second;
  }
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
  Handle nullOutput(CreateFileA("NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
    &attributes, OPEN_EXISTING, 0, 0));
  if (nullOutput.get() == INVALID_HANDLE_VALUE) THROW("Failed to open NUL");
  STARTUPINFOA startup = {}; startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
  startup.wShowWindow = SW_HIDE;
  startup.hStdInput = nullOutput.get();
  startup.hStdOutput = startup.hStdError = nullOutput.get();
  PROCESS_INFORMATION process = {};
  BOOL created = CreateProcessA(path.c_str(), &command[0], 0, 0, TRUE,
    CREATE_SUSPENDED | CREATE_NEW_PROCESS_GROUP | IDLE_PRIORITY_CLASS,
    env.data(), directory.c_str(), &startup, &process);
  if (!created) THROW("Failed to create managed FahCore: " << SysError());
  child.handle = process.hProcess;
  child.pid = process.dwProcessId;
  Handle thread(process.hThread);
  DWORD_PTR actual = 0, system = 0;
  bool exact = SetProcessAffinityMask(process.hProcess, requested) &&
    GetProcessAffinityMask(process.hProcess, &actual, &system) && actual == requested;
  // Priority is best effort; a denied override must not reject a valid mask.
  if (exact && !priorityOverride.empty())
    SetPriorityClass(process.hProcess, desiredPriorityClass());
  bool resumed = exact && ResumeThread(thread.get()) != (DWORD)-1;
  if (!resumed) {
    const bool terminated = TerminateProcess(process.hProcess, 126);
    const DWORD terminationError = terminated ? ERROR_SUCCESS : GetLastError();
    const DWORD state = WaitForSingleObject(process.hProcess, 0);
    const DWORD waitError = state == WAIT_FAILED ? GetLastError() : ERROR_SUCCESS;
    if (state != WAIT_OBJECT_0) {
      rejectedChildren.retain(child.handle, child.pid);
      child.handle = 0;
      LOG_WARNING("Retaining rejected FahCore PID " << child.pid
        << " for cleanup; termination error " << terminationError
        << ", wait result " << state << ", wait error " << waitError);
      throw AffinityRejected();
    }
    if (!exact) throw AffinityRejected();
    THROW("Failed to resume managed FahCore");
  }
}
