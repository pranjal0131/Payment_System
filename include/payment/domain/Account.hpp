#pragma once
//
// Account.hpp — an ENTITY (has identity = accountId) holding a balance.
//
// LLD lesson: Entity vs Value Object.
//   * Money (value object): no id, immutable, compared by value.
//   * Account (entity): has a unique id, mutable state (balance), compared by id.
//
// Money inside an account must be protected against concurrent debit/credit,
// otherwise two threads can both read balance=100, both subtract 100, and we
// just spent 200 from a 100 balance ("lost update" / double-spend).
// We guard mutations with a mutex here. (Later, real durability + atomicity
// moves to the DB layer with row locks / optimistic versioning.)
//
#include <mutex>
#include <string>
#include "payment/common/Money.hpp"
#include "payment/common/Enums.hpp"
#include "payment/common/Result.hpp"

namespace pay::domain {

using common::Money;
using common::Currency;
using common::AccountType;
using common::Status;
using common::ErrorCode;

class Account {
public:
    Account(std::string id, AccountType type, Money openingBalance)
        : id_(std::move(id)), type_(type), balance_(openingBalance) {}

    const std::string& id()   const { return id_; }
    AccountType        type() const { return type_; }

    // Snapshot read of the balance (copy is safe; Money is immutable).
    Money balance() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return balance_;
    }

    // Add funds. Always allowed (within currency).
    Status credit(const Money& amount) {
        std::lock_guard<std::mutex> lk(mtx_);
        if (amount.currency() != balance_.currency())
            return Status::fail(ErrorCode::VALIDATION_FAILED, "currency mismatch on credit");
        if (amount.isNegative())
            return Status::fail(ErrorCode::VALIDATION_FAILED, "credit amount must be >= 0");
        balance_ = balance_ + amount;
        return Status::ok();
    }

    // Remove funds. Rejected if it would overdraw the account.
    // This single guarded check-then-act is what prevents double-spend
    // *within a process*. (Cross-process needs the DB/distributed lock.)
    Status debit(const Money& amount) {
        std::lock_guard<std::mutex> lk(mtx_);
        if (amount.currency() != balance_.currency())
            return Status::fail(ErrorCode::VALIDATION_FAILED, "currency mismatch on debit");
        if (amount.isNegative())
            return Status::fail(ErrorCode::VALIDATION_FAILED, "debit amount must be >= 0");
        if (amount > balance_)
            return Status::fail(ErrorCode::INSUFFICIENT_FUNDS,
                "balance " + balance_.toString() + " < debit " + amount.toString());
        balance_ = balance_ - amount;
        return Status::ok();
    }

private:
    std::string       id_;
    AccountType       type_;
    Money             balance_;
    mutable std::mutex mtx_;   // mutable so const balance() can lock it
};

} // namespace pay::domain
