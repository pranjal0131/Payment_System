# Payment System (C++) — HLD/LLD Learning Project

A production-style payment system built **incrementally** to master High-Level
Design (HLD) and Low-Level Design (LLD) for interviews. Every file is heavily
commented to explain *why*, not just *what*.

**Tech direction:** C++20 · PostgreSQL · Redis · Kafka · RabbitMQ ·
multithreading/concurrency · full OOP · classic design patterns.

> **Architecture choice — Hexagonal (Ports & Adapters).**
> The core domain depends only on **interfaces** (ports). Infrastructure
> (Postgres/Redis/Kafka/RabbitMQ) lives in **adapters** behind those interfaces.
> This lets the whole project **compile and run today** with in-memory adapters,
> while real infra adapters drop in later without touching business logic.
> *(This decoupling is exactly what interviewers look for.)*

---

## Build & Run

**Option A — quick (no extra tools, uses the MSYS2 g++ already installed):**
```powershell
powershell -ExecutionPolicy Bypass -File .\build.ps1
.\build\payment.exe
```

**Option B — production-standard (after `pacman -S mingw-w64-x86_64-cmake`):**
```powershell
cmake -S . -B build -G "MinGW Makefiles"
cmake --build build
.\build\payment.exe
```

Compiler in use: `C:\msys64\mingw64\bin\g++.exe` (GCC 14, C++20).

**Run the tests** (zero-dependency suite, exits non-zero on failure):
```powershell
powershell -ExecutionPolicy Bypass -File .\run-tests.ps1
```
Covers Money, guarded Account (incl. concurrent no-double-spend), the state
machine, validation chain, idempotency, ledger balancing, the full
`PaymentService` flow (happy / idempotent replay / insufficient funds /
200-thread concurrency), and durable file persistence across a "restart".

---

## Project layout

```
include/payment/
  common/     value objects + shared types (Money, Enums, Result)
  domain/     entities (Account, Transaction)  — pure business, no infra
  patterns/   Strategy, State, Factory, Chain   (added Phase 3)
  ports/      interfaces: repositories, cache, event/message bus (Phase 4)
  infra/      concurrency utils: ThreadPool, queues (Phase 2)
  adapters/   in-memory impls + FileTransactionRepository (durable, disk-backed)
  service/    PaymentService orchestration (Phase 5)
src/          main.cpp end-to-end demo (7 scenarios)
tests/        tests.cpp — zero-dependency assert-based suite
docs/         HLD.md, LLD.md, INTERVIEW_QA.md
```

---

## Roadmap

- [x] **Phase 1 — Core domain.** Money value object (integer minor units, no
      floats), strong enums, `Result<T>`/`Status` error handling, `Account`
      entity (guarded debit/credit = in-process double-spend safety),
      `Transaction` + Builder pattern.
- [x] **Phase 2 — Concurrency.** Lock-free atomic `IdGenerator`, thread-safe
      bounded `BlockingQueue` (producer/consumer + backpressure), `ThreadPool`
      with `std::future` task submission. Parallel payments without races.
- [x] **Phase 3 — Design patterns.** Strategy (Card/UPI/Wallet/NetBanking) +
      Factory, `TransactionStateMachine` (explicit transition table rejecting
      illegal moves), Chain of Responsibility (validation → limit → fraud).
- [x] **Phase 4 — Ports & Adapters.** `AccountRepository`,
      `TransactionRepository`, `IdempotencyStore`, `DistributedLock`,
      `LedgerRepository`, `EventPublisher` interfaces + in-memory adapters.
      Real infra (Postgres/Redis/Kafka) drops in as new adapters, no core change.
- [x] **Phase 5 — Service layer.** `PaymentService` end-to-end: idempotency,
      per-account distributed-lock double-spend prevention, balanced
      double-entry ledger, event publication.
- [x] **Phase 6 — Docs + interview Q&A.** See [docs/HLD.md](docs/HLD.md),
      [docs/LLD.md](docs/LLD.md), [docs/INTERVIEW_QA.md](docs/INTERVIEW_QA.md).

## Concepts demonstrated (interview checklist)

OOP (encapsulation/inheritance/polymorphism/abstraction) · SOLID · value
objects vs entities · immutability · Builder/Strategy/Factory/State/Chain/
Observer/Repository/Singleton/Decorator · thread safety, mutex/condition
variable/atomics, producer-consumer · idempotency · double-spend prevention ·
double-entry accounting · event-driven architecture.
