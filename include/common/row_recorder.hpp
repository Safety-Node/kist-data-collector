#pragma once

// Row-stream persistence: one fixed-schema record stream -> one CSV file.
// The row-shaped sibling of BlobRecorder (blob + index) for small,
// structured records — robot lowstate at ~1 kHz, UWB fixes, etc. — where
// "one CSV row per record" is the natural storage form and a blob file
// would be pointless indirection.
//
//   sensor callback thread:  push(record)  -> RecordQueue (copy + stamp only)
//   writer thread (owned):   pop_all batch -> append rows, flush per batch
//
// The hand-off semantics (bounded, drop-counted, drain-on-close) come from
// common/record_queue.hpp — same losslessness contract as the blob path.
// Row streams carry no publisher seq, so StreamStats.wire_gaps stays 0.

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

namespace kist {

// RowFn renders one record as the CSV row (without trailing newline);
// recv_ns (host CLOCK_REALTIME at arrival) is captured in push() so it
// reflects arrival, not write, time — the cross-stream alignment column.
template <typename Record>
class RowRecorder {
public:
    using RowFn = std::string (*)(const Record& record, int64_t recv_ns);

    ~RowRecorder() { close(); }

    bool open(const std::string& csv_path, const std::string& header,
              RowFn row, size_t queue_capacity) {
        file_.open(csv_path, std::ios::trunc);
        if (!file_.is_open()) return false;
        file_ << header << "\n";
        row_ = row;
        queue_.open(queue_capacity);
        thread_ = std::thread(&RowRecorder::run, this);
        return true;
    }

    // Producer side: copy the record in and return (must stay cheap — runs
    // on the sensor callback thread).
    void push(const Record& record) {
        const int64_t recv_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        received_.fetch_add(1, std::memory_order_relaxed);
        if (!queue_.push({record, recv_ns}))
            dropped_.fetch_add(1, std::memory_order_relaxed);
    }

    // Drain everything already queued, then join + close the file. Call
    // only after the subscriber feeding push() has stopped.
    void close() {
        if (!thread_.joinable()) return;
        queue_.close();
        thread_.join();
        file_.close();
    }

    StreamStats stats() const {
        StreamStats s;
        s.received     = received_.load(std::memory_order_relaxed);
        s.dropped      = dropped_.load(std::memory_order_relaxed);
        s.written      = written_.load(std::memory_order_relaxed);
        s.write_errors = write_errors_.load(std::memory_order_relaxed);
        s.bytes        = bytes_.load(std::memory_order_relaxed);
        return s;
    }

private:
    void run() {
        std::deque<std::pair<Record, int64_t>> batch;
        while (queue_.pop_all(batch)) {
            for (auto& [record, recv_ns] : batch) {
                const std::string row = row_(record, recv_ns);
                file_ << row << "\n";
                if (file_.good()) {
                    written_.fetch_add(1, std::memory_order_relaxed);
                    bytes_.fetch_add(row.size() + 1, std::memory_order_relaxed);
                } else {
                    write_errors_.fetch_add(1, std::memory_order_relaxed);
                }
            }
            file_.flush();
        }
    }

    RecordQueue<std::pair<Record, int64_t>> queue_;   // record + recv_ns
    std::ofstream file_;
    RowFn         row_ = nullptr;
    std::thread   thread_;

    std::atomic<uint64_t> received_{0}, dropped_{0}, written_{0},
                          write_errors_{0}, bytes_{0};
};

} // namespace kist
