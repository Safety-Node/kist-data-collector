#include "system/dex3_cmd_recorder.hpp"

#include "unitree_recorder/dex3_cmd_rows.hpp"

#include <unitree/robot/channel/channel_factory.hpp>
#include <unitree/robot/channel/channel_subscriber.hpp>

#include <cstdio>
#include <filesystem>
#include <iostream>

namespace kist {

Dex3CmdRecorder::Dex3CmdRecorder() = default;
Dex3CmdRecorder::~Dex3CmdRecorder() { stop(); }

bool Dex3CmdRecorder::start(int domain_id, const std::string& network_interface,
                            const std::string& session_dir, const std::string& side,
                            size_t queue_capacity) {
    if (running_) return true;
    side_ = side;

    const auto csv_path = std::filesystem::path(session_dir) / ("hand_cmd_" + side + ".csv");
    if (!rec_.open(csv_path.string(), dex3_cmd_header(), &dex3_cmd_row, queue_capacity)) {
        std::cerr << "[Dex3CmdRecorder] cannot open " << csv_path << "\n";
        return false;
    }

    try {
        // Safe when the embedding process already initialized the factory.
        unitree::robot::ChannelFactory::Instance()->Init(domain_id, network_interface);
        sub_.reset(new Sub(dex3_cmd_topic(side)));
        sub_->InitChannel([this](const void* msg) { on_hand_cmd(msg); }, 16);
    } catch (const std::exception& e) {
        std::cerr << "[Dex3CmdRecorder] DDS init failed on interface \""
                  << network_interface << "\": " << e.what() << "\n";
        rec_.close();
        return false;
    }

    std::cout << "[Dex3CmdRecorder] started on domain=" << domain_id
              << " interface=" << network_interface
              << " topic=" << dex3_cmd_topic(side) << "\n";
    running_ = true;
    return true;
}

void Dex3CmdRecorder::stop() {
    sub_.reset();
    rec_.close();
    running_ = false;
}

void Dex3CmdRecorder::on_hand_cmd(const void* message) {
    const auto& c = *static_cast<const unitree_hg::msg::dds_::HandCmd_*>(message);
    // One-time report of the actual vector size, so a mismatch with the
    // fixed CSV schema (kDex3CmdMotors) is visible in the field.
    if (!sizes_logged_) {
        sizes_logged_ = true;
        std::printf("[Dex3CmdRecorder] %s: %zu motor cmd(s) (csv schema: %d motors)\n",
                    side_.c_str(), c.motor_cmd().size(), kDex3CmdMotors);
    }
    rec_.push(c);
}

} // namespace kist
