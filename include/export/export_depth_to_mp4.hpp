#pragma once

// depth.rvl + depth.idx.csv -> depth.mp4 (viewable preview video) as a
// start/stop worker, the depth twin of ExportColorToMp4. Unlike color
// (remux, no re-encode), depth must be synthesized: each RVL bitstream is
// decoded to Z16, colorized (JET, 0.3..4 m, invalid = black), and encoded
// at the stream's measured average fps. The 16-bit exact data stays in
// depth.rvl — this output is for eyes, not for math (use export_depth_png
// or the RVL decoder for analysis).

#include <atomic>
#include <string>
#include <thread>

namespace kist {

class ExportDepthToMp4 {
public:
    ~ExportDepthToMp4() { stop(); }

    // <camera_dir>/depth.{rvl,idx.csv} -> <camera_dir>/depth.mp4.
    // False if already running or the inputs are missing.
    bool start(const std::string& camera_dir);

    // Cancel (if still running) and join. A cancelled run leaves a partial
    // .mp4 and reports succeeded() == false.
    void stop();

    // Join, letting the conversion finish.
    void wait();

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
