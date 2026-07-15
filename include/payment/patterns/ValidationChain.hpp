#pragma once
//
// ValidationChain.hpp — the CHAIN OF RESPONSIBILITY pattern.
//
// LLD lesson: before a payment runs it must pass a pipeline of independent
// checks — basic validation, KYC/limits, fraud scoring. Cramming these into one
// function couples unrelated concerns and makes each check impossible to reorder,
// disable, or unit-test on its own.
//
// Chain of Responsibility models the pipeline as a linked list of handlers. Each
// handler either rejects (short-circuit, stop the chain) or passes the request
// to the next handler. New checks are added by inserting a link — existing
// handlers don't change (Open/Closed again). Order is explicit and configurable,
// which matters: cheap checks (null amount) should run before expensive ones
// (a fraud-model call).
//
#include <memory>
#include <string>
#include <vector>

#include "payment/common/Money.hpp"
#include "payment/common/Result.hpp"
#include "payment/domain/Transaction.hpp"

namespace pay::patterns {

using common::Status;
using common::ErrorCode;
using common::Money;
using domain::Transaction;

// One link in the chain. Each handler inspects the transaction and returns
// ok() to continue or a typed error to reject the whole request.
class ValidationHandler {
public:
    virtual ~ValidationHandler() = default;
    virtual std::string name()  const = 0;
    virtual Status      check(const Transaction& txn) const = 0;
};

using ValidationHandlerPtr = std::shared_ptr<ValidationHandler>;

// The chain runner. Handlers execute in insertion order; the first rejection
// short-circuits and is returned. This object is immutable after wiring, so it
// is safe to share read-only across threads.
class ValidationChain {
public:
    ValidationChain& add(ValidationHandlerPtr h) {
        handlers_.push_back(std::move(h));
        return *this;
    }

    Status run(const Transaction& txn) const {
        for (const auto& h : handlers_) {
            Status s = h->check(txn);
            if (s.isErr()) return s;   // short-circuit on first failure
        }
        return Status::ok();
    }

private:
    std::vector<ValidationHandlerPtr> handlers_;
};

// ---------------------------------------------------------------------------
// Concrete handlers. Small, single-responsibility, independently testable.
// ---------------------------------------------------------------------------

// 1) Structural validation: amount must be present and strictly positive.
class BasicValidationHandler final : public ValidationHandler {
public:
    std::string name() const override { return "BasicValidation"; }
    Status check(const Transaction& txn) const override {
        if (!txn.amount().isPositive())
            return Status::fail(ErrorCode::VALIDATION_FAILED, "amount must be > 0");
        if (txn.payerAccountId() == txn.payeeAccountId())
            return Status::fail(ErrorCode::VALIDATION_FAILED, "payer and payee must differ");
        return Status::ok();
    }
};

// 2) Per-transaction limit (a stand-in for KYC tiers / velocity limits).
class LimitValidationHandler final : public ValidationHandler {
public:
    explicit LimitValidationHandler(Money perTxnLimit) : limit_(perTxnLimit) {}
    std::string name() const override { return "LimitValidation"; }
    Status check(const Transaction& txn) const override {
        if (txn.amount() > limit_)
            return Status::fail(ErrorCode::VALIDATION_FAILED,
                "amount " + txn.amount().toString() + " exceeds per-txn limit " + limit_.toString());
        return Status::ok();
    }
private:
    Money limit_;
};

// 3) Fraud screening (stubbed rule; a real one calls a model / rules engine).
//    Here: flag suspiciously round, very large amounts as an illustration.
class FraudCheckHandler final : public ValidationHandler {
public:
    explicit FraudCheckHandler(Money reviewThreshold) : threshold_(reviewThreshold) {}
    std::string name() const override { return "FraudCheck"; }
    Status check(const Transaction& txn) const override {
        if (txn.amount() >= threshold_)
            return Status::fail(ErrorCode::FRAUD_REJECTED,
                "amount " + txn.amount().toString() + " held for fraud review");
        return Status::ok();
    }
private:
    Money threshold_;
};

} // namespace pay::patterns
