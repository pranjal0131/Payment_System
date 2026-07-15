//
// tests.cpp — a lightweight, zero-dependency test suite.
//
// No gtest/catch2 to install on MinGW — just a tiny CHECK macro that counts
// pass/fail and a main() that returns non-zero if anything failed (so CI / the
// build script can gate on it). Each test asserts ONE behaviour and names the
// invariant it protects. These are the tests that make the resume claim honest.
//
// Build & run:
//   powershell -ExecutionPolicy Bypass -File .\run-tests.ps1
//
#include <atomic>
#include <iostream>
#include <string>
#include <vector>
#include <future>

#include "payment/common/Money.hpp"
#include "payment/domain/Account.hpp"
#include "payment/domain/Transaction.hpp"

#include "payment/patterns/TransactionStateMachine.hpp"
#include "payment/patterns/ValidationChain.hpp"
#include "payment/patterns/PaymentStrategyFactory.hpp"

#include "payment/adapters/InMemoryAccountRepository.hpp"
#include "payment/adapters/InMemoryTransactionRepository.hpp"
#include "payment/adapters/InMemoryIdempotencyStore.hpp"
#include "payment/adapters/InMemoryDistributedLock.hpp"
#include "payment/adapters/InMemoryEventPublisher.hpp"
#include "payment/adapters/InMemoryLedgerRepository.hpp"
#include "payment/adapters/FileTransactionRepository.hpp"

#include "payment/infra/ThreadPool.hpp"
#include "payment/service/PaymentService.hpp"

using namespace pay::common;
using namespace pay::domain;
using namespace pay::patterns;
using namespace pay::adapters;
using namespace pay::service;

// ---- tiny test harness -----------------------------------------------------
static int g_pass = 0;
static int g_fail = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (cond) { ++g_pass; }                                                \
        else { ++g_fail; std::cout << "  FAIL [line " << __LINE__ << "]: "     \
                                   << #cond << "\n"; }                         \
    } while (0)

static void section(const char* name) { std::cout << "\n[" << name << "]\n"; }

// Helper: assemble a PaymentService over a given transaction repo (lets us reuse
// the same wiring for both in-memory and file-backed persistence tests).
struct Harness {
    InMemoryAccountRepository     accounts;
    InMemoryIdempotencyStore      idempotency;
    InMemoryDistributedLock       locks;
    InMemoryLedgerRepository      ledger;
    InMemoryEventPublisher        events;
    PaymentStrategyFactory        strategies;
    ValidationChain               validation;
    pay::ports::TransactionRepository& txns;
    PaymentService                service;

    explicit Harness(pay::ports::TransactionRepository& txnRepo)
        : txns(txnRepo),
          service(accounts, txns, idempotency, locks, ledger, events,
                  strategies, validation) {
        validation
            .add(std::make_shared<BasicValidationHandler>())
            .add(std::make_shared<LimitValidationHandler>(Money::fromMajor(100000, Currency::INR)))
            .add(std::make_shared<FraudCheckHandler>(Money::fromMajor(50000, Currency::INR)));
    }
};

// ===========================================================================
static void test_money() {
    section("Money — integer minor units, currency safety");
    // fromMajor rounds to the nearest minor unit; NEVER stored as a float.
    CHECK(Money::fromMajor(1.50, Currency::INR).minorUnits() == 150);
    CHECK(Money::fromMajor(0.1, Currency::INR).minorUnits() == 10);
    // Arithmetic stays exact.
    Money a = Money::fromMinor(10, Currency::INR);
    Money b = Money::fromMinor(20, Currency::INR);
    CHECK((a + b).minorUnits() == 30);
    CHECK((b - a).minorUnits() == 10);
    // Comparisons.
    CHECK(a < b);
    CHECK(b > a);
    // Mixing currencies is a programmer error -> throws.
    bool threw = false;
    try { (void)(a + Money::fromMinor(5, Currency::USD)); }
    catch (const std::exception&) { threw = true; }
    CHECK(threw);
}

static void test_account() {
    section("Account — guarded debit rejects overdraft");
    Account acc("a1", AccountType::USER_WALLET, Money::fromMajor(100, Currency::INR));
    CHECK(acc.debit(Money::fromMajor(60, Currency::INR)).isOk());
    CHECK(acc.balance().minorUnits() == 4000);                 // 40.00 left
    // Overdraft is rejected with a typed error, balance untouched.
    Status od = acc.debit(Money::fromMajor(50, Currency::INR));
    CHECK(od.isErr());
    CHECK(od.error().code == ErrorCode::INSUFFICIENT_FUNDS);
    CHECK(acc.balance().minorUnits() == 4000);
    // Credit adds funds.
    CHECK(acc.credit(Money::fromMajor(10, Currency::INR)).isOk());
    CHECK(acc.balance().minorUnits() == 5000);
}

static void test_account_concurrency() {
    section("Account — concurrent debits never double-spend");
    // 100 threads each try to debit 1 from a balance of exactly 40.
    Account acc("a2", AccountType::USER_WALLET, Money::fromMajor(40, Currency::INR));
    pay::infra::ThreadPool pool(8);
    std::atomic<int> ok{0};
    std::vector<std::future<void>> fs;
    for (int i = 0; i < 100; ++i)
        fs.push_back(pool.submit([&] {
            if (acc.debit(Money::fromMajor(1, Currency::INR)).isOk()) ok.fetch_add(1);
        }));
    for (auto& f : fs) f.get();
    CHECK(ok.load() == 40);                       // exactly 40 succeed
    CHECK(acc.balance().isZero());                // balance never goes negative
}

static void test_state_machine() {
    section("StateMachine — only legal transitions allowed");
    TransactionStateMachine sm;
    CHECK(sm.canTransition(TxnStatus::INITIATED, TxnStatus::AUTHORIZED));
    CHECK(sm.canTransition(TxnStatus::AUTHORIZED, TxnStatus::CAPTURED));
    CHECK(sm.canTransition(TxnStatus::CAPTURED, TxnStatus::SETTLED));
    // Illegal jumps are rejected.
    CHECK(!sm.canTransition(TxnStatus::SETTLED, TxnStatus::INITIATED));
    CHECK(!sm.canTransition(TxnStatus::INITIATED, TxnStatus::CAPTURED));
    CHECK(sm.transition(TxnStatus::SETTLED, TxnStatus::AUTHORIZED).error().code
          == ErrorCode::INVALID_STATE_TRANSITION);
    // Terminal states have no exits.
    CHECK(sm.isTerminal(TxnStatus::SETTLED));
    CHECK(sm.isTerminal(TxnStatus::FAILED));
    CHECK(!sm.isTerminal(TxnStatus::INITIATED));
}

static Transaction makeTxn(const std::string& id, Money amt,
                           const std::string& payer = "p", const std::string& payee = "m") {
    return Transaction::Builder().id(id).idempotencyKey("k-" + id)
        .payer(payer).payee(payee).amount(amt).method(PaymentMethod::UPI).build();
}

static void test_validation_chain() {
    section("ValidationChain — validation / limit / fraud pipeline");
    ValidationChain chain;
    chain.add(std::make_shared<BasicValidationHandler>())
         .add(std::make_shared<LimitValidationHandler>(Money::fromMajor(1000, Currency::INR)))
         .add(std::make_shared<FraudCheckHandler>(Money::fromMajor(500, Currency::INR)));

    CHECK(chain.run(makeTxn("t1", Money::fromMajor(100, Currency::INR))).isOk());
    // Zero amount fails basic validation.
    CHECK(chain.run(makeTxn("t2", Money::zero(Currency::INR))).error().code
          == ErrorCode::VALIDATION_FAILED);
    // payer == payee fails basic validation.
    CHECK(chain.run(makeTxn("t3", Money::fromMajor(1, Currency::INR), "x", "x")).error().code
          == ErrorCode::VALIDATION_FAILED);
    // Over the fraud threshold is held.
    CHECK(chain.run(makeTxn("t4", Money::fromMajor(600, Currency::INR))).error().code
          == ErrorCode::FRAUD_REJECTED);
}

static void test_idempotency_store() {
    section("IdempotencyStore — putIfAbsent is atomic first-writer-wins");
    InMemoryIdempotencyStore store;
    CHECK(store.putIfAbsent("k", "txn_1") == true);    // first writer wins
    CHECK(store.putIfAbsent("k", "txn_2") == false);   // second is rejected
    CHECK(store.get("k").value() == "txn_1");          // original preserved
}

static void test_ledger_balancing() {
    section("Ledger — rejects any posting that doesn't net to zero");
    InMemoryLedgerRepository ledger;
    Money amt = Money::fromMajor(100, Currency::INR);
    // Balanced: DEBIT payer + CREDIT payee = 0 net.
    std::vector<LedgerEntry> balanced = {
        LedgerEntry("e1", "t1", "payer", Direction::DEBIT,  amt, 1),
        LedgerEntry("e2", "t1", "payee", Direction::CREDIT, amt, 2),
    };
    CHECK(ledger.post(balanced).isOk());
    // Unbalanced: two credits, no matching debit -> money conjured -> rejected.
    std::vector<LedgerEntry> unbalanced = {
        LedgerEntry("e3", "t2", "payer", Direction::CREDIT, amt, 3),
        LedgerEntry("e4", "t2", "payee", Direction::CREDIT, amt, 4),
    };
    CHECK(ledger.post(unbalanced).isErr());
    // Mixed currencies in one posting -> rejected.
    std::vector<LedgerEntry> mixed = {
        LedgerEntry("e5", "t3", "payer", Direction::DEBIT,  Money::fromMajor(1, Currency::INR), 5),
        LedgerEntry("e6", "t3", "payee", Direction::CREDIT, Money::fromMajor(1, Currency::USD), 6),
    };
    CHECK(ledger.post(mixed).isErr());
}

static void test_service_end_to_end() {
    section("PaymentService — happy path, idempotency, insufficient funds");
    InMemoryTransactionRepository txns;
    Harness h(txns);
    auto user = std::make_shared<Account>("u", AccountType::USER_WALLET, Money::fromMajor(1000, Currency::INR));
    auto merc = std::make_shared<Account>("m", AccountType::MERCHANT,    Money::zero(Currency::INR));
    h.accounts.save(user);
    h.accounts.save(merc);

    // Happy path.
    PaymentRequest req{"key-1", "u", "m", Money::fromMajor(300, Currency::INR), PaymentMethod::UPI};
    auto r1 = h.service.pay(req);
    CHECK(r1.isOk());
    CHECK(r1.value().status() == TxnStatus::CAPTURED);
    CHECK(user->balance().minorUnits() == 70000);   // 700.00 left
    CHECK(merc->balance().minorUnits() == 30000);

    // Idempotent retry with the SAME key: replays original, no second charge.
    auto r2 = h.service.pay(req);
    CHECK(r2.isOk());
    CHECK(r2.value().id() == r1.value().id());       // same transaction
    CHECK(user->balance().minorUnits() == 70000);    // balance UNCHANGED

    // Insufficient funds is rejected cleanly.
    PaymentRequest big{"key-2", "u", "m", Money::fromMajor(5000, Currency::INR), PaymentMethod::CARD};
    CHECK(h.service.pay(big).isErr());
    CHECK(user->balance().minorUnits() == 70000);    // failed txn didn't move money
}

static void test_service_concurrency() {
    section("PaymentService — 200 parallel payments, no overdraft");
    InMemoryTransactionRepository txns;
    Harness h(txns);
    // Wallet holds exactly enough for 30 payments of 10.
    auto wallet = std::make_shared<Account>("w", AccountType::USER_WALLET, Money::fromMajor(300, Currency::INR));
    auto merc   = std::make_shared<Account>("m", AccountType::MERCHANT,    Money::zero(Currency::INR));
    h.accounts.save(wallet);
    h.accounts.save(merc);

    pay::infra::ThreadPool pool(8);
    std::atomic<int> ok{0};
    std::vector<std::future<void>> fs;
    for (int i = 0; i < 200; ++i)
        fs.push_back(pool.submit([&, i] {
            PaymentRequest r{"c-" + std::to_string(i), "w", "m",
                             Money::fromMajor(10, Currency::INR), PaymentMethod::WALLET};
            if (h.service.pay(r).isOk()) ok.fetch_add(1);
        }));
    for (auto& f : fs) f.get();

    CHECK(ok.load() == 30);                          // exactly 30 succeed
    CHECK(wallet->balance().isZero());               // never negative
    CHECK(merc->balance().minorUnits() == 30000);    // merchant got exactly 300
}

static void test_file_persistence() {
    section("FileTransactionRepository — durable across a 'restart'");
    const std::string path = "build/test_txns.db";
    std::remove(path.c_str());   // start clean

    Transaction t = makeTxn("txn_persist", Money::fromMajor(42, Currency::INR));
    t.setStatus(TxnStatus::CAPTURED);

    // First instance writes to disk...
    {
        FileTransactionRepository repo(path);
        repo.save(t);
        CHECK(repo.size() == 1);
    }
    // ...a SECOND instance (simulating a process restart) reloads from the file.
    {
        FileTransactionRepository repo(path);
        auto loaded = repo.find("txn_persist");
        CHECK(loaded.has_value());
        CHECK(loaded->amount().minorUnits() == 4200);
        CHECK(loaded->status() == TxnStatus::CAPTURED);       // status survived
        CHECK(loaded->idempotencyKey() == "k-txn_persist");   // key survived
    }
    std::remove(path.c_str());
}

int main() {
    std::cout << "=== Payment System :: Test Suite ===\n";

    test_money();
    test_account();
    test_account_concurrency();
    test_state_machine();
    test_validation_chain();
    test_idempotency_store();
    test_ledger_balancing();
    test_service_end_to_end();
    test_service_concurrency();
    test_file_persistence();

    std::cout << "\n------------------------------------------------\n";
    std::cout << "Passed: " << g_pass << "   Failed: " << g_fail << "\n";
    if (g_fail == 0) std::cout << "ALL TESTS GREEN.\n";
    else             std::cout << "SOME TESTS FAILED.\n";
    return g_fail == 0 ? 0 : 1;
}
