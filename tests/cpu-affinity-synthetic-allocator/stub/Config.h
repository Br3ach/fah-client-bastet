#pragma once
#include <string>
#include <set>
#include <vector>
#include <cstdint>
namespace FAH { namespace Client {
class Config {
  bool classes=false; std::vector<uint32_t> counts; uint32_t cpus=0; bool pin=false;
public:
  Config()=default;
  std::set<std::string> gpus;
  uint32_t reserved=0;
  std::set<std::string> getGPUs() const {return gpus;}
  uint32_t getGPUReservedCores() const {return reserved;}
  static Config Class(std::vector<uint32_t> c){Config x; x.classes=true; x.counts=c; return x;}
  static Config Count(uint32_t n){Config x; x.cpus=n; return x;}
  void setClass(std::vector<uint32_t> c){classes=true;counts=c;cpus=0;}
  void setCount(uint32_t n){classes=false;counts.clear();cpus=n;}
  bool usesCPUClasses() const{return classes;}
  std::vector<uint32_t> getCPUClassCounts() const{return counts;}
  uint32_t getCPUs() const{return classes?getConfiguredCPUTotal():cpus;}
  uint32_t getConfiguredCPUTotal() const{uint32_t n=classes?0:cpus; for(auto v:counts)n+=v; return n;}
  std::string getCPUConfigDescription() const{return classes?"classes":"count";}
};
}}
