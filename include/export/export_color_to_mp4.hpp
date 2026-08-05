#pragma once

// color.h264 + color.idx.csv -> color.mp4 (no re-encode, libavformat remux)
// as a start/stop worker: start() spawns the conversion on its own thread
// and returns immediately; the owner then either wait()s for completion or
// stop()s to cancel mid-run. One instance per stream — a session's cameras
// convert concurrently (tools/export_session_mp4 runs 2 x N of these).
//
// Output details: cut at the first keyframe (leading pre-IDR frames are
// undecodable), every frame stamped with its true arrival time (recv_ns)
// on a 90 kHz track — playback runs at the real captured cadence.

#include <atomic>
#include <string>
#include <thread>

namespace kist {

class ExportColorToMp4 {
public:
    ~ExportColorToMp4() { stop(); }

    // <camera_dir>/color.{h264,idx.csv} -> <camera_dir>/color.mp4.
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
