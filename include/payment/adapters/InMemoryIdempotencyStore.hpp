#pragma once
//
// InMemoryIdempotencyStore.hpp — ADAPTER for the IdempotencyStore port.
//
// putIfAbsent is the crux: it must be ATOMIC (check + insert under one lock),
// otherwise two concurrent retries of the same request both see "absent" and
// both proceed — the exact double-charge idempotency is meant to prevent. In
// Redis this same atomicity comes from `SET key val NX`.
//
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

#include "payment/ports/Ports.hpp"

namespace pay::adapters {

class InMemoryIdempotencyStore final : public ports::IdempotencyStore {
public:
    std::optional<std::string> get(const std::string& key) override {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = store_.find(key);
        return it == store_.end() ? std::nullopt
                                  : std::optional<std::string>(it->second);
    }

    bool putIfAbsent(const std::string& key, const std::string& txnId) override {
        std::lock_guard<std::mutex> lk(mtx_);
        // emplace returns {iterator, inserted?}. inserted == true means the key
        // was absent and WE wrote it — i.e. this caller owns the request.
        auto [_, inserted] = store_.emplace(key, txnId);
        return inserted;
    }

private:
    std::mutex mtx_;
    std::unordered_map<std::string, std::string> store_;
};

} // namespace pay::adapters
