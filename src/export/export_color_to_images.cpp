#include "export/export_color_to_images.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
}

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
    int      is_key = 0;
};

// color.idx.csv: seq,stamp_ns,recv_ns,width,height,is_keyframe,offset,size
std::vector<Row> read_index(const std::filesystem::path& path, size_t& first_key) {
    std::vector<Row> rows;
    std::ifstream idx(path);
    std::string line;
    std::getline(idx, line);  // header
    first_key = SIZE_MAX;
    while (std::getline(idx, line)) {
        unsigned long long seq, offset, size;
        long long stamp_ns, recv_ns;
        int w, h, key;
        if (std::sscanf(line.c_str(), "%llu,%lld,%lld,%d,%d,%d,%llu,%llu",
                        &seq, &stamp_ns, &recv_ns, &w, &h, &key, &offset, &size) != 8)
            continue;
        if (key && first_key == SIZE_MAX) first_key = rows.size();
        rows.push_back({seq, offset, size, key});
    }
    return rows;
}

std::string seq_name(uint64_t seq, const char* ext) {
    char b[32];
    std::snprintf(b, sizeof(b), "%010llu.%s", (unsigned long long)seq, ext);
    return b;
}

} // namespace

bool ExportColorToImages::start(const std::string& camera_dir, int jpeg_quality) {
    if (running_.load()) return false;
    const std::filesystem::path dir = camera_dir;
    if (!std::filesystem::exists(dir / "color.h264") ||
        !std::filesystem::exists(dir / "color.idx.csv"))
        return false;
    cancel_  = false;
    ok_      = false;
    running_ = true;
    thread_  = std::thread(&ExportColorToImages::run, this, camera_dir, jpeg_quality);
    return true;
}

void ExportColorToImages::stop() {
    cancel_ = true;
    wait();
}

void ExportColorToImages::wait() {
    if (thread_.joinable()) thread_.join();
}

void ExportColorToImages::run(const std::string& camera_dir, int jpeg_quality) {
    const std::filesystem::path cam_dir = camera_dir;
    const std::filesystem::path out_dir = cam_dir / "color_jpg";

    size_t k = SIZE_MAX;
    const auto rows = read_index(cam_dir / "color.idx.csv", k);
    std::ifstream blob(cam_dir / "color.h264", std::ios::binary);
    if (rows.empty() || k == SIZE_MAX || !blob.is_open()) {
        std::fprintf(stderr, "[export color_jpg] %s: no decodable frames\n", camera_dir.c_str());
        running_ = false;
        return;
    }
    std::error_code ec;
    std::filesystem::create_directories(out_dir, ec);

    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    AVCodecContext* ctx  = avcodec_alloc_context3(codec);
    if (avcodec_open2(ctx, codec, nullptr) < 0) {
        std::fprintf(stderr, "[export color_jpg] cannot open H.264 decoder\n");
        running_ = false;
        return;
    }

    // Each index row is one access unit. Its wire seq rides the packet's
    // pts, and the decoder stamps that pts onto the frame it produces —
    // exact packet<->frame pairing even when the decoder drops undecodable
    // frames (the ones right after a wire gap, whose reference was lost).
    const std::vector<int> jpg_params = {cv::IMWRITE_JPEG_QUALITY, jpeg_quality};
    SwsContext* sws = nullptr;
    AVFrame*  frame = av_frame_alloc();
    cv::Mat   bgr;
    size_t written = 0;
    bool    io_ok  = true;

    auto drain = [&]() {
        while (avcodec_receive_frame(ctx, frame) == 0) {
            if (frame->pts == AV_NOPTS_VALUE) continue;  // no identity, skip
            if (!sws) {
                sws = sws_getContext(frame->width, frame->height,
                                     AVPixelFormat(frame->format),
                                     frame->width, frame->height, AV_PIX_FMT_BGR24,
                                     SWS_BILINEAR, nullptr, nullptr, nullptr);
                bgr.create(frame->height, frame->width, CV_8UC3);
            }
            uint8_t* dst[1] = {bgr.data};
            int  dst_stride[1] = {int(bgr.step)};
            sws_scale(sws, frame->data, frame->linesize, 0, frame->height,
                      dst, dst_stride);
            const auto name = seq_name(uint64_t(frame->pts), "jpg");
            if (!cv::imwrite((out_dir / name).string(), bgr, jpg_params))
                io_ok = false;
            written++;
        }
    };

    AVPacket* pkt = av_packet_alloc();
    for (size_t r = k; r < rows.size() && !cancel_.load(std::memory_order_relaxed); ++r) {
        av_new_packet(pkt, int(rows[r].size));
        blob.seekg(std::streamoff(rows[r].offset));
        blob.read(reinterpret_cast<char*>(pkt->data), std::streamsize(rows[r].size));
        if (!blob.good()) { io_ok = false; av_packet_unref(pkt); break; }
        pkt->pts = int64_t(rows[r].seq);
        if (avcodec_send_packet(ctx, pkt) == 0) drain();
        av_packet_unref(pkt);
    }
    avcodec_send_packet(ctx, nullptr);  // flush the decoder pipeline
    drain();

    av_packet_free(&pkt);
    av_frame_free(&frame);
    if (sws) sws_freeContext(sws);
    avcodec_free_context(&ctx);

    // The decoder legitimately drops frames whose reference was lost on the
    // wire (right after a gap) — ffmpeg decodes the same count. That's not
    // an export failure; report it so the deficit is visible.
    const size_t expected = rows.size() - k;
    const bool complete = io_ok && !cancel_.load();
    if (complete) {
        std::printf("[export color_jpg] %s: %zu frames (q%d, skipped %zu pre-keyframe",
                    out_dir.c_str(), written, jpeg_quality, k);
        if (written < expected)
            std::printf(", %zu post-gap undecodable", expected - written);
        std::printf(")\n");
    } else {
        std::fprintf(stderr, "[export color_jpg] %s: incomplete (%zu/%zu%s)\n",
                     out_dir.c_str(), written, expected,
                     cancel_.load() ? ", cancelled" : "");
    }
    ok_      = complete;
    running_ = false;
}

} // namespace kist
