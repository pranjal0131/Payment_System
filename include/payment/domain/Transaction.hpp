#pragma once
//
// Transaction.hpp — the core ENTITY of the system + a BUILDER to construct it.
//
// LLD lesson: Builder pattern.
//   A Transaction has many fields (parties, amount, method, idempotency key,
//   timestamps...). A constructor with 7+ args is unreadable and error-prone
//   ("did I pass payer before payee or after?"). The Builder gives a fluent,
//   self-documenting construction and validates required fields in build().
//
#include <string>
#include <chrono>
#include <optional>
#include "payment/common/Money.hpp"
#include "payment/common/Enums.hpp"

namespace pay::domain {

using common::Money;
using common::PaymentMethod;
using common::TxnStatus;

class Transaction {
public:
    // ---- Read-only accessors (state transitions happen via the State pattern,
    //      added in Phase 3; for now status has a guarded setter). ----
    const std::string& id()             const { return id_; }
    const std::string& idempotencyKey() const { return idempotencyKey_; }
    const std::string& payerAccountId() const { return payerAccountId_; }
    const std::string& payeeAccountId() const { return payeeAccountId_; }
    const Money&       amount()         const { return amount_; }
    PaymentMethod      method()         const { return method_; }
    TxnStatus          status()         const { return status_; }
    std::int64_t       createdAtEpoch() const { return createdAtEpoch_; }

    void setStatus(TxnStatus s) { status_ = s; }  // replaced by StateMachine in Phase 3

    // ---------------------- Builder ----------------------
    class Builder {
    public:
        Builder& id(std::string v)             { id_ = std::move(v); return *this; }
        Builder& idempotencyKey(std::string v) { key_ = std::move(v); return *this; }
        Builder& payer(std::string v)          { payer_ = std::move(v); return *this; }
        Builder& payee(std::string v)          { payee_ = std::move(v); return *this; }
        Builder& amount(Money v)               { amount_ = v; return *this; }
        Builder& method(PaymentMethod v)       { method_ = v; return *this; }

        Transaction build() const {
            if (id_.empty())    throw std::invalid_argument("Transaction.id is required");
            if (payer_.empty()) throw std::invalid_argument("Transaction.payer is required");
            if (payee_.empty()) throw std::invalid_argument("Transaction.payee is required");
            if (!amount_)       throw std::invalid_argument("Transaction.amount is required");
            return Transaction(id_, key_, payer_, payee_, *amount_, method_);
        }
    private:
        std::string id_, key_, payer_, payee_;
        std::optional<Money> amount_;
        PaymentMethod method_ = PaymentMethod::UPI;
    };

    // ---- Rehydration (reconstruct from persistence) ----
    // Repository adapters call this when LOADING a transaction from storage.
    // Unlike Builder (which creates a *new* txn in INITIATED state at "now"),
    // rehydrate restores the FULL persisted state — including status and the
    // original timestamp. Builder validation is skipped on purpose: the data was
    // already valid when it was first written. This is the ORM "hydration" pattern.
    static Transaction rehydrate(std::string id, std::string key, std::string payer,
                                 std::string payee, Money amount, PaymentMethod method,
                                 TxnStatus status, std::int64_t createdAtEpoch) {
        Transaction t(std::move(id), std::move(key), std::move(payer),
                      std::move(payee), amount, method);
        t.status_         = status;
        t.createdAtEpoch_ = createdAtEpoch;
        return t;
    }

private:
    Transaction(std::string id, std::string key, std::string payer,
                std::string payee, Money amount, PaymentMethod method)
        : id_(std::move(id)), idempotencyKey_(std::move(key)),
          payerAccountId_(std::move(payer)), payeeAccountId_(std::move(payee)),
          amount_(amount), method_(method), status_(TxnStatus::INITIATED),
          createdAtEpoch_(nowEpoch()) {}

    static std::int64_t nowEpoch() {
        using namespace std::chrono;
        return duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
    }

    std::string  id_;
    std::string  idempotencyKey_;
    std::string  payerAccountId_;
    std::string  payeeAccountId_;
    Money        amount_;
    PaymentMethod method_;
    TxnStatus    status_;
    std::int64_t createdAtEpoch_;
};

} // namespace pay::domain
