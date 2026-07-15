# High-Level Design (HLD) — Payment System

This document describes the system at the **architecture level**: components,
data flow, and the design decisions an interviewer will probe. It is grounded in
the actual code in this repo — every box below maps to a real file.

---

## 1. Problem statement

Build a payment system that can **move money between accounts** reliably, where
"reliably" means, non-negotiably:

1. **No double charge** — a client retry must not debit the payer twice.
2. **No double spend** — concurrent requests can't overdraw a balance.
3. **No lost or created money** — every movement is conserved and auditable.
4. **Correct lifecycle** — a transaction can't jump to an illegal state.

Everything else (throughput, methods, providers) is secondary to those four.

---

## 2. Architecture — Hexagonal (Ports & Adapters)

```
                       ┌─────────────────────────────────────────┐
   HTTP / gRPC  ──▶    │              PaymentService              │
   (driving side)      │   (use-case orchestration, no infra)    │
                       └───────────────┬─────────────────────────┘
                                       │ depends only on PORTS (interfaces)
     ┌───────────────┬─────────────────┼────────────────┬──────────────┐
     ▼               ▼                 ▼                ▼              ▼
AccountRepo   TransactionRepo   IdempotencyStore  DistributedLock  LedgerRepo / EventPublisher
     │               │                 │                │              │
     ▼               ▼                 ▼                ▼              ▼      (driven side)
 InMemory /     InMemory /         InMemory /       InMemory /     InMemory /
 Postgres       Postgres           Redis            Redis Redlock  Postgres / Kafka
   (ADAPTERS — swappable, chosen at the composition root in main.cpp)
```

**Why hexagonal?** The core domain and `PaymentService` depend on **interfaces**
(ports), never on a database or a broker. Concrete technology lives in
**adapters** behind those ports. Benefits:

- The whole system **compiles and runs today** on in-memory adapters — no infra
  to install, fast tests, deterministic demos.
- Swapping in Postgres/Redis/Kafka means **writing a new adapter**, not editing
  business logic (Dependency Inversion Principle).
- It's the clearest possible interview signal for "I understand decoupling."

---

## 3. Components

| Component | Responsibility | Code |
|---|---|---|
| **Money** | Value object; integer minor units, currency-safe, immutable | `common/Money.hpp` |
| **Account** | Entity with a mutex-guarded balance | `domain/Account.hpp` |
| **Transaction** | Aggregate root; built via Builder | `domain/Transaction.hpp` |
| **LedgerEntry** | Immutable double-entry posting | `domain/LedgerEntry.hpp` |
| **PaymentStrategy** | One impl per method (Strategy) | `patterns/PaymentStrategy.hpp` |
| **StateMachine** | Legal lifecycle transitions only | `patterns/TransactionStateMachine.hpp` |
| **ValidationChain** | Validation → limit → fraud pipeline | `patterns/ValidationChain.hpp` |
| **Ports** | Repository/Cache/Lock/Ledger/Event interfaces | `ports/Ports.hpp` |
| **Adapters** | In-memory implementations of every port | `adapters/*` |
| **ThreadPool** | Bounded parallel task execution | `infra/ThreadPool.hpp` |
| **PaymentService** | Orchestrates the whole use case | `service/PaymentService.hpp` |

---

## 4. Payment flow (happy path)

```
Client ─▶ PaymentService.pay(req)
  1. Idempotency: putIfAbsent(key, txnId)         ── first writer wins
  2. Acquire per-account distributed lock          ── serialize this payer
  3. ValidationChain.run(txn)                       ── validate / limit / fraud
  4. AccountRepo.find(payer/payee)
  5. Strategy.authorize(txn)         ── INITIATED → AUTHORIZED
  6. payer.debit / payee.credit      ── guarded, double-spend safe
  7. LedgerRepo.post([DEBIT, CREDIT]) ── must net to ZERO (double-entry)
  8. Strategy.capture               ── AUTHORIZED → CAPTURED
  9. TransactionRepo.save + EventPublisher.publish("PaymentCaptured")
 10. release lock (RAII)
```

If any step fails, the transaction is moved to **FAILED**, persisted, and a
`PaymentFailed` event is emitted. Balances are compensated if the ledger rejects
a batch, keeping live balances consistent with the journal.

---

## 5. The four guarantees, and how each is met

| Guarantee | Mechanism | Where |
|---|---|---|
| **Idempotency** | `putIfAbsent(key)` claims the request atomically; a retry replays the original txn | `PaymentService::pay` step 1 |
| **Double-spend** | Per-account distributed lock serializes concurrent payers; guarded `debit` rejects overdraft | steps 2 & 6 |
| **Conservation** | Ledger rejects any posting whose signed amounts don't sum to zero | `InMemoryLedgerRepository::post` |
| **State integrity** | All status changes go through the transition table | `TransactionStateMachine` |

---

## 6. Scaling to production (what changes, what doesn't)

The **ports don't change** — only adapters and deployment do:

- **AccountRepository / TransactionRepository → PostgreSQL.** Use
  `SELECT ... FOR UPDATE` row locks (or optimistic `version` columns) so the
  double-spend guard survives across processes, not just threads.
- **IdempotencyStore → Redis.** `SET key val NX EX <ttl>` gives the same
  atomic "first writer wins" with automatic expiry.
- **DistributedLock → Redis Redlock / Postgres advisory lock.** Same
  `acquire(key)` shape; now correct across many service instances.
- **EventPublisher → Kafka.** Publish `PaymentCaptured`/`PaymentFailed`;
  notification, analytics, and payout become independent consumers.
- **LedgerRepository → Postgres**, all entries of a posting written in **one DB
  transaction** for atomic durability.
- **IdGenerator → Snowflake / UUIDv7** so ids are unique across instances.

Cross-cutting: horizontal scale behind a load balancer, the ThreadPool for
off-critical-path work (webhooks, settlement), and the **outbox pattern** to
publish events atomically with the DB write.

---

## 7. Trade-offs deliberately taken

- **In-memory adapters** ship first so the design is provable without infra.
  Cost: not durable. Mitigation: ports make the swap a localized change.
- **Synchronous event dispatch** in the demo keeps output readable; production
  hands publishing to the broker/pool so it's off the critical path.
- **Lock-per-account (striping)** maximizes parallelism (different accounts run
  concurrently) at the cost of a small map of locks. A single global lock would
  be simpler but would serialize the entire system.

See [LLD.md](LLD.md) for class-level detail and [INTERVIEW_QA.md](INTERVIEW_QA.md)
for the questions this design is built to answer.
