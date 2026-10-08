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


#include <fah/client/App.h>
#include <fah/client/Groups.h>
#include <fah/client/Config.h>
#include <fah/client/Unit.h>
#include <fah/client/WUCPUAllocationPlanner.h>
#include <cbang/ApplicationMain.h>
#include <cbang/json/Reader.h>
#include <cbang/Exception.h>
#include <sstream>
#include <iostream>
using namespace cb;
using namespace FAH::Client;
namespace {
JSON::ValuePtr json(const std::string &text) {
  std::istringstream input(text);return JSON::Reader(input).parse();
}
class TestUnit : public Unit {
public:
  TestUnit(App &app) : Unit(app,"",1,16,{"GPU0","GPU1"}) {}
  using Unit::resolveAssignmentResources;
};
class AssignmentResourcesTest : public App {
public:
  void run() override {
    setup();
    // Exercise real Groups preparation/publication with detached nested JSON.
    auto proposal = json(R"({"":{"cpus":0,"paused":true,"cpu_mode":"count",
      "cpu_class_counts":[0,0],"gpus":{"copy-test":false}}})");
    const auto original = proposal->toString();
    getGroups()->configure(*proposal);
    getGroups()->configure(*proposal); // Reusing caller values must not attach them.
    if (proposal->toString()!=original) THROW("Configuration mutated caller JSON");
    auto &published = getGroups()->getGroup("").getConfig();
    proposal->get("")->get("gpus")->insertBoolean("copy-test",true);
    proposal->get("")->get("cpu_class_counts")->append(1U);
    if (published.get("gpus")->getBoolean("copy-test") ||
        published.getCPUClassCounts()!=std::vector<uint32_t>({0,0}))
      THROW("Published configuration shares caller-owned nested values");
    published.get("gpus")->insertBoolean("copy-test",false);
    if (!proposal->get("")->get("gpus")->getBoolean("copy-test"))
      THROW("Caller configuration shares published nested values");
    TestUnit accepted(*this);
    accepted.resolveAssignmentResources(json(R"({"cpus":1,"gpus":["GPU0"]})"));
    if (accepted.getScheduledCPUs()!=1 || accepted.getGPUs()!=std::set<std::string>{"GPU0"})
      THROW("Single assigned GPU did not replace candidates");
    for (const auto &payload : {
      R"({"cpus":1,"gpus":["GPU0","GPU1"]})",
      R"({"cpus":1,"gpus":["GPU0","GPU0"]})",
      R"({"cpus":1,"gpus":["GPU2"]})",
      R"({"cpus":1,"gpus":[""]})",
      R"({"cpus":1,"gpus":"GPU0"})",
      R"({"cpus":1,"gpus":[2]})"}) {
      TestUnit invalid(*this);bool rejected=false;
      try {invalid.resolveAssignmentResources(json(payload));}catch(const Exception&) {rejected=true;}
      if (!rejected || invalid.getScheduledCPUs()!=16 || invalid.getGPUs()!=std::set<std::string>({"GPU0","GPU1"}))
        THROW("Invalid assignment changed candidate resources");
    }
    for (const auto &payload : {R"({"cpus":2})",R"({"cpus":2,"gpus":[]})"}) {
      TestUnit cpu(*this);cpu.resolveAssignmentResources(json(payload));
      if (cpu.getScheduledCPUs()!=2 || cpu.hasGPUs()) THROW("CPU assignment retained candidate GPUs");
    }
    for (const auto &payload : {
      R"({"cpus":4})",
      R"({"cpus":4,"min_cpus":4,"max_cpus":4})",
      R"({"cpus":4,"min_cpus":2,"max_cpus":8})"}) {
      TestUnit valid(*this);valid.resolveAssignmentResources(json(payload));
      if (valid.getScheduledCPUs()!=4 || valid.hasGPUs())
        THROW("Valid CPU bounds were rejected or changed resources");
    }
    for (const auto &payload : {
      R"({"cpus":4,"min_cpus":5,"max_cpus":4,"gpus":["GPU0"]})",
      R"({"cpus":4,"min_cpus":5})",
      R"({"cpus":4,"max_cpus":3})"}) {
      TestUnit invalid(*this);bool rejected=false;
      try {invalid.resolveAssignmentResources(json(payload));}
      catch(const Exception&) {rejected=true;}
      if (!rejected || invalid.getScheduledCPUs()!=16 ||
          invalid.getGPUs()!=std::set<std::string>({"GPU0","GPU1"}))
        THROW("Invalid CPU bounds changed candidate resources");
      auto persisted=json("{\"state\":{\"number\":6,\"id\":\"invalid-bounds\","
        "\"cpus\":16,\"gpus\":[\"GPU0\",\"GPU1\"],\"state\":\"DOWNLOAD\","
        "\"group\":\"\"},\"data\":{\"assignment\":{\"data\":"+std::string(payload)+"}}}");
      rejected=false;
      try {SmartPointer<Unit> loadedInvalid=new Unit(*this,persisted);}
      catch(const Exception&) {rejected=true;}
      if (!rejected) THROW("Reload accepted invalid CPU bounds");
    }
    // Exercise real Unit getters after the scheduler changes runtime workers.
    const std::vector<unsigned> order{0,1,2,3};
    const std::vector<CPUSet> cores{{0},{1},{2},{3}};
    auto recover = [&](const std::string &assignment, unsigned minimum,
        unsigned maximum, unsigned reduced) {
      auto persisted = json("{\"state\":{\"number\":5,\"id\":\"bounds\",\"cpus\":4,"
        "\"gpus\":[],\"state\":\"DOWNLOAD\",\"group\":\"\"},"
        "\"data\":{\"assignment\":{\"data\":" + assignment + "}}}");
      SmartPointer<Unit> unit = new Unit(*this, persisted);
      auto schedule = [&](unsigned budget) {
        auto plan = WUCPUAllocationPlanner::plan(
          {{unit->getID(),unit->getMinCPUs(),unit->getMaxCPUs(),true}},
          budget,order,cores);
        auto it = plan.workers.find(unit->getID());
        unit->setScheduledCPUs(it == plan.workers.end() ? 0 : it->second);
      };
      schedule(reduced);
      if (unit->getScheduledCPUs()!=reduced || unit->getMinCPUs()!=minimum ||
          unit->getMaxCPUs()!=maximum) THROW("Runtime reduction changed assignment bounds");
      schedule(4);
      if (unit->getScheduledCPUs()!=4) THROW("Restored budget did not recover assigned workers");
    };
    recover(R"({"cpus":4})",4,4,0);
    recover(R"({"cpus":4,"min_cpus":1})",1,4,2);
    recover(R"({"cpus":4,"min_cpus":1,"max_cpus":6})",1,6,2);
    TestUnit placeholder(*this);
    placeholder.setScheduledCPUs(2);
    if (placeholder.getMinCPUs()!=2 || placeholder.getMaxCPUs()!=2)
      THROW("Unassigned placeholder fallback changed");
    TestUnit unsent(*this);
    if (!unsent.matchesAssignmentOffer(16,{"GPU0","GPU1"})) THROW("Unsent offer mismatch");
    SmartPointer<Unit> offer = new Unit(*this,json(R"({"state":{"number":3,"id":"offer","cpus":4,"gpus":["GPU0"],"state":"ASSIGN","group":""},"data":{"resources":{"cpu":{"cpus":8},"GPU0":{}}}})"));
    if (!offer->matchesAssignmentOffer(8,{"GPU0"}) ||
        offer->matchesAssignmentOffer(4,{"GPU0"}) ||
        offer->matchesAssignmentOffer(16,{"GPU0"}) ||
        offer->matchesAssignmentOffer(8,{"GPU1"})) THROW("Captured offer comparison failed");
    SmartPointer<Unit> missing = new Unit(*this,json(R"({"state":{"number":4,"id":"missing","cpus":0,"gpus":[],"state":"ASSIGN","group":""},"data":{"resources":{"cpu":{}}}})"));
    if (missing->matchesAssignmentOffer(0,{})) THROW("Missing count accepted");
    TestUnit cancelled(*this);cancelled.abortPendingAssignment();
    if (cancelled.getState()!=UnitState::UNIT_DONE) THROW("Pending assignment was not aborted");
    cancelled.abortPendingAssignment(); // Idempotent after completion.
    SmartPointer<Unit> loaded = new Unit(*this,json(R"({"state":{"number":2,"id":"reload","cpus":16,"gpus":["GPU0","GPU1"],"state":"DOWNLOAD","group":""},"data":{"assignment":{"data":{"cpus":1,"gpus":["GPU1"]}}}})"));
    if (loaded->getGPUs()!=std::set<std::string>{"GPU1"} || loaded->getScheduledCPUs()!=1)
      THROW("Reload retained unselected candidates");
    loaded->abortPendingAssignment();
    if (loaded->getState()!=UnitState::UNIT_DOWNLOAD) THROW("Accepted WU was aborted during migration");
    std::cout<<"PASS: single-GPU assignment resolution, invalid-resource rejection, CPU selection and preparation-state reload\n";
  }
};
}
int main(int argc,char **argv) {return doApplication<AssignmentResourcesTest>(argc,argv);}
