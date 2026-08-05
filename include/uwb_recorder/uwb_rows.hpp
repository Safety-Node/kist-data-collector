#pragma once

// UWB fix CSV schema — header + one row per rt/kist/uwb/pose fix.
// recv_ns = this host's arrival clock (epoch ns), the cross-stream
// alignment column; stamp_ns = the transmitter's capture clock.

#include "uwb/uwb_position.hpp"

#include <cinttypes>
#include <cstdio>
#include <string>

namespace kist {

constexpr const char* kUwbCsvHeader = "recv_ns,stamp_ns,x,y,z";

inline std::string uwb_row(const UwbPosition& fix, int64_t recv_ns) {
    char buf[128];
    std::snprintf(buf, sizeof(buf), "%" PRId64 ",%" PRId64 ",%.7g,%.7g,%.7g",
                  recv_ns, fix.stamp_ns, double(fix.x), double(fix.y), double(fix.z));
    return buf;
}

} // namespace kist
