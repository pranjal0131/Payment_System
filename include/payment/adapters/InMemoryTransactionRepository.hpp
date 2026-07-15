#pragma once
//
// InMemoryTransactionRepository.hpp — ADAPTER for the TransactionRepository port.
//
// Stores transactions by id. Transaction has no default/assignment-friendly
// public constructor, so we keep them in a map<string, Transaction> and replace
// the whole entry on save (last-write-wins, adequate for the demo).
//
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

#include "payment/ports/Ports.hpp"

namespace pay::adapters {

class InMemoryTransactionRepository final : public ports::TransactionRepository {
public:
    std::optional<domain::Transaction> find(const std::string& txnId) override {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = store_.find(txnId);
        return it == store_.end() ? std::nullopt
                                  : std::optional<domain::Transaction>(it->second);
    }

    void save(const domain::Transaction& txn) override {
        std::lock_guard<std::mutex> lk(mtx_);
        // map::insert_or_assign avoids needing Transaction to be default-constructible.
        store_.insert_or_assign(txn.id(), txn);
    }

private:
    std::mutex mtx_;
    std::unordered_map<std::string, domain::Transaction> store_;
};

} // namespace pay::adapters
