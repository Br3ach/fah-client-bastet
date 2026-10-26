#include <fah/client/App.h>
#include <fah/client/CoreProcess.h>
#include <fah/client/Unit.h>

#include <cbang/ApplicationMain.h>
#include <cbang/json/Reader.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace fs = std::filesystem;

namespace FAH::Client {
class ViewerUnit : public Unit {
public:
  ViewerUnit(App &app, const cb::JSON::ValuePtr &data) : Unit(app, data) {}
  using Unit::processStarted;
  using Unit::readViewerData;
};

class UnitViewerTest : public App {
public:
  void run() override {
    setup();
    std::istringstream input(
      R"({"data":{},"state":{"id":"viewer-test-unit","number":1,"group":"","state":"CORE","cpus":1}})");
    cb::SmartPointer<ViewerUnit> unit = new ViewerUnit(*this,
      cb::JSON::Reader(input).parse());
    const fs::path directory(unit->getDirectory());
    fs::create_directories(directory);
    const auto top = directory / "viewerTop.json";
    const auto frame = directory / "viewerFrame0.json";
    const auto oldTime = fs::file_time_type::clock::now() -
      std::chrono::seconds(40);
    const auto write = [](const fs::path &path, const char *content,
                          fs::file_time_type time) {
      std::ofstream(path) << content;
      fs::last_write_time(path, time);
    };
    write(top, R"({"atoms":[["old",0,0,0,6]],"bonds":[]})", oldTime);
    write(frame, "[[1,2,3]]", oldTime);
    unit->readViewerData();
    if (unit->getFrames().size() != 1)
      throw std::runtime_error("initial viewer frame missing");

    cb::SmartPointer<CoreProcess> process = new CoreProcess("unused");
    unit->processStarted(process, oldTime);
    unit->readViewerData();
    if (!unit->getTopology().isNull() || !unit->getFrames().empty())
      throw std::runtime_error("stale topology read during slow restart");

    write(top, R"({"atoms":[["new",0,0,0,6]],"bonds":[]})",
          oldTime + std::chrono::seconds(1));
    write(frame, "[[4,5,6]]", oldTime + std::chrono::seconds(1));
    unit->readViewerData();
    if (unit->getTopology().isNull() || unit->getFrames().size() != 1)
      throw std::runtime_error("fresh topology did not enable viewer");

    // Publication can finish before the launch call returns.
    write(top, R"({"atoms":[["fast",0,0,0,6]],"bonds":[]})",
          oldTime + std::chrono::seconds(2));
    unit->processStarted(process, oldTime + std::chrono::seconds(1));
    unit->readViewerData();
    if (unit->getTopology().isNull() || unit->getFrames().size() != 1)
      throw std::runtime_error("fast topology publication was missed");
  }
};
}

int main(int argc, char *argv[]) {
  return cb::doApplication<FAH::Client::UnitViewerTest>(argc, argv);
}
