#pragma once
//
// IdGenerator.hpp — thread-safe, monotonic, prefixed ID generation.
//
// LLD lesson: every entity (transaction, ledger entry, event) needs a unique id.
// Generating ids from a shared counter is a textbook data race: two threads read
// the same value, both increment, both hand out the same id. We fix this with a
// std::atomic counter — the increment is a single indivisible hardware operation,
// so no mutex is needed and there is zero lock contention on the hot path.
//
// HLD note: in a real distributed system a single in-process counter is not
// enough (many service instances). Production options are Snowflake ids
// (timestamp + machine id + sequence), UUIDv7, or a DB sequence. We keep the
// same *interface* here so a distributed generator drops in later without
// touching callers — the whole point of coding to an abstraction.
//
#include <atomic>
#include <cstdint>
#include <string>

namespace pay::infra {

// A monotonically increasing id source. Cheap, lock-free, thread-safe.
//
//   IdGenerator txnIds{"txn"};
//   txnIds.next();  // -> "txn_1", "txn_2", ... across all threads, no dupes
//
class IdGenerator {
public:
    explicit IdGenerator(std::string prefix, std::uint64_t start = 1)
        : prefix_(std::move(prefix)), counter_(start) {}

    // Returns a unique, human-readable id such as "txn_42".
    // fetch_add is atomic: N concurrent callers get N distinct numbers.
    std::string next() {
        std::uint64_t n = counter_.fetch_add(1, std::memory_order_relaxed);
        return prefix_ + "_" + std::to_string(n);
    }

    // Raw numeric id when you don't want the prefix string cost.
    std::uint64_t nextRaw() {
        return counter_.fetch_add(1, std::memory_order_relaxed);
    }

private:
    const std::string          prefix_;
    std::atomic<std::uint64_t> counter_;
};

} // namespace pay::infra
