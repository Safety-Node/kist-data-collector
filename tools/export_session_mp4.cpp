// Converts a whole session to mp4, all camera streams concurrently.
// Scans <session_dir> for camera dirs (anything holding a color or depth
// index) and starts one ExportColorToMp4 + one ExportDepthToMp4 per camera —
// each is its own worker thread, so 3 cameras convert on 6 threads at once.
// Outputs land next to their sources: <camera>/color.mp4, <camera>/depth.mp4.
//
//   ./export_session_mp4 <session_dir>

#include "export/export_color_to_mp4.hpp"
#include "export/export_depth_to_mp4.hpp"

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
        std::unique_ptr<kist::ExportColorToMp4> color;
        std::unique_ptr<kist::ExportDepthToMp4> depth;
    };
    std::vector<Job> jobs;

    // A camera dir is any session subdir carrying a stream index.
    for (const auto& entry : std::filesystem::directory_iterator(session)) {
        if (!entry.is_directory()) continue;
        const auto dir = entry.path();
        Job job;
        job.camera = dir.filename().string();
        job.color  = std::make_unique<kist::ExportColorToMp4>();
        job.depth  = std::make_unique<kist::ExportDepthToMp4>();
        const bool c = job.color->start(dir.string());
        const bool d = job.depth->start(dir.string());
        if (!c) job.color.reset();
        if (!d) job.depth.reset();
        if (job.color || job.depth) jobs.push_back(std::move(job));
    }
    if (jobs.empty()) {
        std::fprintf(stderr, "no camera streams under %s\n", session.c_str());
        return 1;
    }

    size_t threads = 0;
    for (const auto& j : jobs) threads += (j.color ? 1 : 0) + (j.depth ? 1 : 0);
    std::printf("[export_session_mp4] %zu camera(s), %zu conversion thread(s) running...\n",
                jobs.size(), threads);

    const auto t0 = std::chrono::steady_clock::now();
    bool all_ok = true;
    for (auto& j : jobs) {
        if (j.color) { j.color->wait(); all_ok &= j.color->succeeded(); }
        if (j.depth) { j.depth->wait(); all_ok &= j.depth->succeeded(); }
    }
    const double secs = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - t0).count();

    std::printf("[export_session_mp4] %s in %.1f s\n",
                all_ok ? "all conversions done" : "finished WITH FAILURES (see above)",
                secs);
    return all_ok ? 0 : 1;
}
