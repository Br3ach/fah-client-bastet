#!/usr/bin/env python3
"""Exercise the production App::configure body with configuration adapters."""
from pathlib import Path
import os, subprocess, tempfile
root = Path(__file__).resolve().parents[2]
source = (root / 'src/fah/client/App.cpp').read_text()
start = source.index('void App::configure(')
body = source[start:source.index('\n\n\nstring App::getURL', start)]
harness = r"""
#include <cassert>
#include <memory>
#include <string>
#include <stdexcept>
using namespace std;
namespace JSON { struct Value {
 bool config=false, groups=false, change=false;
 bool hasDict(const string &key) const {return key=="config"?config:groups;}
 shared_ptr<Value> get(const string &) const {return make_shared<Value>(*this);}
}; }
struct Config {string state="old"; string toString(){return state;}
 void configure(const JSON::Value &v){if(v.change)state="new";}};
struct Groups {int updates=0;bool fail=false;
 void configure(const JSON::Value &){if(fail)throw runtime_error("reject");updates++;}};
struct App {Config config; Groups groups; int updates=0; bool allowed=true;
 bool validateChange(const JSON::Value &){return allowed;}
 Config *getConfig(){return &config;} Groups *getGroups(){return &groups;}
 void triggerUpdate(){updates++;} void configure(const JSON::Value &);
};
""" + body + r"""
int main(){
 for(bool hasConfig:{false,true}) for(bool hasGroups:{false,true})
 for(bool globalChange:{false,true}) {
  App app; JSON::Value msg{hasConfig,hasGroups,globalChange}; app.configure(msg);
  bool groupSave=hasConfig&&hasGroups;
  assert(app.groups.updates==int(groupSave));
  assert(app.updates==int(!groupSave||(hasConfig&&globalChange)));
 }
 App rejected; rejected.groups.fail=true; JSON::Value both{true,true,true};
 try{rejected.configure(both);assert(false);}catch(const runtime_error &){}
 assert(rejected.config.state=="old"&&rejected.updates==0);
 App denied;denied.allowed=false;denied.configure(both);
 assert(denied.groups.updates==0&&denied.updates==0);
}
"""
with tempfile.TemporaryDirectory(prefix='fah-config-update-') as directory:
 build=Path(directory); cpp=build/'test.cpp';cpp.write_text(harness)
 if os.name=='nt':
  subprocess.run(['cl','/nologo','/EHsc','/std:c++17',str(cpp),'/Fetest.exe'],cwd=build,check=True)
  exe=build/'test.exe'
 else:
  exe=build/'test';subprocess.run(['g++','-std=c++17',str(cpp),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
print('PASS: group-only, unchanged/changed global settings, rejection and authorization')
