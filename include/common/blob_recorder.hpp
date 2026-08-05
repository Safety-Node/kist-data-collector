#pragma once

// Blob-stream persistence: one compressed frame stream -> blob file + CSV
// index. The writer half of the recording pipeline —
//
//   sensor callback thread:  push(frame)   -> RecordQueue (copy + stamp only)
//   writer thread (owned):   pop_all batch -> append blob + index row, flush
//
// The latest-wins DataBuffer in kist-ext-sensor-io cannot feed a recorder
// (a poll that lands after two SetData() calls has already lost a frame),
// so recorders tap the subscribers' set_on_frame() hook instead; push()
// runs on that DDS thread and must never block — the hand-off semantics
// (bounded, drop-counted, drain-on-close) live in common/record_queue.hpp.
//
// The payload is written verbatim (no decode/re-encode), so the write rate
// is the wire rate (~MB/s per stream). `dropped` and `write_errors` staying
// 0 is the losslessness invariant, reported at 1 Hz and in meta.yaml.
// Row-shaped streams (UWB fixes, robot state) don't fit the blob+index
// form — give those their own writer over the same RecordQueue.

#include "common/record_queue.hpp"
#include "common/stream_stats.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <fstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace kist {

// Append-only blob file + CSV index. Each append writes the payload at the
// current offset and one index row "<prefix>,<offset>,<size>" — the index is
// the random-access map into the blob (and the per-frame timestamp record).
class BlobSink {
public:
    bool open(const std::string& data_path, const std::string& index_path,
              const std::string& index_header) {
        data_.open(data_path, std::ios::binary | std::ios::trunc);
        index_.open(index_path, std::ios::trunc);
        if (!data_.is_open() || !index_.is_open()) return false;
        index_ << index_header << "\n";
        return true;
    }

    bool append(const std::vector<uint8_t>& payload, const std::string& row_prefix) {
        data_.write(reinterpret_cast<const char*>(payload.data()),
                    std::streamsize(payload.size()));
        index_ << row_prefix << ',' << offset_ << ',' << payload.size() << "\n";
        offset_ += payload.size();
        return data_.good() && index_.good();
    }

    void flush() { data_.flush(); index_.flush(); }

    void close() {
        if (data_.is_open())  data_.close();
        if (index_.is_open()) index_.close();
    }

private:
    std::ofstream data_;
    std::ofstream index_;
    uint64_t      offset_ = 0;
};

// Queue + writer thread for one stream of frames (H264ColorFrame or
// RvlDepthFrame — anything with .sequence and .data). RowFn renders the
// index columns before offset,size; recv_ns (host CLOCK_REALTIME at
// arrival) is captured in push() so it reflects arrival, not write, time.
template <typename Frame>
class BlobRecorder {
public:
    using RowFn = std::string (*)(const Frame& frame, int64_t recv_ns);

    ~BlobRecorder() { close(); }

    bool open(const std::string& data_path, const std::string& index_path,
              const std::string& index_header, RowFn row, size_t queue_capacity) {
        if (!sink_.open(data_path, index_path, index_header)) return false;
        row_ = row;
        queue_.open(queue_capacity);
        thread_ = std::thread(&BlobRecorder::run, this);
        return true;
    }

    // Producer side: copy the frame in and return. The copy (~tens of KB
    // of compressed payload) is the whole cost on the callback thread.
    void push(const Frame& frame) {
        const int64_t recv_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        received_.fetch_add(1, std::memory_order_relaxed);
        if (!queue_.push({frame, recv_ns}))
            dropped_.fetch_add(1, std::memory_order_relaxed);
    }

    // Drain everything already queued, then join + close the files. Call
    // only after the subscriber feeding push() has stopped.
    void close() {
        if (!thread_.joinable()) return;
        queue_.close();
        thread_.join();
        sink_.close();
    }

    StreamStats stats() const {
        StreamStats s;
        s.received     = received_.load(std::memory_order_relaxed);
        s.dropped      = dropped_.load(std::memory_order_relaxed);
        s.written      = written_.load(std::memory_order_relaxed);
        s.write_errors = write_errors_.load(std::memory_order_relaxed);
        s.wire_gaps    = wire_gaps_.load(std::memory_order_relaxed);
        s.bytes        = bytes_.load(std::memory_order_relaxed);
        return s;
    }

private:
    void run() {
        std::deque<std::pair<Frame, int64_t>> batch;
        while (queue_.pop_all(batch)) {
            for (auto& [frame, recv_ns] : batch) {
                if (have_seq_ && frame.sequence > last_seq_ + 1)
                    wire_gaps_.fetch_add(frame.sequence - last_seq_ - 1,
                                         std::memory_order_relaxed);
                last_seq_ = frame.sequence;
                have_seq_ = true;
                if (sink_.append(frame.data, row_(frame, recv_ns))) {
                    written_.fetch_add(1, std::memory_order_relaxed);
                    bytes_.fetch_add(frame.data.size(), std::memory_order_relaxed);
                } else {
                    write_errors_.fetch_add(1, std::memory_order_relaxed);
                }
            }
            sink_.flush();
        }
    }

    RecordQueue<std::pair<Frame, int64_t>> queue_;   // frame + recv_ns
    BlobSink    sink_;
    RowFn       row_ = nullptr;
    std::thread thread_;

    std::atomic<uint64_t> received_{0}, dropped_{0}, written_{0},
                          write_errors_{0}, wire_gaps_{0}, bytes_{0};
    // seq tracking is writer-thread-only
    uint64_t last_seq_ = 0;
    bool     have_seq_ = false;
};

} // namespace kist
