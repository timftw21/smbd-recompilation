#pragma once
#include <string>
#include <string_view>

namespace smbd {
void enable_call_trace();
std::string call_trace_report(std::string_view executable_sha256);
}
