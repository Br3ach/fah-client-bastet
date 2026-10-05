#pragma once
#include <iostream>
#include <sstream>
#define LOG_DEBUG(a,b) do { if ((a)<=1) {std::ostringstream _x; _x << b; std::cout << "D " << _x.str() << "\n";} } while(0)
#define LOG_INFO(a,b) do { std::ostringstream _x; _x << b; std::cout << "I " << _x.str() << "\n"; } while(0)
#define LOG_WARNING(b) do { std::ostringstream _x; _x << b; std::cout << "W " << _x.str() << "\n"; } while(0)
#define LOG_ERROR(b) do { std::ostringstream _x; _x << b; std::cout << "E " << _x.str() << "\n"; } while(0)
namespace cb {}
