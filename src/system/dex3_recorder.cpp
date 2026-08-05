#include "system/dex3_recorder.hpp"

#include "unitree_recorder/dex3_rows.hpp"

#include <unitree/robot/channel/channel_factory.hpp>
#include <unitree/robot/channel/channel_subscriber.hpp>

#include <cstdio>
#include <filesystem>
#include <iostream>

namespace kist {

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
                    s.press_sensor_state().size(), kDex3Motors, kDex3PressPads);
    }
    rec_.push(s);
}

} // namespace kist
