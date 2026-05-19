#pragma once
#include <chrono>
#include <cstdint>
#include <string>

namespace Time {

int64_t nowMonotonicMs();

std::string formatStamp(std::chrono::system_clock::time_point t);

} // namespace Time
