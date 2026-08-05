#include "common/session.hpp"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace kist {

namespace {

int64_t now_epoch_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string format_time(int64_t epoch_ns, const char* fmt, bool utc) {
    const std::time_t t = std::time_t(epoch_ns / 1000000000);
    std::tm tm{};
    if (utc) gmtime_r(&t, &tm); else localtime_r(&t, &tm);
    char buf[64];
    std::strftime(buf, sizeof(buf), fmt, &tm);
    return buf;
}

} // namespace

SessionInfo session_create(const std::string& output_dir) {
    SessionInfo s;
    s.started_ns = now_epoch_ns();
    const std::filesystem::path dir =
        std::filesystem::path(output_dir) / format_time(s.started_ns, "%Y%m%d_%H%M%S", false);
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        std::cerr << "[session] cannot create " << dir << ": " << ec.message() << "\n";
        return s;
    }
    s.dir = dir.string();
    return s;
}

void session_write_meta(const SessionInfo& session, int domain_id,
                        const std::string& dds_config,
                        const std::vector<std::string>& cameras) {
    std::ofstream f(std::filesystem::path(session.dir) / "meta.yaml", std::ios::trunc);
    f << "session:\n"
      << "  started_utc: " << format_time(session.started_ns, "%Y-%m-%dT%H:%M:%SZ", true) << "\n"
      << "  started_epoch_ns: " << session.started_ns << "\n"
      << "  domain_id: " << domain_id << "\n"
      << "  dds_config: " << dds_config << "\n"
      << "cameras:\n";
    for (const auto& name : cameras)
        f << "  - " << name << "\n";
    f << "format:\n"
      << "  # per camera: <name>/{color.h264,color.idx.csv,depth.rvl,depth.idx.csv}\n"
      << "  color: Annex-B H.264 NAL units, frames appended verbatim; idx csv maps\n"
      << "         seq/stamp_ns/recv_ns -> byte (offset,size); is_keyframe marks IDR.\n"
      << "  depth: RVL bitstreams (lossless Z16, kist-ext-sensor-io codec) appended\n"
      << "         verbatim; idx csv adds width/height/depth_scale per frame.\n"
      << "  lowstate.csv: one row per rt/lowstate msg (~1 kHz) — recv_ns, tick\n"
      << "         (robot ms counter), mode_machine/mode_pr, IMU (quaternion wxyz,\n"
      << "         gyro, accel, rpy, temp), body motors m00..m34 x q/dq/ddq/tau_est.\n"
      << "  hand_{left,right}.csv: one row per rt/dex3/<side>/state msg — recv_ns,\n"
      << "         finger motors f0..f6 x q/dq/ddq/tau_est (f0 = thumb rotation),\n"
      << "         press pads press0..8 x 12 pressure channels.\n"
      << "  uwb.csv: one row per rt/kist/uwb/pose fix — recv_ns, stamp_ns, x, y, z\n"
      << "         (UWB local frame, m); time gaps = no fix, not loss.\n"
      << "  clocks: stamp_ns = transmitter capture clock (epoch ns);\n"
      << "          recv_ns = this host's arrival clock (epoch ns) — the\n"
      << "          cross-stream alignment column, shared by every file above.\n";
}

void session_finalize_meta(const SessionInfo& session,
                           const std::vector<StreamSummary>& streams) {
    std::ofstream f(std::filesystem::path(session.dir) / "meta.yaml", std::ios::app);
    const int64_t ended_ns = now_epoch_ns();
    f << "ended_utc: " << format_time(ended_ns, "%Y-%m-%dT%H:%M:%SZ", true) << "\n"
      << "ended_epoch_ns: " << ended_ns << "\n"
      << "streams:\n";
    for (const auto& s : streams) {
        f << "  - source: "   << s.source          << "\n"
          << "    stream: "   << s.stream          << "\n"
          << "    received: " << s.stats.received  << "\n"
          << "    written: "  << s.stats.written   << "\n"
          << "    dropped: "  << s.stats.dropped   << "\n"
          << "    write_errors: " << s.stats.write_errors << "\n"
          << "    wire_gaps: "<< s.stats.wire_gaps << "\n"
          << "    bytes: "    << s.stats.bytes     << "\n";
    }
}

} // namespace kist
