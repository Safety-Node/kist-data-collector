#include "system/lowstate_recorder.hpp"

#include <unitree/robot/channel/channel_factory.hpp>
#include <unitree/robot/channel/channel_subscriber.hpp>

#include <cinttypes>
#include <cstdio>
#include <filesystem>
#include <iostream>

namespace kist {

namespace {

constexpr int kNumMotors = 35;   // std::array size in unitree_hg LowState_

std::string lowstate_header() {
    std::string h =
        "recv_ns,tick,mode_machine,mode_pr,"
        "quat_w,quat_x,quat_y,quat_z,"
        "gyro_x,gyro_y,gyro_z,accel_x,accel_y,accel_z,"
        "rpy_roll,rpy_pitch,rpy_yaw,imu_temp";
    for (int i = 0; i < kNumMotors; ++i) {
        char b[64];
        std::snprintf(b, sizeof(b), ",m%02d_q,m%02d_dq,m%02d_ddq,m%02d_tau", i, i, i, i);
        h += b;
    }
    return h;
}

std::string lowstate_row(const unitree_hg::msg::dds_::LowState_& s, int64_t recv_ns) {
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

} // namespace

LowstateRecorder::LowstateRecorder() = default;
LowstateRecorder::~LowstateRecorder() { stop(); }

bool LowstateRecorder::start(int domain_id, const std::string& network_interface,
                            const std::string& session_dir, size_t queue_capacity,
                            const std::string& topic) {
    if (running_) return true;

    const auto csv_path = std::filesystem::path(session_dir) / "lowstate.csv";
    if (!rec_.open(csv_path.string(), lowstate_header(), &lowstate_row, queue_capacity)) {
        std::cerr << "[LowstateRecorder] cannot open " << csv_path << "\n";
        return false;
    }

    try {
        // Safe when the embedding process already initialized the factory.
        unitree::robot::ChannelFactory::Instance()->Init(domain_id, network_interface);
        sub_.reset(new Sub(topic));
        // Reader history deeper than the cameras' (16): at ~1 kHz a brief
        // callback stall would overrun a shallow history before we notice.
        sub_->InitChannel([this](const void* msg) { on_lowstate(msg); }, 16);
    } catch (const std::exception& e) {
        std::cerr << "[LowstateRecorder] DDS init failed on interface \""
                  << network_interface << "\": " << e.what() << "\n";
        rec_.close();
        return false;
    }

    std::cout << "[LowstateRecorder] started on domain=" << domain_id
              << " interface=" << network_interface << " topic=" << topic << "\n";
    running_ = true;
    return true;
}

void LowstateRecorder::stop() {
    sub_.reset();
    rec_.close();
    running_ = false;
}

void LowstateRecorder::on_lowstate(const void* message) {
    rec_.push(*static_cast<const unitree_hg::msg::dds_::LowState_*>(message));
}

} // namespace kist
