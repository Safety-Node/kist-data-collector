#pragma once

// color.h264 + color.idx.csv -> per-frame JPEGs (CV-training form) as a
// start/stop worker. Each index row's (offset,size) slice is fed to the
// H.264 decoder as one access unit, so every output frame is named by its
// wire `seq` — the same seq space depth shares, making cross-modal pairing
// a filename match: color_jpg/<seq>.jpg <-> depth_png/<seq>.png.
//
// JPEG (quality 95) over PNG on purpose: the source is already H.264-lossy,
// so PNG would only preserve codec artifacts losslessly at 4-5x the bytes;
// q95 JPEG is visually transparent and the standard training-set form.
// Frames before the first keyframe are undecodable and skipped (they remain
// in color.h264).

#include <atomic>
#include <string>
#include <thread>

namespace kist {

class ExportColorToImages {
public:
    ~ExportColorToImages() { stop(); }

    // <camera_dir>/color.{h264,idx.csv} -> <camera_dir>/color_jpg/<seq>.jpg
    bool start(const std::string& camera_dir, int jpeg_quality = 95);

    void stop();   // cancel (if running) and join; partial output remains
    void wait();   // join, letting the conversion finish

    bool running() const { return running_.load(); }
    bool succeeded() const { return ok_.load(); }

private:
    void run(const std::string& camera_dir, int jpeg_quality);

    std::thread       thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> cancel_{false};
    std::atomic<bool> ok_{false};
};

} // namespace kist
