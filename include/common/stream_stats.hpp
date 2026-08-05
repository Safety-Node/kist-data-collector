#pragma once

// Counters shared by every recorded stream (blob or row): received/dropped
// advance on the producer thread, the rest on the writer thread. All
// monotonic — snapshots feed the 1 Hz report and the session summary.
// `dropped` and `write_errors` staying 0 is the losslessness invariant.

#include <cstdint>

namespace kist {

struct StreamStats {
    uint64_t received     = 0;  // records delivered by the sensor callback
    uint64_t written      = 0;  // records persisted
    uint64_t dropped      = 0;  // queue-full drops (0 = lossless recording)
    uint64_t write_errors = 0;  // records the filesystem refused (disk full / I/O error)
    uint64_t wire_gaps    = 0;  // missing publisher seqs (lost before arrival; blob streams only)
    uint64_t bytes        = 0;  // payload bytes persisted
};

} // namespace kist
