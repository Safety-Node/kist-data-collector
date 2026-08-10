#pragma once

// rt/lowcmd / rt/arm_sdk CSV schema — header + one row per LowCmd_ message
// (both topics carry the same unitree_hg type). recv_ns = this host's
// arrival clock (epoch ns), the cross-stream alignment column — LowCmd_
// carries no tick/seq of its own. Per motor: the full command tuple
// (mode, target q/dq/tau, gains kp/kd); q is an absolute joint target.

#include <unitree/idl/hg/LowCmd_.hpp>

#include <cinttypes>
#include <cstdio>
#include <string>

namespace kist {

constexpr int kLowcmdMotors = 35;   // std::array size in unitree_hg LowCmd_

inline std::string lowcmd_header() {
    std::string h = "recv_ns,mode_pr,mode_machine";
    for (int i = 0; i < kLowcmdMotors; ++i) {
        char b[80];
        std::snprintf(b, sizeof(b), ",m%02d_mode,m%02d_q,m%02d_dq,m%02d_tau,m%02d_kp,m%02d_kd",
                      i, i, i, i, i, i);
        h += b;
    }
    return h;
}

inline std::string lowcmd_row(const unitree_hg::msg::dds_::LowCmd_& c, int64_t recv_ns) {
    std::string row;
    row.reserve(3072);
    char b[160];

    std::snprintf(b, sizeof(b), "%" PRId64 ",%u,%u", recv_ns,
                  unsigned(c.mode_pr()), unsigned(c.mode_machine()));
    row += b;

    for (const auto& m : c.motor_cmd()) {
        std::snprintf(b, sizeof(b), ",%u,%.7g,%.7g,%.7g,%.7g,%.7g",
                      unsigned(m.mode()), double(m.q()), double(m.dq()),
                      double(m.tau()), double(m.kp()), double(m.kd()));
        row += b;
    }
    return row;
}

} // namespace kist
