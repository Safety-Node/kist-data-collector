#pragma once

// Reliable DDS camera subscriber — the recording-side replacement for
// kist-ext-sensor-io's ColorSubscriber/DepthSubscriber. Those wrap unitree's
// ChannelSubscriber, which creates BEST_EFFORT readers and exposes no QoS —
// a frame lost on the wire is simply gone. The SDK's writers DO offer
// RELIABLE (probed empirically), so a reader that requests it gets RTPS
// retransmission and recovers isolated wire losses — exactly the residual
// ~0.1% profile the tuned stack still showed. Only the collector changes;
// other consumers keep their best-effort readers (QoS is per-reader).
//
// QoS: Reliable + KeepAll (samples are drained immediately on the listener
// thread, so the depth never builds) + a resource cap as backstop. With
// reliable=false it degrades to a best-effort KeepLast reader — the safety
// valve if a transmitter ever stops offering RELIABLE (a reliable reader
// would then match nothing at all).
//
// All instances share one DomainParticipant, created on first use; it reads
// CYCLONEDDS_URI, which main routes to config/cyclonedds.xml (NIC + buffers).

#include <dds/dds.hpp>

#include <functional>
#include <iostream>
#include <memory>
#include <string>

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
            map_      = map;
            on_frame_ = std::move(on_frame);
            listener_ = std::make_unique<Listener>(this);
            reader_   = dds::sub::DataReader<MsgT>(
                sub_, topic_, qos, listener_.get(),
                dds::core::status::StatusMask::data_available());
        } catch (const std::exception& e) {
            std::cerr << "[ReliableSubscriber] start failed on " << topic_name
                      << ": " << e.what() << "\n";
            return false;
        }
        std::cout << "[ReliableSubscriber] " << (reliable ? "RELIABLE" : "best-effort")
                  << " reader on " << topic_name << "\n";
        return true;
    }

    void stop() {
        if (reader_ != dds::core::null) {
            reader_.listener(nullptr, dds::core::status::StatusMask::none());
            reader_.close();
            reader_ = dds::core::null;
        }
        listener_.reset();
    }

private:
    struct Listener : public dds::sub::NoOpDataReaderListener<MsgT> {
        explicit Listener(ReliableSubscriber* o) : owner(o) {}
        void on_data_available(dds::sub::DataReader<MsgT>& reader) override {
            auto samples = reader.take();
            for (const auto& s : samples)
                if (s.info().valid())
                    owner->on_frame_(owner->map_(s.data()));
        }
        ReliableSubscriber* owner;
    };

    dds::topic::Topic<MsgT>    topic_  = dds::core::null;
    dds::sub::Subscriber       sub_    = dds::core::null;
    dds::sub::DataReader<MsgT> reader_ = dds::core::null;
    std::unique_ptr<Listener>  listener_;
    MapFn                      map_ = nullptr;
    OnFrameFn                  on_frame_;
};

} // namespace kist
