#pragma once
//
// InMemoryDistributedLock.hpp — ADAPTER for the DistributedLock port.
//
// In a single process a per-key std::mutex is a perfect stand-in for a
// distributed lock: it serializes threads contending for the SAME key while
// letting different keys run fully in parallel (this is "lock striping" — we
// lock per account, not one global lock). The real adapter swaps in Redis
// Redlock or a Postgres advisory lock with the identical `acquire(key)` shape.
//
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "payment/ports/Ports.hpp"

namespace pay::adapters {

class InMemoryDistributedLock final : public ports::DistributedLock {
public:
    std::unique_ptr<ports::DistributedLock::Guard> acquire(const std::string& key) override {
        // Fetch (or lazily create) the mutex dedicated to this key, then block
        // on it. Blocking here is deliberate: it mirrors a distributed lock that
        // waits for the current holder to release.
        return std::make_unique<InMemoryGuard>(mutexFor(key));
    }

private:
    // RAII guard that holds the per-key lock for its lifetime.
    class InMemoryGuard final : public ports::DistributedLock::Guard {
    public:
        explicit InMemoryGuard(std::shared_ptr<std::mutex> m)
            : owned_(std::move(m)), lk_(*owned_) {}
        bool acquired() const override { return lk_.owns_lock(); }
    private:
        std::shared_ptr<std::mutex>   owned_;  // keep the mutex alive while held
        std::unique_lock<std::mutex>  lk_;
    };

    std::shared_ptr<std::mutex> mutexFor(const std::string& key) {
        std::lock_guard<std::mutex> lk(mapMtx_);
        auto& slot = locks_[key];
        if (!slot) slot = std::make_shared<std::mutex>();
        return slot;
    }

    std::mutex mapMtx_;  // guards the map of per-key mutexes only
    std::unordered_map<std::string, std::shared_ptr<std::mutex>> locks_;
};

} // namespace pay::adapters
