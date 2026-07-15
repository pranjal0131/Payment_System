//
// main.cpp — full end-to-end demo of the payment system (Phases 1–5).
//
// This wires the hexagon together: in-memory ADAPTERS behind the PORTS, the
// design PATTERNS (Strategy/Factory/State/Chain), and the concurrency primitives
// (ThreadPool). It then drives the PaymentService through the scenarios an
// interviewer will ask about: happy path, idempotent retry, insufficient funds,
// fraud rejection, and a concurrent double-spend stress test.
//
// Nothing here needs Postgres/Redis/Kafka — that is the entire point of coding
// to interfaces. Swap the adapters and the same demo runs on real infra.
//
#include <atomic>
#include <iostream>
#include <memory>
#include <vector>

#include "payment/common/Money.hpp"
#include "payment/domain/Account.hpp"

#include "payment/adapters/InMemoryAccountRepository.hpp"
#include "payment/adapters/InMemoryTransactionRepository.hpp"
#include "payment/adapters/InMemoryIdempotencyStore.hpp"
#include "payment/adapters/InMemoryDistributedLock.hpp"
#include "payment/adapters/InMemoryEventPublisher.hpp"
#include "payment/adapters/InMemoryLedgerRepository.hpp"
#include "payment/adapters/FileTransactionRepository.hpp"

#include "payment/patterns/PaymentStrategyFactory.hpp"
#include "payment/patterns/ValidationChain.hpp"

#include "payment/infra/ThreadPool.hpp"
#include "payment/service/PaymentService.hpp"

using namespace pay::common;
using namespace pay::domain;
using namespace pay::adapters;
using namespace pay::patterns;
using namespace pay::service;

static void line() { std::cout << "------------------------------------------------\n"; }
static void title(const char* t) { std::cout << "\n### " << t << " ###\n"; }

int main() {
    std::cout << "=== Payment System :: End-to-End Demo (Phases 1-5) ===\n";

    // --- Compose the application: adapters, patterns, service --------------
    // (In a real app a DI container / factory does this "composition root".)
    InMemoryAccountRepository     accounts;
    InMemoryTransactionRepository transactions;
    InMemoryIdempotencyStore      idempotency;
    InMemoryDistributedLock       locks;
    InMemoryLedgerRepository      ledger;
    InMemoryEventPublisher        events;
    PaymentStrategyFactory        strategies;

    // Wire the validation pipeline: cheap checks first, fraud last.
    ValidationChain validation;
    validation
        .add(std::make_shared<BasicValidationHandler>())
        .add(std::make_shared<LimitValidationHandler>(Money::fromMajor(100000, Currency::INR)))
        .add(std::make_shared<FraudCheckHandler>(Money::fromMajor(50000, Currency::INR)));

    // A subscriber that reacts to domain events (stands in for a notification /
    // analytics consumer that would live behind Kafka in production).
    events.subscribe([](const DomainEvent& e) {
        std::cout << "  [event] " << e.type << " for " << e.aggregateId
                  << " " << e.payload << "\n";
    });

    PaymentService service(accounts, transactions, idempotency, locks, ledger,
                           events, strategies, validation);

    // --- Seed accounts ------------------------------------------------------
    auto user     = std::make_shared<Account>("acc_user_42", AccountType::USER_WALLET,
                                               Money::fromMajor(2000, Currency::INR));
    auto merchant = std::make_shared<Account>("acc_merchant_7", AccountType::MERCHANT,
                                               Money::zero(Currency::INR));
    accounts.save(user);
    accounts.save(merchant);

    // ======================================================================
    title("1. Happy path — a UPI payment of 500 INR");
    line();
    {
        PaymentRequest req{"idem-key-001", user->id(), merchant->id(),
                           Money::fromMajor(500, Currency::INR), PaymentMethod::UPI};
        auto res = service.pay(req);
        std::cout << (res.isOk()
            ? "  OK  txn=" + res.value().id() + " status=" + std::string(toString(res.value().status()))
            : "  ERR " + res.error().message) << "\n";
        std::cout << "  payer balance:    " << user->balance() << "\n";
        std::cout << "  merchant balance: " << merchant->balance() << "\n";
    }

    // ======================================================================
    title("2. Idempotency — retry the SAME key, must NOT charge twice");
    line();
    {
        PaymentRequest retry{"idem-key-001", user->id(), merchant->id(),
                             Money::fromMajor(500, Currency::INR), PaymentMethod::UPI};
        auto res = service.pay(retry);
        std::cout << "  replayed txn: " << (res.isOk() ? res.value().id() : res.error().message) << "\n";
        std::cout << "  payer balance UNCHANGED: " << user->balance() << "\n";
    }

    // ======================================================================
    title("3. Insufficient funds — debit larger than balance is rejected");
    line();
    {
        PaymentRequest big{"idem-key-002", user->id(), merchant->id(),
                           Money::fromMajor(999999, Currency::INR), PaymentMethod::CARD};
        auto res = service.pay(big);
        std::cout << "  result: " << (res.isOk() ? "OK" : res.error().message) << "\n";
    }

    // ======================================================================
    title("4. Fraud rejection — amount over the review threshold is held");
    line();
    {
        PaymentRequest fraud{"idem-key-003", user->id(), merchant->id(),
                             Money::fromMajor(60000, Currency::INR), PaymentMethod::CARD};
        auto res = service.pay(fraud);
        std::cout << "  result: " << (res.isOk() ? "OK" : res.error().message) << "\n";
    }

    // ======================================================================
    title("5. Concurrency — 50 parallel 100-INR debits, no double-spend");
    line();
    {
        // Fresh wallet with exactly enough for 12 payments of 100 INR.
        auto wallet = std::make_shared<Account>("acc_stress", AccountType::USER_WALLET,
                                                Money::fromMajor(1200, Currency::INR));
        accounts.save(wallet);

        pay::infra::ThreadPool pool(8);
        std::atomic<int> ok{0}, rejected{0};
        std::vector<std::future<void>> futures;

        // 50 threads race to spend from the SAME account. The per-account
        // distributed lock serializes them; exactly 12 succeed and the rest are
        // cleanly rejected for insufficient funds — the balance never goes negative.
        for (int i = 0; i < 50; ++i) {
            futures.push_back(pool.submit([&, i] {
                PaymentRequest r{"stress-" + std::to_string(i), wallet->id(),
                                 merchant->id(), Money::fromMajor(100, Currency::INR),
                                 PaymentMethod::WALLET};
                auto res = service.pay(r);
                (res.isOk() ? ok : rejected).fetch_add(1);
            }));
        }
        for (auto& f : futures) f.get();

        std::cout << "  succeeded: " << ok.load() << "  rejected: " << rejected.load() << "\n";
        std::cout << "  final wallet balance: " << wallet->balance()
                  << "  (never negative)\n";
    }

    // ======================================================================
    title("6. Ledger — double-entry journal for the merchant (audit trail)");
    line();
    {
        auto entries = ledger.entriesFor(merchant->id());
        std::cout << "  " << entries.size() << " credit entries recorded:\n";
        for (const auto& e : entries)
            std::cout << "    seq=" << e.seq() << " " << toString(e.direction())
                      << " " << e.amount() << " (txn " << e.txnId() << ")\n";
    }

    // ======================================================================
    title("7. Durable adapter — same port, now persisted to disk");
    line();
    {
        // FileTransactionRepository implements the SAME TransactionRepository
        // port as the in-memory one — so it could be dropped straight into the
        // PaymentService above with a one-line change. Here we show the payoff
        // that in-memory storage can't give: survival across a process restart.
        const std::string dbPath = "build/transactions.db";

        std::string savedId;
        {
            FileTransactionRepository repo(dbPath);          // "process 1"
            Transaction t = Transaction::Builder()
                .id("txn_persist_1").idempotencyKey("persist-key")
                .payer(user->id()).payee(merchant->id())
                .amount(Money::fromMajor(750, Currency::INR))
                .method(PaymentMethod::CARD).build();
            t.setStatus(TxnStatus::CAPTURED);
            repo.save(t);                                    // written to disk
            savedId = t.id();
            std::cout << "  saved " << savedId << " to " << dbPath << "\n";
        }
        {
            FileTransactionRepository repo(dbPath);          // "process 2" (restart)
            auto loaded = repo.find(savedId);
            std::cout << "  after restart, reloaded from disk: "
                      << (loaded ? loaded->id() + " (" + std::string(toString(loaded->status())) +
                                   ", " + loaded->amount().toString() + ")"
                                 : std::string("NOT FOUND"))
                      << "\n";
        }
    }

    line();
    std::cout << "Demo complete. See docs/HLD.md, docs/LLD.md, docs/INTERVIEW_QA.md.\n";
    return 0;
}
