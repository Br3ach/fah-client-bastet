#pragma once
#include "Config.h"
namespace FAH { namespace Client { class Group { Config c; public: Group()=default; Group(const Config&x):c(x){} const Config& getConfig() const{return c;} Config& getConfig(){return c;} }; }}
