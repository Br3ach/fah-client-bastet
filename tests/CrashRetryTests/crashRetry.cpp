// Exercise real Unit exit handling and SQLite persistence with disposable children.
#include <fah/client/App.h>
#include <fah/client/CoreProcess.h>
#include <fah/client/ExitCode.h>
#include <fah/client/Unit.h>
#ifdef _WIN32
#include <windows.h>
#endif
#include <cbang/ApplicationMain.h>
#include <cbang/db/NameValueTable.h>
#include <cbang/json/Reader.h>
#include <cbang/os/SystemUtilities.h>
#include <fstream>
#include <cstdlib>
#include <sstream>
#include <stdexcept>
#include <iostream>

using namespace FAH::Client;

static void require(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}

static cb::JSON::ValuePtr parse(const std::string &text) {
  std::istringstream stream(text);
  return cb::JSON::Reader(stream).parse();
}

class TestUnit : public Unit {
public:
  TestUnit(App &app, const cb::JSON::ValuePtr &blob) : Unit(app, blob) {
    setState(UNIT_RUN);
  }

  void exitWith(unsigned code, bool killed = false) {
    cb::SmartPointer<CoreProcess> child =
      new CoreProcess(cb::SystemUtilities::getExecutablePath());
#ifdef _WIN32
    if (killed) {
      DWORD_PTR available, system;
      require(GetProcessAffinityMask(GetCurrentProcess(), &available, &system),
        "cannot read test CPU mask");
      unsigned cpu = 0;
      while (!(available & (DWORD_PTR(1) << cpu))) ++cpu;
      child->setRequiredAffinity({cpu});
    }
#endif
    child->exec({"--child", std::to_string(code), killed ? "wait" : "exit"});
    processStarted(child);
    if (killed) child->kill();
    finalizeRun();
  }
};

class CrashRetryTest : public App {
  bool quitting = false;
  cb::SmartPointer<TestUnit> make(const std::string &id) {
    return new TestUnit(*this, parse(
      "{\"data\":{\"request\":{},\"assignment\":{\"data\":{\"cpus\":1}}},"
      "\"state\":{\"number\":1,\"id\":\"" + id + "\",\"group\":\"\","
      "\"state\":\"ASSIGN\",\"cpus\":1,\"gpus\":[],\"run_time\":0}}"));
  }

public:
  bool shouldQuit() const override {return quitting || App::shouldQuit();}

  void run() override {
    setup();
    cb::SystemUtilities::ensureDirectory("work");
    const unsigned crash = 0xc0000409U;
    if (std::getenv("FAH_CRASH_TEST_RELOAD")) {
      auto blob = getDB("units").getJSON("persist");
      require(blob->selectU32("state.crash_retries") == 1, "DB marker lost");
      cb::SmartPointer<TestUnit> unit = new TestUnit(*this, blob);
      require(unit->getU32("crash_retries") == 1, "reload marker lost");
      require(unit->getRetries() == 0, "ordinary retry counter did not reset on load");
      unit->exitWith(crash);
      require(unit->getState() == UnitState::UNIT_DUMP, "reload granted another retry");
      std::cout << "reload: crash budget survives process restart\n";
      return;
    }

    auto unit = make("twice");
    unit->exitWith(crash);
    require(unit->getState() == UnitState::UNIT_RUN, "first crash dumped WU");
    require(unit->getU32("crash_retries") == 1, "first crash not recorded");
    require(unit->getRetries() == 1 && unit->isWaiting(), "no retry backoff");
    unit->exitWith(crash);
    require(unit->getState() == UnitState::UNIT_DUMP, "second crash retried");

    unit = make("normal-restart");
    unit->exitWith(ExitCode::CORE_RESTART);
    unit->exitWith(ExitCode::CORE_RESTART);
    require(unit->getState() == UnitState::UNIT_RUN && unit->getRetries() == 2,
      "ordinary restart changed");
    require(!unit->has("crash_retries"), "ordinary restart spent crash budget");

    unit = make("interrupted");
    unit->exitWith(ExitCode::INTERRUPTED);
    require(unit->getState() == UnitState::UNIT_RUN && !unit->has("crash_retries"),
      "normal interruption spent crash budget");

    unit = make("explicit-failure");
    unit->exitWith(ExitCode::FAILED_1);
    require(unit->getState() == UnitState::UNIT_DUMP && !unit->has("crash_retries"),
      "explicit failure retried");

    unit = make("killed");
    unit->exitWith(0, true);
    require(unit->getState() == UnitState::UNIT_DUMP && !unit->has("crash_retries"),
      "deliberate kill retried");

    unit = make("results");
    cb::SystemUtilities::ensureDirectory(unit->getDirectory());
    std::ofstream(unit->getDirectory() + "/wuresults_01.dat") << "test result";
    unit->exitWith(crash);
    require(unit->getState() == UnitState::UNIT_UPLOAD && !unit->has("crash_retries"),
      "existing results bypassed");

    unit = make("persist");
    unit->exitWith(crash);
    require(unit->getState() == UnitState::UNIT_RUN, "persistence case did not retry");
    require(getDB("units").getJSON("persist")->selectU32("state.crash_retries") == 1,
      "crash marker not saved immediately");

    unit = make("shutdown");
    quitting = true;
    unit->exitWith(crash);
    require(unit->getState() == UnitState::UNIT_RUN && !unit->has("crash_retries"),
      "shutdown spent crash budget");
    std::cout << "exit handling: first/second crash, restart, interruption, failure, kill, results, shutdown passed\n";
  }
};

int main(int argc, char **argv) {
#ifdef _WIN32
  if (argc == 4 && std::string(argv[1]) == "--child") {
    if (std::string(argv[3]) == "wait") Sleep(30000);
    ExitProcess((unsigned)std::stoull(argv[2]));
  }
  return cb::doApplication<CrashRetryTest>(argc, argv);
#else
  std::cout << "Windows exception-exit tests require Windows\n";
#endif
}
