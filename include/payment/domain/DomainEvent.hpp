#pragma once
//
// DomainEvent.hpp — a fact that already happened, published for others to react.
//
// LLD/HLD lesson: event-driven architecture. When a payment settles, many
// downstream concerns care (notifications, analytics, fraud retraining, merchant
// payout). If the PaymentService called each of them directly it would be tightly
// coupled to all of them and slow (its latency = sum of theirs). Instead it
// publishes an EVENT and moves on; subscribers react asynchronously.
//
// Events are named in the PAST TENSE ("PaymentCaptured") because they describe
// something that is already true and immutable. This is the same envelope you'd
// serialize onto Kafka/RabbitMQ in production.
//
#include <cstdint>
#include <string>

namespace pay::domain {

struct DomainEvent {
    std::string  type;        // e.g. "PaymentCaptured", "PaymentFailed"
    std::string  aggregateId; // the transaction id this event is about
    std::string  payload;     // JSON-ish detail (kept as a string for the demo)
    std::int64_t occurredAtEpoch;
};

} // namespace pay::domain
