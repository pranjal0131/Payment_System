#pragma once
//
// Ports.hpp — the PORTS of the hexagonal architecture (all interfaces in one place).
//
// LLD/HLD lesson: a "port" is an interface the core domain OWNS and depends on.
// The domain says "I need somewhere to save an account" (AccountRepository) — it
// does NOT say "I need PostgreSQL". Concrete technology lives in ADAPTERS that
// implement these ports. Result:
//
//   * The domain compiles and is fully testable with zero external infra.
//   * Swapping Postgres -> DynamoDB, or in-memory -> real, changes ONE adapter
//     and touches no business logic (Dependency Inversion Principle).
//
// Each interface below is deliberately narrow (Interface Segregation): callers
// depend only on the operations they actually use.
//
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "payment/common/Money.hpp"
#include "payment/common/Result.hpp"
#include "payment/domain/Account.hpp"
#include "payment/domain/Transaction.hpp"
#include "payment/domain/LedgerEntry.hpp"
#include "payment/domain/DomainEvent.hpp"

namespace pay::ports {

using common::Money;
using common::Status;
using domain::Account;
using domain::Transaction;
using domain::LedgerEntry;
using domain::DomainEvent;

// --- Persistence for account state ------------------------------------------
// Adapter examples: InMemory (now), Postgres with row-level locks (later).
class AccountRepository {
public:
    virtual ~AccountRepository() = default;
    virtual std::shared_ptr<Account> find(const std::string& accountId) = 0;
    virtual void                     save(std::shared_ptr<Account> account) = 0;
};

// --- Persistence for transactions -------------------------------------------
class TransactionRepository {
public:
    virtual ~TransactionRepository() = default;
    virtual std::optional<Transaction> find(const std::string& txnId) = 0;
    virtual void                       save(const Transaction& txn) = 0;
};

// --- Idempotency store (a cache port) ---------------------------------------
// Maps an idempotency key -> the txn id produced the first time we saw it, so a
// retried request returns the original result instead of charging twice.
// Adapter examples: InMemory (now), Redis with SET NX + TTL (later).
class IdempotencyStore {
public:
    virtual ~IdempotencyStore() = default;
    // Returns the previously stored txn id for this key, if any.
    virtual std::optional<std::string> get(const std::string& key) = 0;
    // Atomically store key->txnId only if absent. Returns true if WE won the
    // race (first writer). Mirrors Redis `SET key val NX`.
    virtual bool putIfAbsent(const std::string& key, const std::string& txnId) = 0;
};

// --- Distributed lock port --------------------------------------------------
// Guards a critical section across processes/instances (in-memory it is just a
// mutex; in production it is Redis Redlock / a DB advisory lock). The service
// uses it to serialize concurrent operations on the SAME payer account so two
// requests can't double-spend across instances.
class DistributedLock {
public:
    virtual ~DistributedLock() = default;
    // RAII guard: lock on construction, release on destruction.
    class Guard {
    public:
        virtual ~Guard() = default;
        virtual bool acquired() const = 0;
    };
    virtual std::unique_ptr<Guard> acquire(const std::string& key) = 0;
};

// --- Event publisher port ---------------------------------------------------
// Adapter examples: InMemory dispatch (now), Kafka/RabbitMQ producer (later).
class EventPublisher {
public:
    virtual ~EventPublisher() = default;
    virtual void publish(const DomainEvent& event) = 0;
};

// --- Ledger port (double-entry postings) ------------------------------------
class LedgerRepository {
public:
    virtual ~LedgerRepository() = default;
    // Append a set of entries as ONE atomic, balanced posting. Implementations
    // MUST reject a batch whose signed amounts don't sum to zero.
    virtual Status post(const std::vector<LedgerEntry>& entries) = 0;
    virtual std::vector<LedgerEntry> entriesFor(const std::string& accountId) = 0;
};

} // namespace pay::ports
