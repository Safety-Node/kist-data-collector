#include "unitree_recorder/dex3_recorder.hpp"

#include <unitree/robot/channel/channel_factory.hpp>
#include <unitree/robot/channel/channel_subscriber.hpp>

#include <cinttypes>
#include <cstdio>
#include <filesystem>
#include <iostream>

namespace kist {

namespace {

// Fixed CSV width from the Dex3-1 hardware: 7 finger motors, and press-
// sensor pads of 12 pressure channels each (kPressPads bounds the flattened
// columns; shorter/absent pads are zero-filled, extras are dropped — the
// message carries them as vectors, but a CSV schema must be fixed).
constexpr int kMotors    = 7;
constexpr int kPressPads = 3;
constexpr int kPressChan = 12;

std::string dex3_header() {
    std::string h = "recv_ns";
    char b[64];
    for (int i = 0; i < kMotors; ++i) {
        std::snprintf(b, sizeof(b), ",f%d_q,f%d_dq,f%d_ddq,f%d_tau", i, i, i, i);
        h += b;
    }
    for (int p = 0; p < kPressPads; ++p)
        for (int c = 0; c < kPressChan; ++c) {
            std::snprintf(b, sizeof(b), ",press%d_%d", p, c);
            h += b;
        }
    return h;
}

std::string dex3_row(const unitree_hg::msg::dds_::HandState_& s, int64_t recv_ns) {
    std::string row;
    row.reserve(1024);
    char b[96];

    std::snprintf(b, sizeof(b), "%" PRId64, recv_ns);
    row += b;

    const auto& motors = s.motor_state();
    for (int i = 0; i < kMotors; ++i) {
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
    for (int p = 0; p < kPressPads; ++p)
        for (int c = 0; c < kPressChan; ++c) {
            const float v = (size_t(p) < pads.size()) ? pads[size_t(p)].pressure()[size_t(c)] : 0.f;
            std::snprintf(b, sizeof(b), ",%.7g", double(v));
            row += b;
        }
    return row;
}

} // namespace

Dex3Recorder::Dex3Recorder() = default;
Dex3Recorder::~Dex3Recorder() { stop(); }

bool Dex3Recorder::start(int domain_id, const std::string& network_interface,
                         const std::string& session_dir, const std::string& side,
                         size_t queue_capacity) {
    if (running_) return true;
    side_ = side;

    const auto csv_path = std::filesystem::path(session_dir) / ("hand_" + side + ".csv");
    if (!rec_.open(csv_path.string(), dex3_header(), &dex3_row, queue_capacity)) {
        std::cerr << "[Dex3Recorder] cannot open " << csv_path << "\n";
        return false;
    }

    try {
        // Safe when the embedding process already initialized the factory.
        unitree::robot::ChannelFactory::Instance()->Init(domain_id, network_interface);
        sub_.reset(new Sub(dex3_state_topic(side)));
        sub_->InitChannel([this](const void* msg) { on_hand_state(msg); }, 16);
    } catch (const std::exception& e) {
        std::cerr << "[Dex3Recorder] DDS init failed on interface \""
                  << network_interface << "\": " << e.what() << "\n";
        rec_.close();
        return false;
    }

    std::cout << "[Dex3Recorder] started on domain=" << domain_id
              << " interface=" << network_interface
              << " topic=" << dex3_state_topic(side) << "\n";
    running_ = true;
    return true;
}

void Dex3Recorder::stop() {
    sub_.reset();
    rec_.close();
    running_ = false;
}

void Dex3Recorder::on_hand_state(const void* message) {
    const auto& s = *static_cast<const unitree_hg::msg::dds_::HandState_*>(message);
    // One-time report of the actual vector sizes, so a mismatch with the
    // fixed CSV schema (kMotors/kPressPads) is visible in the field.
    if (!sizes_logged_) {
        sizes_logged_ = true;
        std::printf("[Dex3Recorder] %s: %zu motor(s), %zu press pad(s) "
                    "(csv schema: %d motors, %d pads)\n",
                    side_.c_str(), s.motor_state().size(),
                    s.press_sensor_state().size(), kMotors, kPressPads);
    }
    rec_.push(s);
}

} // namespace kist
