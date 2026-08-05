#include "export/export_color_to_mp4.hpp"

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/mathematics.h>
}

#include <cinttypes>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace kist {

namespace {

struct Row {
    int64_t recv_ns = 0;
    int     is_key  = 0;
};

// color.idx.csv: seq,stamp_ns,recv_ns,width,height,is_keyframe,offset,size
std::vector<Row> read_index(const std::filesystem::path& path, uint64_t& cut_offset,
                            size_t& first_key) {
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
        if (key && first_key == SIZE_MAX) {
            first_key  = rows.size();
            cut_offset = offset;
        }
        rows.push_back({recv_ns, key});
    }
    return rows;
}

// Non-seekable read source for the demuxer: the blob file, starting at the
// first keyframe's byte offset.
int read_cb(void* opaque, uint8_t* buf, int n) {
    const size_t got = std::fread(buf, 1, size_t(n), static_cast<FILE*>(opaque));
    return got > 0 ? int(got) : AVERROR_EOF;
}

} // namespace

bool ExportColorToMp4::start(const std::string& camera_dir) {
    if (running_.load()) return false;
    const std::filesystem::path dir = camera_dir;
    if (!std::filesystem::exists(dir / "color.h264") ||
        !std::filesystem::exists(dir / "color.idx.csv"))
        return false;
    cancel_  = false;
    ok_      = false;
    running_ = true;
    thread_  = std::thread(&ExportColorToMp4::run, this, camera_dir);
    return true;
}

void ExportColorToMp4::stop() {
    cancel_ = true;
    wait();
}

void ExportColorToMp4::wait() {
    if (thread_.joinable()) thread_.join();
}

void ExportColorToMp4::run(const std::string& camera_dir) {
    const std::filesystem::path cam_dir = camera_dir;
    const std::string out_path = (cam_dir / "color.mp4").string();

    uint64_t cut_offset = 0;
    size_t   k          = SIZE_MAX;
    const auto rows = read_index(cam_dir / "color.idx.csv", cut_offset, k);
    if (rows.size() < 2 || k == SIZE_MAX) {
        std::fprintf(stderr, "[export color] %s: no decodable frames\n", camera_dir.c_str());
        running_ = false;
        return;
    }

    FILE* blob = std::fopen((cam_dir / "color.h264").c_str(), "rb");
    if (!blob || std::fseek(blob, long(cut_offset), SEEK_SET) != 0) {
        std::fprintf(stderr, "[export color] %s: cannot open blob\n", camera_dir.c_str());
        running_ = false;
        return;
    }

    // ── demux the raw Annex-B stream (parser splits it into access units) ──
    unsigned char* iobuf = static_cast<unsigned char*>(av_malloc(1 << 16));
    AVIOContext* avio = avio_alloc_context(iobuf, 1 << 16, 0, blob, &read_cb,
                                           nullptr, nullptr);
    AVFormatContext* in = avformat_alloc_context();
    in->pb = avio;
    int rc = avformat_open_input(&in, nullptr, av_find_input_format("h264"), nullptr);
    if (rc == 0) rc = avformat_find_stream_info(in, nullptr);
    if (rc < 0) {
        std::fprintf(stderr, "[export color] %s: cannot parse h264 (%d)\n",
                     camera_dir.c_str(), rc);
        running_ = false;
        return;
    }

    // ── mp4 muxer, stream copy ──
    AVFormatContext* out = nullptr;
    avformat_alloc_output_context2(&out, nullptr, nullptr, out_path.c_str());
    AVStream* ost = avformat_new_stream(out, nullptr);
    avcodec_parameters_copy(ost->codecpar, in->streams[0]->codecpar);
    ost->codecpar->codec_tag = 0;
    // 90 kHz track timescale (the MPEG convention): fine enough that frames
    // arriving in a DDS burst (ms apart) keep distinct timestamps.
    ost->time_base = AVRational{1, 90000};
    AVDictionary* mux_opts = nullptr;
    av_dict_set(&mux_opts, "video_track_timescale", "90000", 0);
    const bool open_ok =
        avio_open(&out->pb, out_path.c_str(), AVIO_FLAG_WRITE) >= 0 &&
        avformat_write_header(out, &mux_opts) >= 0;
    av_dict_free(&mux_opts);
    if (!open_ok) {
        std::fprintf(stderr, "[export color] cannot write %s\n", out_path.c_str());
        running_ = false;
        return;
    }

    // Per-frame pts from the index's recv_ns (arrival clock). Baseline
    // profile carries no B-frames, so decode order == presentation order
    // and packet i maps to index row k+i.
    const AVRational us = {1, 1000000};
    const int64_t    t0 = rows[k].recv_ns;
    auto row_us = [&](size_t i) { return (rows[i].recv_ns - t0) / 1000; };

    AVPacket* pkt = av_packet_alloc();
    size_t  i        = 0;
    int64_t last_pts = INT64_MIN;
    bool    write_ok = true;
    while (!cancel_.load(std::memory_order_relaxed) &&
           av_read_frame(in, pkt) >= 0 && k + i < rows.size()) {
        const size_t  r    = k + i;
        const int64_t pts  = row_us(r);
        const int64_t next = (r + 1 < rows.size()) ? row_us(r + 1) : pts + 33333;
        int64_t ts = av_rescale_q(pts, us, ost->time_base);
        if (ts <= last_pts) ts = last_pts + 1;  // burst arrivals: keep strictly monotonic
        last_pts = ts;
        pkt->pts = pkt->dts = ts;
        pkt->duration       = av_rescale_q(next - pts, us, ost->time_base);
        pkt->stream_index   = 0;
        pkt->pos            = -1;
        if (av_interleaved_write_frame(out, pkt) < 0) {
            write_ok = false;
            break;
        }
        i++;
    }
    av_packet_free(&pkt);
    av_write_trailer(out);

    const bool complete = write_ok && !cancel_.load() && i == rows.size() - k;
    if (complete)
        std::printf("[export color] %s: %zu frames over %.2f s (skipped %zu pre-keyframe)\n",
                    out_path.c_str(), i, double(rows.back().recv_ns - t0) / 1e9, k);
    else
        std::fprintf(stderr, "[export color] %s: incomplete (%zu/%zu frames%s)\n",
                     out_path.c_str(), i, rows.size() - k,
                     cancel_.load() ? ", cancelled" : "");

    avio_closep(&out->pb);
    avformat_free_context(out);
    avformat_close_input(&in);
    avio_context_free(&avio);
    std::fclose(blob);

    ok_      = complete;
    running_ = false;
}

} // namespace kist
