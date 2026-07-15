#pragma once
//
// LedgerEntry.hpp — one immutable line in the double-entry ledger.
//
// LLD lesson: never store "the balance" as the source of truth. Store the
// append-only *history* of postings; the balance is derived by summing them.
// This is double-entry accounting — the 700-year-old model every bank uses.
//
//   * Every transaction produces >= 2 entries that SUM TO ZERO.
//     Payer:  DEBIT  1500 INR   (money leaves)
//     Payee:  CREDIT 1500 INR   (money arrives)
//   * Entries are IMMUTABLE and append-only. You never edit or delete money
//     history — a correction is a new compensating entry. This gives a perfect
//     audit trail and makes reconciliation possible.
//
// We treat DEBIT/CREDIT as signed movements against an account's balance so the
// ledger sums cleanly regardless of account type.
//
#include <cstdint>
#include <string>

#include "payment/common/Money.hpp"
#include "payment/common/Enums.hpp"

namespace pay::domain {

using common::Money;
using common::Direction;

class LedgerEntry {
public:
    LedgerEntry(std::string entryId, std::string txnId, std::string accountId,
                Direction direction, Money amount, std::int64_t seq)
        : entryId_(std::move(entryId)), txnId_(std::move(txnId)),
          accountId_(std::move(accountId)), direction_(direction),
          amount_(amount), seq_(seq) {}

    const std::string& entryId()   const { return entryId_; }
    const std::string& txnId()     const { return txnId_; }     // groups the pair
    const std::string& accountId() const { return accountId_; }
    Direction          direction() const { return direction_; }
    const Money&       amount()    const { return amount_; }
    std::int64_t       seq()       const { return seq_; }       // global ordering

    // Signed effect on the account's balance: credits add, debits subtract.
    Money signedAmount() const {
        return direction_ == Direction::CREDIT ? amount_ : (amount_ * -1);
    }

private:
    std::string  entryId_;
    std::string  txnId_;
    std::string  accountId_;
    Direction    direction_;
    Money        amount_;
    std::int64_t seq_;
};

} // namespace pay::domain
