#include "system/lowstate_recorder.hpp"

#include "unitree_recorder/lowstate_rows.hpp"

#include <unitree/robot/channel/channel_factory.hpp>
#include <unitree/robot/channel/channel_subscriber.hpp>

#include <filesystem>
#include <iostream>

namespace kist {

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
