#pragma once

// Reliable DDS camera subscriber — the recording-side replacement for
// kist-ext-sensor-io's ColorSubscriber/DepthSubscriber. Those wrap unitree's
// ChannelSubscriber, which creates BEST_EFFORT readers and exposes no QoS —
// a frame lost on the wire is simply gone. The SDK's writers DO offer
// RELIABLE (probed empirically), so a reader that requests it gets RTPS
// retransmission and recovers isolated wire losses. Only the collector
// changes; other consumers keep their best-effort readers (QoS is per-reader).
//
// Delivery is a waitset + owned take-thread, NOT a listener: detaching a
// ddscxx 0.10 listener while data is in flight races EntityDelegate::
// prevent_callbacks and hits an assertion (observed as a shutdown core
// dump under full camera load). With a waitset the teardown is trivially
// safe: stop the thread, then close the reader — no callback machinery.
//
// QoS: Reliable + KeepAll (drained continuously by the take-thread) + a
// resource cap as backstop. reliable=false degrades to best-effort
// KeepLast — the safety valve if a transmitter ever stops offering
// RELIABLE (a reliable reader would then match nothing at all).
//
// All instances share one DomainParticipant, created on first use; it reads
// CYCLONEDDS_URI, which main routes to config/cyclonedds.xml (NIC + buffers).

#include <dds/dds.hpp>

#include <atomic>
#include <functional>
#include <iostream>
#include <string>
#include <thread>

namespace kist {

inline dds::domain::DomainParticipant& reliable_dds_participant(int domain_id) {
    static dds::domain::DomainParticipant dp{domain_id};
    return dp;
}

template <typename MsgT, typename FrameT>
class ReliableSubscriber {
public:
    using MapFn     = FrameT (*)(const MsgT&);
    using OnFrameFn = std::function<void(const FrameT&)>;

    ReliableSubscriber() = default;
    ~ReliableSubscriber() { stop(); }

    bool start(int domain_id, const std::string& topic_name, bool reliable,
               MapFn map, OnFrameFn on_frame) {
        try {
            auto& dp  = reliable_dds_participant(domain_id);
            topic_    = dds::topic::Topic<MsgT>(dp, topic_name);
            sub_      = dds::sub::Subscriber(dp);
            auto qos  = sub_.default_datareader_qos();
            if (reliable)
                qos << dds::core::policy::Reliability::Reliable(
                           dds::core::Duration::from_millisecs(100))
                    << dds::core::policy::History::KeepAll()
                    << dds::core::policy::ResourceLimits(4096);
            else
                qos << dds::core::policy::History::KeepLast(30);
            reader_   = dds::sub::DataReader<MsgT>(sub_, topic_, qos);
            map_      = map;
            on_frame_ = std::move(on_frame);

            cond_ = dds::sub::cond::ReadCondition(
                reader_, dds::sub::status::DataState::any());
            waitset_.attach_condition(cond_);
        } catch (const std::exception& e) {
            std::cerr << "[ReliableSubscriber] start failed on " << topic_name
                      << ": " << e.what() << "\n";
            return false;
        }
        running_ = true;
        thread_  = std::thread(&ReliableSubscriber::run, this);
        std::cout << "[ReliableSubscriber] " << (reliable ? "RELIABLE" : "best-effort")
                  << " reader on " << topic_name << "\n";
        return true;
    }

    void stop() {
        if (!thread_.joinable()) return;
        running_ = false;
        thread_.join();       // take-thread gone -> nothing touches the reader
        drain();              // last sweep for samples that landed meanwhile
        waitset_.detach_condition(cond_);
        reader_.close();
        reader_ = dds::core::null;
    }

private:
    void run() {
        while (running_.load(std::memory_order_relaxed)) {
            try {
                waitset_.wait(dds::core::Duration::from_millisecs(100));
            } catch (const dds::core::TimeoutError&) {
                continue;  // periodic running_ check
            }
            drain();
        }
    }

    void drain() {
        auto samples = reader_.take();
        for (const auto& s : samples)
            if (s.info().valid())
                on_frame_(map_(s.data()));
    }

    dds::topic::Topic<MsgT>      topic_  = dds::core::null;
    dds::sub::Subscriber         sub_    = dds::core::null;
    dds::sub::DataReader<MsgT>   reader_ = dds::core::null;
    dds::sub::cond::ReadCondition cond_  = dds::core::null;
    dds::core::cond::WaitSet     waitset_;

    std::thread       thread_;
    std::atomic<bool> running_{false};
    MapFn             map_ = nullptr;
    OnFrameFn         on_frame_;
};

} // namespace kist
