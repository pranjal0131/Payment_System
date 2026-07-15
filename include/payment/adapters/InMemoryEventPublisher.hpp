#pragma once
//
// InMemoryEventPublisher.hpp — ADAPTER for the EventPublisher port.
//
// Implements a tiny in-process pub/sub so you can SEE events flow end-to-end.
// Subscribers register a callback; publish() fans out to all of them and also
// keeps a log for assertions/demo. The real adapter serializes the event and
// produces to Kafka/RabbitMQ; subscribers become separate consumer services.
//
// NOTE: this demo dispatches synchronously on the caller's thread. In production
// you would hand off to the ThreadPool / a real broker so publishing never
// blocks the payment's critical path.
//
#include <functional>
#include <mutex>
#include <vector>

#include "payment/ports/Ports.hpp"

namespace pay::adapters {

class InMemoryEventPublisher final : public ports::EventPublisher {
public:
    using Subscriber = std::function<void(const domain::DomainEvent&)>;

    void subscribe(Subscriber s) {
        std::lock_guard<std::mutex> lk(mtx_);
        subscribers_.push_back(std::move(s));
    }

    void publish(const domain::DomainEvent& event) override {
        std::vector<Subscriber> snapshot;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            log_.push_back(event);
            snapshot = subscribers_;   // copy so we don't hold the lock in callbacks
        }
        for (auto& s : snapshot) s(event);
    }

    std::vector<domain::DomainEvent> log() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return log_;
    }

private:
    mutable std::mutex               mtx_;
    std::vector<Subscriber>          subscribers_;
    std::vector<domain::DomainEvent> log_;
};

} // namespace pay::adapters
