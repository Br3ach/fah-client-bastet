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
#include <fah/client/Config.h>
#include <cbang/ApplicationMain.h>
#include <cbang/Exception.h>
#include <cbang/json/Reader.h>
#include <cbang/util/Resource.h>
#include <iostream>
#include <limits>

using namespace cb;
using namespace FAH::Client;
namespace FAH {namespace Client {extern const DirectoryResource resource0;}}

static void require(bool condition, const char *message) {
  if (!condition) THROW(message);
}

class ConfigValidationTest : public App {
public:
  void run() override {
    const auto defaults = JSON::Reader::parse(resource0.get("group.json"));
    for (const auto payload : {
        R"({"cpu_mode":"invalid"})", R"({"cpu_mode":1})",
        R"({"cpu_class_counts":{}})", R"({"cpu_class_counts":[1.5]})",
        R"({"cpu_class_counts":[-1]})", R"({"cpu_class_counts":[4294967296]})",
        R"({"cpu_class_counts":["1"]})", R"({"gpu_reserved_cores":-1})",
        R"({"gpu_reserved_cores":1.5})", R"({"gpu_reserved_cores":4294967296})"}) {
      Config config(*this, defaults->copy(true));
      bool rejected = false;
      try {config.load(*JSON::parse(payload));}
      catch (const Exception &) {rejected = true;}
      require(rejected, payload);
    }
    for (const auto value : {"0", "4294967295"})
      Config::validateCPUCount(*JSON::parse(value));
    for (const auto value : {"-1", "1.5", "4294967296", "true", "null", "\"1\""}) {
      bool rejected = false;
      try {Config::validateCPUCount(*JSON::parse(value));}
      catch (const Exception &) {rejected = true;}
      require(rejected, value);
    }
    Config classes(*this, defaults->copy(true));
    classes.load(*JSON::parse(R"({"cpu_mode":"classes","cpu_class_counts":[2,3]})"));
    require(classes.getConfiguredCPUTotal() == 5, "Valid class intent lost");
    Config dormant(*this, defaults->copy(true));
    dormant.load(*JSON::parse(R"({"cpus":4,"cpu_class_counts":[4294967295,1]})"));
    require(dormant.getConfiguredCPUTotal() == 4 &&
      dormant.getCPUClassCounts().size() == 2, "Dormant class intent lost");
    classes.load(*JSON::parse(R"({"cpu_class_counts":[4294967295,1]})"));
    require(classes.getConfiguredCPUTotal() == std::numeric_limits<uint32_t>::max(),
      "Loaded class-total saturation changed");
    Config priority(*this, defaults->copy(true));
    priority.load(*JSON::parse(R"({"gpu_priority":"unsupported"})"));
    require(priority.getGPUPriority().empty(), "Unsupported priority retained");
    Config legacy(*this, defaults->copy(true));
    legacy.load(*JSON::parse(R"({"cpus":1.5})"));
    require(legacy.getConfiguredCPUTotal() == 1, "Legacy loading changed");
    Config::validateCPUSetting("unrelated", *JSON::parse("null"));
    std::cout << "PASS: real Config field validation and loading contracts\n";
  }
};

int main(int argc, char *argv[]) {
  return doApplication<ConfigValidationTest>(argc, argv);
}
