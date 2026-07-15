# Low-Level Design (LLD) — Payment System

Class-level design: the objects, their responsibilities, the patterns they
embody, and the C++ techniques used. Every section names the file so you can
read the code alongside.

---

## 1. Layering (dependency direction)

```
common/   ◀── domain/   ◀── patterns/ ◀── service/
   ▲           ▲              ▲            │
   └───────────┴── ports/ ◀───┴────────────┘
                     ▲
                 adapters/   (implement ports; depend inward only)
```

**Rule:** dependencies point **inward**. `common` knows nothing about anything;
`service` knows the ports but not the adapters. `main.cpp` is the *composition
root* — the only place that knows concrete adapter types and wires them together.

---

## 2. `common/` — shared kernel

### Money (`Money.hpp`) — Value Object
- **Immutable**: no setters; arithmetic returns new objects → inherently thread-safe.
- **No identity**: compared by `(minorUnits, currency)`.
- **Integer minor units** (paise/cents), never `double`. `0.1 + 0.2 != 0.3` in
  floating point is real lost money; we never risk it.
- **Currency-safe**: mixing currencies throws — a programmer error, not a
  business outcome.

### Result<T> / Status (`Result.hpp`) — explicit error handling
- **Expected** business failures (insufficient funds, fraud, duplicate) are
  **return values**, not exceptions. They are part of the API contract.
- Exceptions are reserved for **programmer errors** (e.g. currency mismatch).
- `ErrorCode` is machine-readable (for metrics/alerting); `message` is for humans.
- Same idea as Rust `Result<T,E>` / C++23 `std::expected`, hand-rolled for C++20.

### Enums (`Enums.hpp`) — `enum class` everywhere
Scoped enums give type safety (can't pass a `Currency` where a `TxnStatus` is
expected) and no implicit int conversion. Each has a `toString`.

---

## 3. `domain/` — entities & aggregates

### Account (`Account.hpp`) — Entity
- **Identity** = `accountId`; **mutable** balance.
- `debit`/`credit` are **mutex-guarded** (`std::lock_guard`). The single guarded
  check-then-act (`if (amount > balance) reject; else subtract`) is what prevents
  **in-process** double-spend — without it two threads both read 100, both
  subtract 100, and 200 leaves a 100 balance ("lost update").
- `balance()` locks too; `mtx_` is `mutable` so a `const` read can lock.

### Transaction (`Transaction.hpp`) — Aggregate root + Builder
- Many fields → a 7-arg constructor is unreadable and error-prone. The **Builder**
  gives fluent construction and validates required fields in `build()`.
- Private constructor forces construction through the Builder.

### LedgerEntry (`LedgerEntry.hpp`) — immutable posting
- One line of the **double-entry** journal. `signedAmount()` returns +amount for
  CREDIT, −amount for DEBIT so a balanced posting sums to zero.

---

## 4. `patterns/` — the design patterns

### Strategy + Factory (`PaymentStrategy.hpp`, `PaymentStrategyFactory.hpp`)
- **Strategy**: one interface (`authorize/capture/refund`), one class per method
  (Card/UPI/Wallet/NetBanking). Adding AMEX/BNPL = add a class, edit nothing.
  This kills the `switch(method)` that would otherwise spread through the service.
- **Factory**: centralizes "which concrete strategy for this method". Strategies
  are stateless, so it hands back one shared instance (flyweight reuse).

### State Machine (`TransactionStateMachine.hpp`) — State pattern as a table
- A single **transition table** is the source of truth for legal moves.
- `transition(from,to)` returns a typed error on an illegal jump — the service
  never corrupts the lifecycle. Terminal states map to an empty set.

### Chain of Responsibility (`ValidationChain.hpp`)
- The validation pipeline is a list of handlers (`BasicValidation → Limit →
  Fraud`). Each returns `ok()` to continue or an error to short-circuit.
- **Order matters and is explicit**: cheap checks first, expensive fraud last.
- New checks are inserted as links; existing handlers don't change.

---

## 5. `infra/` — concurrency primitives

### IdGenerator (`IdGenerator.hpp`)
- `std::atomic<uint64_t>` counter; `fetch_add` is a single indivisible op → no
  mutex, no contention, no duplicate ids across threads.

### BlockingQueue (`BlockingQueue.hpp`) — Producer/Consumer
- `std::mutex` (mutual exclusion) + two `std::condition_variable`s (sleep instead
  of busy-wait) + a `closed_` flag (clean shutdown).
- **Bounded** capacity → **backpressure**: producers block when full instead of
  letting the queue grow unbounded and OOM the process.
- `pop()` returns `nullopt` only when closed **and** drained → the signal for a
  worker loop to exit.

### ThreadPool (`ThreadPool.hpp`)
- N workers created **once**; tasks fed from the BlockingQueue. Caps concurrency
  and amortizes thread-creation cost vs. thread-per-request.
- `submit()` wraps the callable in a `std::packaged_task` and returns a
  `std::future<R>` — exceptions thrown on a worker are captured and re-thrown at
  `future.get()` instead of calling `std::terminate`.
- Destructor closes the queue, workers drain, then `join()` — graceful shutdown.

---

## 6. `ports/` & `adapters/` — hexagon boundary

Each port is a narrow interface (Interface Segregation). Notable ones:

- **IdempotencyStore.putIfAbsent** — *atomic* check-and-set. Mirrors Redis
  `SET key val NX`. Non-atomic would let two retries both proceed.
- **DistributedLock.acquire** — returns an **RAII Guard**; lock released when the
  guard drops. In-memory it's a per-key `std::mutex` (lock striping); production
  swaps Redlock/advisory lock behind the same shape.
- **LedgerRepository.post** — must **reject** any batch whose signed amounts
  don't net to zero. Enforced in the adapter so no caller can bypass it.

Adapters (`adapters/InMemory*`) are all `final`, mutex-guarded, and store data in
hash maps / vectors. They are the only classes that would be rewritten for real
infra.

---

## 7. `service/PaymentService` — orchestration

- **Constructor injection** of every port + pattern collaborator → depends on
  interfaces only, fully unit-testable with fakes.
- `pay()` is the one use case; the four guarantees are labelled `[1]`–`[4]`
  inline in the code.
- `advance()` routes every status change through the state machine; `fail()`
  centralizes the FAILED-path (persist + event + typed error).
- The per-account lock guard is an RAII local, so it releases on **every** exit
  path, including early error returns.

---

## 8. C++ / OOP techniques on display

| Technique | Where |
|---|---|
| RAII (locks, thread joining, pool shutdown) | `Account`, `DistributedLock`, `ThreadPool` |
| `std::atomic` lock-free counters | `IdGenerator`, `PaymentService::seq_` |
| `condition_variable` producer/consumer | `BlockingQueue` |
| `std::future` / `packaged_task` | `ThreadPool::submit` |
| `std::variant`-based `Result<T>` | `Result.hpp` |
| Pure-virtual ports + `final` adapters | `ports/`, `adapters/` |
| Builder, Strategy, Factory, State, Chain, Repository, Observer | see `patterns/`, `ports/` |
| Immutability / value objects | `Money`, `LedgerEntry` |
| Dependency injection / composition root | `PaymentService`, `main.cpp` |

SOLID: **S** small single-purpose classes · **O** add methods/handlers without
editing existing code · **L** adapters substitute for ports · **I** narrow ports
· **D** service depends on abstractions.
