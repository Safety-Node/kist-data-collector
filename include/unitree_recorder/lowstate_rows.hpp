#pragma once

// rt/lowstate CSV schema — header + one row per LowState_ message.
// recv_ns = this host's arrival clock (epoch ns), the cross-stream
// alignment column; tick = the robot's own ms counter.

#include <unitree/idl/hg/LowState_.hpp>

#include <cinttypes>
#include <cstdio>
#include <string>

namespace kist {

constexpr int kLowstateMotors = 35;   // std::array size in unitree_hg LowState_

inline std::string lowstate_header() {
    std::string h =
        "recv_ns,tick,mode_machine,mode_pr,"
        "quat_w,quat_x,quat_y,quat_z,"
        "gyro_x,gyro_y,gyro_z,accel_x,accel_y,accel_z,"
        "rpy_roll,rpy_pitch,rpy_yaw,imu_temp";
    for (int i = 0; i < kLowstateMotors; ++i) {
        char b[64];
        std::snprintf(b, sizeof(b), ",m%02d_q,m%02d_dq,m%02d_ddq,m%02d_tau", i, i, i, i);
        h += b;
    }
    return h;
}

inline std::string lowstate_row(const unitree_hg::msg::dds_::LowState_& s, int64_t recv_ns) {
    std::string row;
    row.reserve(2048);
    char b[128];

    std::snprintf(b, sizeof(b), "%" PRId64 ",%u,%u,%u", recv_ns,
                  s.tick(), unsigned(s.mode_machine()), unsigned(s.mode_pr()));
    row += b;

    const auto& imu = s.imu_state();
    for (float v : imu.quaternion())     { std::snprintf(b, sizeof(b), ",%.7g", double(v)); row += b; }
    for (float v : imu.gyroscope())      { std::snprintf(b, sizeof(b), ",%.7g", double(v)); row += b; }
    for (float v : imu.accelerometer())  { std::snprintf(b, sizeof(b), ",%.7g", double(v)); row += b; }
    for (float v : imu.rpy())            { std::snprintf(b, sizeof(b), ",%.7g", double(v)); row += b; }
    std::snprintf(b, sizeof(b), ",%d", int(imu.temperature()));
    row += b;

    for (const auto& m : s.motor_state()) {
        std::snprintf(b, sizeof(b), ",%.7g,%.7g,%.7g,%.7g",
                      double(m.q()), double(m.dq()), double(m.ddq()), double(m.tau_est()));
        row += b;
    }
    return row;
}

} // namespace kist
