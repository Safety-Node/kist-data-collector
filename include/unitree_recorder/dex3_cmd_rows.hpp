#pragma once

// Dex3 hand-command CSV schema — header + one row per HandCmd_ message.
// The command twin of dex3_rows.hpp: same 7 fixed motor columns (f0 = thumb
// rotation), but the command tuple (mode, target q/dq/tau, gains kp/kd)
// instead of the measured state. The message carries the motors as a
// vector; shorter/absent slots are zero-filled, extras dropped — a CSV
// schema must be fixed. recv_ns is the only timestamp (no tick/seq).

#include <unitree/idl/hg/HandCmd_.hpp>

#include <cinttypes>
#include <cstdio>
#include <string>

namespace kist {

constexpr int kDex3CmdMotors = 7;

inline std::string dex3_cmd_header() {
    std::string h = "recv_ns";
    char b[80];
    for (int i = 0; i < kDex3CmdMotors; ++i) {
        std::snprintf(b, sizeof(b), ",f%d_mode,f%d_q,f%d_dq,f%d_tau,f%d_kp,f%d_kd",
                      i, i, i, i, i, i);
        h += b;
    }
    return h;
}

inline std::string dex3_cmd_row(const unitree_hg::msg::dds_::HandCmd_& c, int64_t recv_ns) {
    std::string row;
    row.reserve(1024);
    char b[160];

    std::snprintf(b, sizeof(b), "%" PRId64, recv_ns);
    row += b;

    const auto& motors = c.motor_cmd();
    for (int i = 0; i < kDex3CmdMotors; ++i) {
        if (size_t(i) < motors.size()) {
            const auto& m = motors[size_t(i)];
            std::snprintf(b, sizeof(b), ",%u,%.7g,%.7g,%.7g,%.7g,%.7g",
                          unsigned(m.mode()), double(m.q()), double(m.dq()),
                          double(m.tau()), double(m.kp()), double(m.kd()));
        } else {
            std::snprintf(b, sizeof(b), ",0,0,0,0,0,0");
        }
        row += b;
    }
    return row;
}

} // namespace kist
