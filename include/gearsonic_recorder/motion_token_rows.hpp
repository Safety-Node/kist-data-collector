#pragma once

// Motion-token CSV schema — header + one row per MotionTokenState msg
// (rt/kist/motion_token, published by kist-gearsonic-inference at 50 Hz:
// the 64-dim SONIC token its whole-body decoder actually consumed each
// CONTROL tick — the training-data ground truth, not recomputable offline).
//
// seq increments only on decoded ticks: seq/time gaps = the robot was
// outside CONTROL (INIT ramp, damping, e-stop) — not loss. stamp_ns is the
// publisher's computation-tick clock (epoch ns) — the 50 Hz alignment
// column; recv_ns is host arrival. The training export keeps only
// arbiter_mode == 1 (teleop demonstration) rows.
//   arbiter_mode: 0 normal / 1 teleop / 2 vla / 3 recovering
//   encoder_mode: 0 g1 / 1 teleop / 255 = encoder skipped (vla, recovery)

#include "kist_msgs.hpp"

#include <cinttypes>
#include <cstdio>
#include <string>

namespace kist {

constexpr int kMotionTokenDim = 64;

inline std::string motion_token_header() {
    std::string h = "recv_ns,stamp_ns,seq,arbiter_mode,encoder_mode";
    char b[16];
    for (int i = 0; i < kMotionTokenDim; ++i) {
        std::snprintf(b, sizeof(b), ",t%02d", i);
        h += b;
    }
    return h;
}

inline std::string motion_token_row(const kist_msgs::MotionTokenState& m,
                                    int64_t recv_ns) {
    std::string row;
    row.reserve(1024);
    char b[96];

    std::snprintf(b, sizeof(b), "%" PRId64 ",%" PRId64 ",%llu,%u,%u",
                  recv_ns, static_cast<int64_t>(m.stamp_ns()),
                  static_cast<unsigned long long>(m.seq()),
                  unsigned(m.arbiter_mode()), unsigned(m.encoder_mode()));
    row += b;

    for (int i = 0; i < kMotionTokenDim; ++i) {
        std::snprintf(b, sizeof(b), ",%.7g", double(m.token_state()[i]));
        row += b;
    }
    return row;
}

} // namespace kist
