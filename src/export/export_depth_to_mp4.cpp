#include "export/export_depth_to_mp4.hpp"

#include "realsense/receiver/depth_decoder.hpp"
#include "realsense/rvl_depth_frame.hpp"

#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace kist {

namespace {

struct Row {
    int64_t  recv_ns = 0;
    uint64_t offset  = 0;
    uint64_t size    = 0;
    int      width   = 0;
    int      height  = 0;
    float    scale   = 0.001f;
};

// depth.idx.csv: seq,stamp_ns,recv_ns,width,height,depth_scale,offset,size
std::vector<Row> read_index(const std::filesystem::path& path) {
    std::vector<Row> rows;
    std::ifstream idx(path);
    std::string line;
    std::getline(idx, line);  // header
    while (std::getline(idx, line)) {
        unsigned long long seq, offset, size;
        long long stamp_ns, recv_ns;
        int w, h;
        float scale;
        if (std::sscanf(line.c_str(), "%llu,%lld,%lld,%d,%d,%f,%llu,%llu",
                        &seq, &stamp_ns, &recv_ns, &w, &h, &scale, &offset, &size) != 8)
            continue;
        rows.push_back({recv_ns, offset, size, w, h, scale});
    }
    return rows;
}

// Same look as export_depth_png --preview: meters clamped 0.3..4 m through
// JET; invalid (0) pixels stay black.
cv::Mat colorize(const cv::Mat& z16, float depth_scale) {
    cv::Mat meters, u8, color;
    z16.convertTo(meters, CV_32F, double(depth_scale));
    cv::Mat norm = (meters - 0.3f) / (4.0f - 0.3f) * 255.0f;
    norm.setTo(0, meters == 0.0f);
    norm.convertTo(u8, CV_8U);
    cv::applyColorMap(u8, color, cv::COLORMAP_JET);
    color.setTo(cv::Scalar(0, 0, 0), z16 == 0);
    return color;
}

} // namespace

bool ExportDepthToMp4::start(const std::string& camera_dir) {
    if (running_.load()) return false;
    const std::filesystem::path dir = camera_dir;
    if (!std::filesystem::exists(dir / "depth.rvl") ||
        !std::filesystem::exists(dir / "depth.idx.csv"))
        return false;
    cancel_  = false;
    ok_      = false;
    running_ = true;
    thread_  = std::thread(&ExportDepthToMp4::run, this, camera_dir);
    return true;
}

void ExportDepthToMp4::stop() {
    cancel_ = true;
    wait();
}

void ExportDepthToMp4::wait() {
    if (thread_.joinable()) thread_.join();
}

void ExportDepthToMp4::run(const std::string& camera_dir) {
    const std::filesystem::path cam_dir = camera_dir;
    const std::string out_path = (cam_dir / "depth.mp4").string();

    const auto rows = read_index(cam_dir / "depth.idx.csv");
    std::ifstream rvl(cam_dir / "depth.rvl", std::ios::binary);
    if (rows.size() < 2 || !rvl.is_open()) {
        std::fprintf(stderr, "[export depth] %s: no frames\n", camera_dir.c_str());
        running_ = false;
        return;
    }

    // VideoWriter is CFR-only, so encode at the measured average rate (the
    // exact per-frame times stay in the index; this output is a preview).
    const double span_s = double(rows.back().recv_ns - rows.front().recv_ns) / 1e9;
    const double fps    = double(rows.size() - 1) / (span_s > 0 ? span_s : 1.0);

    cv::VideoWriter writer(out_path, cv::VideoWriter::fourcc('m', 'p', '4', 'v'),
                           fps, cv::Size(rows[0].width, rows[0].height));
    if (!writer.isOpened()) {
        std::fprintf(stderr, "[export depth] cannot write %s\n", out_path.c_str());
        running_ = false;
        return;
    }

    RvlDepthDecoder decoder;
    size_t written = 0;
    for (const auto& row : rows) {
        if (cancel_.load(std::memory_order_relaxed)) break;
        RvlDepthFrame f;
        f.width       = row.width;
        f.height      = row.height;
        f.depth_scale = row.scale;
        f.data.resize(row.size);
        rvl.seekg(std::streamoff(row.offset));
        rvl.read(reinterpret_cast<char*>(f.data.data()), std::streamsize(row.size));
        if (!rvl.good()) break;

        const DepthFrame d = decoder.decode(f);
        const cv::Mat z16(d.height, d.width, CV_16UC1,
                          const_cast<uint8_t*>(d.data.data()), size_t(d.stride_bytes));
        writer.write(colorize(z16, row.scale));
        written++;
    }
    writer.release();

    const bool complete = written == rows.size() && !cancel_.load();
    if (complete)
        std::printf("[export depth] %s: %zu frames @ %.2f fps over %.2f s\n",
                    out_path.c_str(), written, fps, span_s);
    else
        std::fprintf(stderr, "[export depth] %s: incomplete (%zu/%zu frames%s)\n",
                     out_path.c_str(), written, rows.size(),
                     cancel_.load() ? ", cancelled" : "");

    ok_      = complete;
    running_ = false;
}

} // namespace kist
