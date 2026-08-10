#include "system/lowcmd_recorder.hpp"

#include "unitree_recorder/lowcmd_rows.hpp"

#include <unitree/robot/channel/channel_factory.hpp>
#include <unitree/robot/channel/channel_subscriber.hpp>

#include <filesystem>
#include <iostream>

namespace kist {

LowcmdRecorder::LowcmdRecorder() = default;
LowcmdRecorder::~LowcmdRecorder() { stop(); }

bool LowcmdRecorder::start(int domain_id, const std::string& network_interface,
                           const std::string& session_dir, size_t queue_capacity,
                           const std::string& topic, const std::string& csv_name) {
    if (running_) return true;
    label_ = std::filesystem::path(csv_name).stem().string();

    const auto csv_path = std::filesystem::path(session_dir) / csv_name;
    if (!rec_.open(csv_path.string(), lowcmd_header(), &lowcmd_row, queue_capacity)) {
        std::cerr << "[LowcmdRecorder] cannot open " << csv_path << "\n";
        return false;
    }

    try {
        // Safe when the embedding process already initialized the factory.
        unitree::robot::ChannelFactory::Instance()->Init(domain_id, network_interface);
        sub_.reset(new Sub(topic));
        // Same reader history as lowstate: commands stream at up to ~1 kHz.
        sub_->InitChannel([this](const void* msg) { on_lowcmd(msg); }, 16);
    } catch (const std::exception& e) {
        std::cerr << "[LowcmdRecorder] DDS init failed on interface \""
                  << network_interface << "\": " << e.what() << "\n";
        rec_.close();
        return false;
    }

    std::cout << "[LowcmdRecorder] started on domain=" << domain_id
              << " interface=" << network_interface << " topic=" << topic << "\n";
    running_ = true;
    return true;
}

void LowcmdRecorder::stop() {
    sub_.reset();
    rec_.close();
    running_ = false;
}

void LowcmdRecorder::on_lowcmd(const void* message) {
    rec_.push(*static_cast<const unitree_hg::msg::dds_::LowCmd_*>(message));
}

} // namespace kist
