# Interview Q&A — grounded in this codebase

Answers you can defend because they map to real code here. Each cites the file
that backs it. Practice saying these out loud.

---

## Money & correctness

**Q. Why not store money as a `double`?**
Floating point can't represent most decimals exactly — `0.1 + 0.2 != 0.3`. In a
payment system that rounding error is real money lost or created, and it
compounds. We store the amount as an **integer count of the smallest unit**
(paise/cents) in `Money` (`common/Money.hpp`). All arithmetic is integer; we
never divide into fractional units.

**Q. Value object vs entity?**
A **value object** (`Money`) has no identity, is immutable, and is compared by
value. An **entity** (`Account`, `Transaction`) has a stable identity (an id) and
mutable state, compared by id. Immutability makes value objects trivially
thread-safe.

**Q. Why return `Result<T>` instead of throwing?**
Expected business failures (insufficient funds, fraud, duplicate) are part of the
**contract** and happen constantly, so they're return values (`Result.hpp`).
Exceptions are reserved for **programmer errors** (e.g. mixing currencies). This
keeps the happy path readable and forces callers to handle failure explicitly.

---

## The four hard guarantees

**Q. How do you prevent a double charge on client retries (idempotency)?**
The client sends an **idempotency key**. `PaymentService::pay` calls
`IdempotencyStore.putIfAbsent(key, txnId)` **before doing any work**. It's an
atomic check-and-set (Redis `SET NX` in production). The first request wins; a
retry finds the key taken and **replays the original transaction** instead of
charging again. Demo scenario 2 proves the balance is unchanged on retry.

**Q. How do you prevent double-spend under concurrency?**
Two layers. (1) A **per-account distributed lock** (`DistributedLock.acquire`)
serializes all requests touching the same payer, so their read-modify-write of
the balance can't interleave — different accounts still run in parallel (lock
striping). (2) The guarded `Account::debit` rejects any overdraft under its mutex.
Demo scenario 5: 50 parallel debits of 100 from a 1200 wallet → exactly 12
succeed, balance lands on 0, **never negative**.

**Q. Why a distributed lock *and* a DB row lock in production?**
The in-process mutex only serializes threads in one process. With many service
instances you need cross-process serialization — a Redis lock or Postgres
`SELECT ... FOR UPDATE` / advisory lock. Same `acquire(key)` interface, different
adapter.

**Q. What guarantees money isn't created or destroyed?**
**Double-entry accounting**. Every movement posts a balanced pair —
`DEBIT payer` + `CREDIT payee` — and `LedgerRepository.post` **rejects** any
batch whose signed amounts don't sum to zero (`InMemoryLedgerRepository`). The
journal is append-only and immutable, so it's a perfect audit trail; the balance
is *derived* from it, never the other way around.

**Q. How do you stop illegal status transitions?**
Every status change goes through `TransactionStateMachine`, an explicit
transition table. `SETTLED → INITIATED` isn't in the table, so it's rejected with
`INVALID_STATE_TRANSITION`. No call site can corrupt the lifecycle.

---

## Concurrency

**Q. Why a thread pool instead of a thread per request?**
Thread-per-request is expensive (kernel thread creation, context switches) and
unbounded — a spike spawns thousands of threads and thrashes. `ThreadPool` creates
N workers once and feeds them from a bounded queue, capping concurrency.

**Q. Why is the queue *bounded*?**
Backpressure. An unbounded in-memory queue under a traffic spike grows until it
OOMs the process — an outage. A bounded `BlockingQueue` blocks producers when
full, applying pressure upstream instead.

**Q. mutex vs condition_variable vs atomic — when each?**
- **atomic** for a single lock-free counter (`IdGenerator`): `fetch_add` is one
  indivisible op, no lock needed.
- **mutex** for mutual exclusion over a compound structure (the queue, the
  balance).
- **condition_variable** to *wait* for a state change (queue non-empty) without
  busy-spinning the CPU.

**Q. How does `submit()` propagate an exception from a worker?**
It wraps the task in `std::packaged_task`, which captures any exception into the
returned `std::future`. It's re-thrown at `future.get()` — a throwing task never
crashes the worker (which would `std::terminate`).

---

## Architecture & patterns

**Q. Why hexagonal / ports & adapters?**
The domain and service depend on **interfaces**, not on Postgres/Redis/Kafka.
So the system compiles and runs on in-memory adapters today, and real infra drops
in as **new adapters** without touching business logic (Dependency Inversion).
It's the cleanest way to keep the core testable and technology-agnostic.

**Q. Where does each classic pattern show up?**
Builder (`Transaction`), Strategy + Factory (payment methods), State
(`TransactionStateMachine`), Chain of Responsibility (`ValidationChain`),
Repository (ports), Observer (`EventPublisher`), RAII guard (`DistributedLock`).

**Q. Why Strategy for payment methods instead of a switch?**
A `switch(method)` spreads through the service and must be edited for every new
method (violates Open/Closed). Strategy isolates each method behind one interface;
adding a provider is adding a class. The Factory centralizes selection.

**Q. Why Chain of Responsibility for validation?**
Checks are independent and their **order matters** (cheap first, fraud last).
The chain lets you insert/reorder/disable a check without touching the others,
and each handler is unit-testable in isolation.

---

## Event-driven & scaling

**Q. Why publish events instead of calling downstream services directly?**
Direct calls couple the service to every consumer and make its latency the *sum*
of theirs. Publishing `PaymentCaptured` lets notification/analytics/payout react
**asynchronously** as independent consumers. In production the `EventPublisher`
adapter produces to Kafka.

**Q. How do you publish an event and commit the DB atomically?**
The **outbox pattern**: write the event to an `outbox` table in the *same* DB
transaction as the state change, then a relay publishes it to the broker. Avoids
the "committed but never published" (or vice-versa) gap.

**Q. What breaks first at scale, and how do you fix it?**
The single-instance in-memory locks/stores. Fixes: move `IdempotencyStore` to
Redis, `DistributedLock` to Redlock/advisory lock, repositories to Postgres with
row locks, `IdGenerator` to Snowflake/UUIDv7, events to Kafka — all behind the
existing ports, so the service code is unchanged.

---

## Quick "walk me through a payment" (30-second version)

Reserve the idempotency key → take the per-account lock → run validation/fraud →
authorize via the method strategy → guarded debit/credit → post the balanced
double-entry pair → capture and advance the state machine to CAPTURED → save and
publish `PaymentCaptured` → release the lock (RAII). Any failure → FAILED +
`PaymentFailed` event + typed error, with balances compensated if the ledger
rejects the batch.
