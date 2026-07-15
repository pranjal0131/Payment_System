#pragma once
//
// PaymentService.hpp — the APPLICATION SERVICE that orchestrates a payment.
//
// This is the hexagon's core use-case layer. It owns NO infrastructure: every
// dependency is a PORT injected through the constructor (Dependency Injection).
// That is why the whole flow runs on in-memory adapters today and on
// Postgres/Redis/Kafka tomorrow with no change here.
//
// A single pay() call demonstrates the four hard problems every real payment
// system must solve, each called out inline below:
//
//   [1] IDEMPOTENCY      — a retried request must not charge twice.
//   [2] DOUBLE-SPEND     — concurrent requests on one account must serialize.
//   [3] STATE INTEGRITY  — only legal lifecycle transitions are allowed.
//   [4] DOUBLE-ENTRY     — money is moved via balanced ledger postings, never
//                          created or destroyed.
//
#include <memory>
#include <string>
#include <vector>

#include "payment/common/Money.hpp"
#include "payment/common/Result.hpp"
#include "payment/domain/Transaction.hpp"
#include "payment/domain/LedgerEntry.hpp"
#include "payment/domain/DomainEvent.hpp"
#include "payment/ports/Ports.hpp"
#include "payment/patterns/PaymentStrategyFactory.hpp"
#include "payment/patterns/TransactionStateMachine.hpp"
#include "payment/patterns/ValidationChain.hpp"
#include "payment/infra/IdGenerator.hpp"

namespace pay::service {

using common::Money;
using common::Result;
using common::Status;
using common::ErrorCode;
using common::PaymentMethod;
using common::TxnStatus;
using common::Direction;
using domain::Transaction;
using domain::LedgerEntry;
using domain::DomainEvent;

// Input DTO for a payment. A dedicated request object keeps the public API
// stable as fields are added, and keeps call sites self-documenting.
struct PaymentRequest {
    std::string   idempotencyKey;   // client-supplied; the key to safe retries
    std::string   payerAccountId;
    std::string   payeeAccountId;
    Money         amount;
    PaymentMethod method;
};

class PaymentService {
public:
    // All collaborators are injected. The service depends on the PORT interfaces,
    // not on any concrete adapter — the Dependency Inversion Principle in action.
    PaymentService(ports::AccountRepository&     accounts,
                   ports::TransactionRepository& transactions,
                   ports::IdempotencyStore&      idempotency,
                   ports::DistributedLock&       locks,
                   ports::LedgerRepository&      ledger,
                   ports::EventPublisher&        events,
                   patterns::PaymentStrategyFactory& strategies,
                   patterns::ValidationChain&        validation)
        : accounts_(accounts), transactions_(transactions), idempotency_(idempotency),
          locks_(locks), ledger_(ledger), events_(events),
          strategies_(strategies), validation_(validation),
          txnIds_("txn"), entryIds_("le"), seq_(1) {}

    // Execute a payment end-to-end. Returns the resulting Transaction on success
    // (including a replayed one on a duplicate), or a typed Error on failure.
    Result<Transaction> pay(const PaymentRequest& req) {
        // -----------------------------------------------------------------
        // [1] IDEMPOTENCY — reserve the key BEFORE doing any work.
        // We mint a candidate txn id and atomically claim the key. If someone
        // already claimed it (a retry / concurrent duplicate), we return their
        // original transaction instead of processing a second charge.
        // -----------------------------------------------------------------
        const std::string txnId = txnIds_.next();
        if (!idempotency_.putIfAbsent(req.idempotencyKey, txnId)) {
            auto existingId = idempotency_.get(req.idempotencyKey);
            if (existingId) {
                if (auto prior = transactions_.find(*existingId))
                    return Result<Transaction>::ok(*prior);   // replay original
            }
            return Result<Transaction>::fail(ErrorCode::DUPLICATE_REQUEST,
                "duplicate request for idempotency key: " + req.idempotencyKey);
        }

        // Build the transaction aggregate (Builder pattern) in INITIATED state.
        Transaction txn = Transaction::Builder()
            .id(txnId)
            .idempotencyKey(req.idempotencyKey)
            .payer(req.payerAccountId)
            .payee(req.payeeAccountId)
            .amount(req.amount)
            .method(req.method)
            .build();

        // -----------------------------------------------------------------
        // [2] DOUBLE-SPEND — serialize everything that touches the payer's
        // account behind a per-account distributed lock. Two concurrent
        // payments from the same wallet cannot interleave their read-modify-
        // write of the balance. Different payers still run fully in parallel.
        // The RAII Guard releases the lock when it goes out of scope.
        // -----------------------------------------------------------------
        auto guard = locks_.acquire("account:" + req.payerAccountId);

        // Run the validation / KYC / fraud pipeline (Chain of Responsibility).
        if (Status v = validation_.run(txn); v.isErr())
            return fail(txn, v.error().code, v.error().message);

        // Load both accounts through the repository port.
        auto payer = accounts_.find(req.payerAccountId);
        auto payee = accounts_.find(req.payeeAccountId);
        if (!payer) return fail(txn, ErrorCode::ACCOUNT_NOT_FOUND, "payer not found");
        if (!payee) return fail(txn, ErrorCode::ACCOUNT_NOT_FOUND, "payee not found");

        // Select the payment method implementation (Strategy via Factory).
        auto strategy = strategies_.create(req.method);
        if (!strategy)
            return fail(txn, ErrorCode::VALIDATION_FAILED, "unsupported payment method");

        // -----------------------------------------------------------------
        // [3] STATE INTEGRITY — authorize, and advance the lifecycle only
        // through the state machine, which rejects any illegal transition.
        // -----------------------------------------------------------------
        auto auth = strategy->authorize(txn);
        if (auth.isErr())
            return fail(txn, auth.error().code, auth.error().message);
        advance(txn, TxnStatus::AUTHORIZED);

        // Move the money. In-process guarded debit/credit gives immediate
        // double-spend safety on the live balances...
        if (Status d = payer->debit(req.amount); d.isErr())
            return fail(txn, d.error().code, d.error().message);
        payee->credit(req.amount);   // credit cannot fail for a valid amount

        // -----------------------------------------------------------------
        // [4] DOUBLE-ENTRY — ...and record the movement as a balanced pair of
        // ledger postings (DEBIT payer / CREDIT payee). The ledger adapter
        // rejects the batch unless the signed amounts net to zero, so money can
        // never be conjured or lost. This journal is the audit source of truth.
        // -----------------------------------------------------------------
        std::vector<LedgerEntry> posting = {
            LedgerEntry(entryIds_.next(), txnId, req.payerAccountId,
                        Direction::DEBIT,  req.amount, seq_++),
            LedgerEntry(entryIds_.next(), txnId, req.payeeAccountId,
                        Direction::CREDIT, req.amount, seq_++),
        };
        if (Status p = ledger_.post(posting); p.isErr()) {
            // Compensate the in-memory balances if the journal rejects the batch
            // (keeps live balances consistent with the ledger — the source of truth).
            payer->credit(req.amount);
            payee->debit(req.amount);
            return fail(txn, p.error().code, p.error().message);
        }

        // Capture: funds committed. Advance to CAPTURED and persist.
        if (Status c = strategy->capture(txn, auth.value()); c.isErr())
            return fail(txn, c.error().code, c.error().message);
        advance(txn, TxnStatus::CAPTURED);

        transactions_.save(txn);
        publish(txn, "PaymentCaptured");
        return Result<Transaction>::ok(txn);
    }

private:
    // Advance the transaction's status through the state machine. Illegal jumps
    // are impossible: canTransition gates every change. (Guarded here so callers
    // can't corrupt the lifecycle.)
    void advance(Transaction& txn, TxnStatus to) {
        if (stateMachine_.canTransition(txn.status(), to))
            txn.setStatus(to);
    }

    // Mark the transaction FAILED, persist it, emit an event, and surface the error.
    Result<Transaction> fail(Transaction& txn, ErrorCode code, const std::string& msg) {
        advance(txn, TxnStatus::FAILED);
        transactions_.save(txn);
        publish(txn, "PaymentFailed");
        return Result<Transaction>::fail(code, msg);
    }

    void publish(const Transaction& txn, const std::string& type) {
        events_.publish(DomainEvent{
            type, txn.id(),
            std::string("{\"amount\":\"") + txn.amount().toString() +
                "\",\"status\":\"" + std::string(common::toString(txn.status())) + "\"}",
            txn.createdAtEpoch()});
    }

    ports::AccountRepository&          accounts_;
    ports::TransactionRepository&      transactions_;
    ports::IdempotencyStore&           idempotency_;
    ports::DistributedLock&            locks_;
    ports::LedgerRepository&           ledger_;
    ports::EventPublisher&             events_;
    patterns::PaymentStrategyFactory&  strategies_;
    patterns::ValidationChain&         validation_;
    patterns::TransactionStateMachine  stateMachine_;   // owned; pure logic, no state

    infra::IdGenerator txnIds_;
    infra::IdGenerator entryIds_;
    // Global ledger ordering. Atomic because payments on DIFFERENT accounts run
    // in parallel (the per-account lock does not serialize them against each other).
    std::atomic<std::int64_t> seq_;
};

} // namespace pay::service
