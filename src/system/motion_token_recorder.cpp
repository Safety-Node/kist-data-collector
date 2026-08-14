#include "system/motion_token_recorder.hpp"

#include "gearsonic_recorder/motion_token_rows.hpp"

#include <unitree/robot/channel/channel_factory.hpp>
#include <unitree/robot/channel/channel_subscriber.hpp>

#include <filesystem>
#include <iostream>

namespace kist {

MotionTokenRecorder::MotionTokenRecorder() = default;
MotionTokenRecorder::~MotionTokenRecorder() { stop(); }

bool MotionTokenRecorder::start(int domain_id, const std::string& network_interface,
                                const std::string& session_dir,
                                size_t queue_capacity) {
    if (running_) return true;

    const auto csv_path = std::filesystem::path(session_dir) / "motion_token.csv";
    if (!rec_.open(csv_path.string(), motion_token_header(), &motion_token_row,
                   queue_capacity)) {
        std::cerr << "[MotionTokenRecorder] cannot open " << csv_path << "\n";
        return false;
    }

    try {
        // Safe when the embedding process already initialized the factory.
        unitree::robot::ChannelFactory::Instance()->Init(domain_id, network_interface);
        sub_.reset(new Sub(kMotionTokenTopic));
        sub_->InitChannel([this](const void* msg) { on_token(msg); }, 16);
    } catch (const std::exception& e) {
        std::cerr << "[MotionTokenRecorder] DDS init failed on interface \""
                  << network_interface << "\": " << e.what() << "\n";
        rec_.close();
        return false;
    }

    std::cout << "[MotionTokenRecorder] started on domain=" << domain_id
              << " interface=" << network_interface
              << " topic=" << kMotionTokenTopic << "\n";
    running_ = true;
    return true;
}

void MotionTokenRecorder::stop() {
    sub_.reset();
    rec_.close();
    running_ = false;
}

void MotionTokenRecorder::on_token(const void* message) {
    rec_.push(*static_cast<const kist_msgs::MotionTokenState*>(message));
}

} // namespace kist
