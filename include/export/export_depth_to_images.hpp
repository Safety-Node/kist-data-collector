#pragma once

// depth.rvl + depth.idx.csv -> per-frame 16-bit PNGs (the exact-data form)
// as a start/stop worker. Every RVL slice decodes to Z16 and lands as
// depth_png/<seq>.png — 16-bit grayscale PNG, lossless, pixel * depth_scale
// (from the index) = meters. PNG is mandatory here: JPEG is 8-bit and lossy,
// which would destroy the depth values; the colorized look-only form is
// ExportDepthToMp4's job.

#include <atomic>
#include <string>
#include <thread>

namespace kist {

class ExportDepthToImages {
public:
    ~ExportDepthToImages() { stop(); }

    // <camera_dir>/depth.{rvl,idx.csv} -> <camera_dir>/depth_png/<seq>.png
    bool start(const std::string& camera_dir);

    void stop();   // cancel (if running) and join; partial output remains
    void wait();   // join, letting the conversion finish

    bool running() const { return running_.load(); }
    bool succeeded() const { return ok_.load(); }

private:
    void run(const std::string& camera_dir);

    std::thread       thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> cancel_{false};
    std::atomic<bool> ok_{false};
};

} // namespace kist
