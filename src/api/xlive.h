#pragma once

#include <stdint.h>

#include <string>

namespace xls {

bool Initialised();
uint32_t TitleId();
uint32_t TitleVersion();
std::string LinkStamp(); // the dll's link time, "YYYY-MM-DD HH:MM UTC"

}
