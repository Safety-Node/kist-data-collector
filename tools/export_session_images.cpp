// Converts a whole session to per-frame training images, all camera streams
// concurrently — the dataset twin of export_session_mp4:
//   color.h264 -> <camera>/color_jpg/<seq>.jpg   (q95; source is H.264-lossy
//                                                  already, PNG would only be bigger)
//   depth.rvl  -> <camera>/depth_png/<seq>.png   (16-bit, lossless; x depth_scale = m)
// Files are named by wire seq, shared between a camera's color and depth —
// pairing across modalities is a filename match; seq -> recv_ns comes from
// the idx CSVs for alignment with lowstate/hand rows.
//
//   ./export_session_images <session_dir>

#include "export/export_color_to_images.hpp"
#include "export/export_depth_to_images.hpp"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <session_dir>\n", argv[0]);
        return 1;
    }
    const std::filesystem::path session = argv[1];
    if (!std::filesystem::is_directory(session)) {
        std::fprintf(stderr, "not a directory: %s\n", session.c_str());
        return 1;
    }

    struct Job {
        std::string camera;
        std::unique_ptr<kist::ExportColorToImages> color;
        std::unique_ptr<kist::ExportDepthToImages> depth;
    };
    std::vector<Job> jobs;

    for (const auto& entry : std::filesystem::directory_iterator(session)) {
        if (!entry.is_directory()) continue;
        const auto dir = entry.path();
        Job job;
        job.camera = dir.filename().string();
        job.color  = std::make_unique<kist::ExportColorToImages>();
        job.depth  = std::make_unique<kist::ExportDepthToImages>();
        if (!job.color->start(dir.string())) job.color.reset();
        if (!job.depth->start(dir.string())) job.depth.reset();
        if (job.color || job.depth) jobs.push_back(std::move(job));
    }
    if (jobs.empty()) {
        std::fprintf(stderr, "no camera streams under %s\n", session.c_str());
        return 1;
    }

    size_t threads = 0;
    for (const auto& j : jobs) threads += (j.color ? 1 : 0) + (j.depth ? 1 : 0);
    std::printf("[export_session_images] %zu camera(s), %zu conversion thread(s) running...\n",
                jobs.size(), threads);

    const auto t0 = std::chrono::steady_clock::now();
    bool all_ok = true;
    for (auto& j : jobs) {
        if (j.color) { j.color->wait(); all_ok &= j.color->succeeded(); }
        if (j.depth) { j.depth->wait(); all_ok &= j.depth->succeeded(); }
    }
    const double secs = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - t0).count();

    std::printf("[export_session_images] %s in %.1f s\n",
                all_ok ? "all conversions done" : "finished WITH FAILURES (see above)",
                secs);
    return all_ok ? 0 : 1;
}
