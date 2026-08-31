#include "export/export_depth_to_images.hpp"

#include "realsense/receiver/depth_decoder.hpp"
#include "realsense/rvl_depth_frame.hpp"

#include <opencv2/imgcodecs.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace kist {

namespace {

struct Row {
    uint64_t seq    = 0;
    uint64_t offset = 0;
    uint64_t size   = 0;
    int      width  = 0;
    int      height = 0;
    float    scale  = 0.001f;
};

// depth.idx.csv: seq,stamp_ns,recv_ns,width,height,depth_scale[,fx,fy,cx,cy],offset,size
// The four intrinsics columns were added later; parse both layouts so older
// recordings still export (the exporter only needs offset/size/w/h/scale).
std::vector<Row> read_index(const std::filesystem::path& path) {
    std::vector<Row> rows;
    std::ifstream idx(path);
    std::string line;
    std::getline(idx, line);  // header
    while (std::getline(idx, line)) {
        unsigned long long seq, offset, size;
        long long stamp_ns, recv_ns;
        int w, h;
        float scale, fx, fy, cx, cy;
        if (std::sscanf(line.c_str(), "%llu,%lld,%lld,%d,%d,%f,%f,%f,%f,%f,%llu,%llu",
                        &seq, &stamp_ns, &recv_ns, &w, &h, &scale,
                        &fx, &fy, &cx, &cy, &offset, &size) == 12) {
            rows.push_back({seq, offset, size, w, h, scale});
        } else if (std::sscanf(line.c_str(), "%llu,%lld,%lld,%d,%d,%f,%llu,%llu",
                        &seq, &stamp_ns, &recv_ns, &w, &h, &scale, &offset, &size) == 8) {
            rows.push_back({seq, offset, size, w, h, scale});
        }
    }
    return rows;
}

} // namespace

bool ExportDepthToImages::start(const std::string& camera_dir) {
    if (running_.load()) return false;
    const std::filesystem::path dir = camera_dir;
    if (!std::filesystem::exists(dir / "depth.rvl") ||
        !std::filesystem::exists(dir / "depth.idx.csv"))
        return false;
    cancel_  = false;
    ok_      = false;
    running_ = true;
    thread_  = std::thread(&ExportDepthToImages::run, this, camera_dir);
    return true;
}

void ExportDepthToImages::stop() {
    cancel_ = true;
    wait();
}

void ExportDepthToImages::wait() {
    if (thread_.joinable()) thread_.join();
}

void ExportDepthToImages::run(const std::string& camera_dir) {
    const std::filesystem::path cam_dir = camera_dir;
    const std::filesystem::path out_dir = cam_dir / "depth_png";

    const auto rows = read_index(cam_dir / "depth.idx.csv");
    std::ifstream rvl(cam_dir / "depth.rvl", std::ios::binary);
    if (rows.empty() || !rvl.is_open()) {
        std::fprintf(stderr, "[export depth_png] %s: no frames\n", camera_dir.c_str());
        running_ = false;
        return;
    }
    std::error_code ec;
    std::filesystem::create_directories(out_dir, ec);

    RvlDepthDecoder decoder;
    size_t written = 0;
    bool   io_ok   = true;
    char   name[32];
    for (const auto& row : rows) {
        if (cancel_.load(std::memory_order_relaxed)) break;
        RvlDepthFrame f;
        f.width       = row.width;
        f.height      = row.height;
        f.depth_scale = row.scale;
        f.data.resize(row.size);
        rvl.seekg(std::streamoff(row.offset));
        rvl.read(reinterpret_cast<char*>(f.data.data()), std::streamsize(row.size));
        if (!rvl.good()) { io_ok = false; break; }

        const DepthFrame d = decoder.decode(f);
        const cv::Mat z16(d.height, d.width, CV_16UC1,
                          const_cast<uint8_t*>(d.data.data()), size_t(d.stride_bytes));
        std::snprintf(name, sizeof(name), "%010llu.png", (unsigned long long)row.seq);
        if (!cv::imwrite((out_dir / name).string(), z16)) { io_ok = false; break; }
        written++;
    }

    const bool complete = io_ok && !cancel_.load() && written == rows.size();
    if (complete)
        std::printf("[export depth_png] %s: %zu frames (16-bit, x depth_scale = m)\n",
                    out_dir.c_str(), written);
    else
        std::fprintf(stderr, "[export depth_png] %s: incomplete (%zu/%zu%s)\n",
                     out_dir.c_str(), written, rows.size(),
                     cancel_.load() ? ", cancelled" : "");
    ok_      = complete;
    running_ = false;
}

} // namespace kist
