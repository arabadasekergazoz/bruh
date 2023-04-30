#pragma once
#include <string>

namespace authentication {
	bool login(std::string username, std::string password);
	bool createaccount(std::string username, std::string password, std::string license);
}