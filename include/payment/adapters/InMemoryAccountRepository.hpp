#pragma once
//
// InMemoryAccountRepository.hpp — an ADAPTER implementing the AccountRepository port.
//
// This is the "test double / local dev" adapter. It stores accounts in a hash
// map guarded by a mutex. The exact same interface will later be implemented by
// a PostgresAccountRepository (SELECT ... FOR UPDATE for row locks). The service
// layer never knows the difference — that is the payoff of coding to the port.
//
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "payment/ports/Ports.hpp"

namespace pay::adapters {

class InMemoryAccountRepository final : public ports::AccountRepository {
public:
    std::shared_ptr<domain::Account> find(const std::string& accountId) override {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = store_.find(accountId);
        return it == store_.end() ? nullptr : it->second;
    }

    void save(std::shared_ptr<domain::Account> account) override {
        std::lock_guard<std::mutex> lk(mtx_);
        // We store the shared_ptr itself, so mutations to the Account (which is
        // internally mutex-guarded) are visible to every holder — mimicking a
        // shared row of truth.
        store_[account->id()] = std::move(account);
    }

private:
    std::mutex mtx_;
    std::unordered_map<std::string, std::shared_ptr<domain::Account>> store_;
};

} // namespace pay::adapters
