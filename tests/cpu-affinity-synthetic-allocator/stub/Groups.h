#pragma once
#include <map>
#include <vector>
#include <string>
#include "Group.h"
namespace FAH { namespace Client { class Groups { std::map<std::string,Group> g; public:
  void set(const std::string&n,const Config&c){g[n]=Group(c);} std::vector<std::string> keys() const {std::vector<std::string> r;for(auto&p:g)r.push_back(p.first);return r;}
  const Group& getGroup(const std::string&n) const{return g.at(n);} Group& getGroup(const std::string&n){return g.at(n);} }; }}
