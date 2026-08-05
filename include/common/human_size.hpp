#pragma once

// Human-readable size for the 1 Hz reports: B / KB / MB. Thin streams
// (UWB, ~650 B/s) would otherwise sit at "0.0 MB" for minutes.

#include <cstdint>
#include <cstdio>
#include <string>

namespace kist {

inline std::string human_size(uint64_t bytes) {
    char b[32];
    if (bytes >= 1000000)   std::snprintf(b, sizeof(b), "%6.1f MB", double(bytes) / 1e6);
    else if (bytes >= 1000) std::snprintf(b, sizeof(b), "%6.1f KB", double(bytes) / 1e3);
    else                    std::snprintf(b, sizeof(b), "%4llu B ", (unsigned long long)bytes);
    return b;
}

} // namespace kist
