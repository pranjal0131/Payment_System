#pragma once
//
// InMemoryLedgerRepository.hpp — ADAPTER for the LedgerRepository port.
//
// The single most important invariant in the whole system lives here:
//
//     every posting must balance — the signed amounts sum to ZERO.
//
// If a batch does not balance we REJECT it. This is what guarantees money is
// never created or destroyed, only moved. We enforce it in the adapter so no
// caller can ever bypass it. The real adapter writes these same rows inside one
// DB transaction (all-or-nothing durability).
//
#include <mutex>
#include <string>
#include <vector>

#include "payment/ports/Ports.hpp"

namespace pay::adapters {

class InMemoryLedgerRepository final : public ports::LedgerRepository {
public:
    common::Status post(const std::vector<domain::LedgerEntry>& entries) override {
        if (entries.empty())
            return common::Status::fail(common::ErrorCode::VALIDATION_FAILED,
                                        "ledger posting must have entries");

        // 1) All entries must share a currency to be summable.
        common::Currency ccy = entries.front().amount().currency();

        // 2) Signed amounts must net to zero — the double-entry rule.
        std::int64_t net = 0;
        for (const auto& e : entries) {
            if (e.amount().currency() != ccy)
                return common::Status::fail(common::ErrorCode::VALIDATION_FAILED,
                                            "mixed currencies in one posting");
            net += e.signedAmount().minorUnits();
        }
        if (net != 0)
            return common::Status::fail(common::ErrorCode::INTERNAL_ERROR,
                "unbalanced posting: net " + std::to_string(net) + " minor units (must be 0)");

        // 3) Commit atomically (single lock == the demo's "one DB transaction").
        std::lock_guard<std::mutex> lk(mtx_);
        for (const auto& e : entries) entries_.push_back(e);
        return common::Status::ok();
    }

    std::vector<domain::LedgerEntry> entriesFor(const std::string& accountId) override {
        std::lock_guard<std::mutex> lk(mtx_);
        std::vector<domain::LedgerEntry> out;
        for (const auto& e : entries_)
            if (e.accountId() == accountId) out.push_back(e);
        return out;
    }

private:
    std::mutex mtx_;
    std::vector<domain::LedgerEntry> entries_;   // append-only journal
};

} // namespace pay::adapters
