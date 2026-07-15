#pragma once
//
// TransactionStateMachine.hpp — the STATE pattern (as an explicit transition table).
//
// LLD lesson: a transaction has a lifecycle, and only *some* transitions are
// legal. INITIATED -> AUTHORIZED is fine; SETTLED -> INITIATED is nonsense and,
// in a payment system, a would-be double-settlement or fraud. Scattering
// `if (status == X) status = Y` around the codebase means every call site can
// silently make an illegal jump.
//
// We centralize the rules in ONE place: a transition table. Every state change
// must go through `transition()`, which rejects illegal moves with a typed error
// instead of corrupting state. This is the defensive core of txn integrity.
//
//   INITIATED  ─▶ AUTHORIZED ─▶ CAPTURED ─▶ SETTLED
//        │             │            │
//        └─▶ FAILED    └─▶ FAILED   └─▶ REFUNDED
//
#include <initializer_list>
#include <unordered_map>
#include <unordered_set>

#include "payment/common/Enums.hpp"
#include "payment/common/Result.hpp"

namespace pay::patterns {

using common::TxnStatus;
using common::Status;
using common::ErrorCode;

class TransactionStateMachine {
public:
    TransactionStateMachine() {
        // Adjacency list of allowed transitions. The single source of truth for
        // "what can happen next". Terminal states (SETTLED, FAILED, REFUNDED)
        // map to an empty set — nothing follows them.
        allowed_ = {
            {TxnStatus::INITIATED,  {TxnStatus::AUTHORIZED, TxnStatus::FAILED}},
            {TxnStatus::AUTHORIZED, {TxnStatus::CAPTURED,   TxnStatus::FAILED}},
            {TxnStatus::CAPTURED,   {TxnStatus::SETTLED,    TxnStatus::REFUNDED}},
            {TxnStatus::SETTLED,    {}},
            {TxnStatus::FAILED,     {}},
            {TxnStatus::REFUNDED,   {}},
        };
    }

    // Is moving from -> to a legal transition?
    bool canTransition(TxnStatus from, TxnStatus to) const {
        auto it = allowed_.find(from);
        return it != allowed_.end() && it->second.count(to) > 0;
    }

    // Validate a transition. Returns ok() if legal, a typed error otherwise.
    // The service applies the new status only when this succeeds.
    Status transition(TxnStatus from, TxnStatus to) const {
        if (!canTransition(from, to)) {
            return Status::fail(
                ErrorCode::INVALID_STATE_TRANSITION,
                std::string("illegal transition ") + std::string(common::toString(from)) +
                " -> " + std::string(common::toString(to)));
        }
        return Status::ok();
    }

    // A terminal state can never transition again — useful for guarding retries.
    bool isTerminal(TxnStatus s) const {
        auto it = allowed_.find(s);
        return it != allowed_.end() && it->second.empty();
    }

private:
    std::unordered_map<TxnStatus, std::unordered_set<TxnStatus>> allowed_;
};

} // namespace pay::patterns
