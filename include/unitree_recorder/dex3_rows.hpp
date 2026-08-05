#pragma once

// Dex3 hand-state CSV schema — header + one row per HandState_ message.
// Fixed CSV width from the Dex3-1 hardware: 7 finger motors (f0 = thumb
// rotation), and press-sensor pads of 12 pressure channels each (the pad
// constants bound the flattened columns; shorter/absent pads are zero-
// filled, extras are dropped — the message carries them as vectors, but a
// CSV schema must be fixed).

#include <unitree/idl/hg/HandState_.hpp>

#include <cinttypes>
#include <cstdio>
#include <string>

namespace kist {

constexpr int kDex3Motors       = 7;
constexpr int kDex3PressPads    = 9;   // Dex3-1 reports 9 pads in the field
constexpr int kDex3PressChannel = 12;

inline std::string dex3_header() {
    std::string h = "recv_ns";
    char b[64];
    for (int i = 0; i < kDex3Motors; ++i) {
        std::snprintf(b, sizeof(b), ",f%d_q,f%d_dq,f%d_ddq,f%d_tau", i, i, i, i);
        h += b;
    }
    for (int p = 0; p < kDex3PressPads; ++p)
        for (int c = 0; c < kDex3PressChannel; ++c) {
            std::snprintf(b, sizeof(b), ",press%d_%d", p, c);
            h += b;
        }
    return h;
}

inline std::string dex3_row(const unitree_hg::msg::dds_::HandState_& s, int64_t recv_ns) {
    std::string row;
    row.reserve(1024);
    char b[96];

    std::snprintf(b, sizeof(b), "%" PRId64, recv_ns);
    row += b;

    const auto& motors = s.motor_state();
    for (int i = 0; i < kDex3Motors; ++i) {
        if (size_t(i) < motors.size()) {
            const auto& m = motors[size_t(i)];
            std::snprintf(b, sizeof(b), ",%.7g,%.7g,%.7g,%.7g",
                          double(m.q()), double(m.dq()), double(m.ddq()), double(m.tau_est()));
        } else {
            std::snprintf(b, sizeof(b), ",0,0,0,0");
        }
        row += b;
    }

    const auto& pads = s.press_sensor_state();
    for (int p = 0; p < kDex3PressPads; ++p)
        for (int c = 0; c < kDex3PressChannel; ++c) {
            const float v = (size_t(p) < pads.size()) ? pads[size_t(p)].pressure()[size_t(c)] : 0.f;
            std::snprintf(b, sizeof(b), ",%.7g", double(v));
            row += b;
        }
    return row;
}

} // namespace kist
