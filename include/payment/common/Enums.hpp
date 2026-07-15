#pragma once
//
// Enums.hpp — strongly-typed domain enums + their string conversions.
//
// LLD lesson: use `enum class` (scoped enums), NOT plain enums or magic strings.
//   * Type-safe: you cannot accidentally pass a Currency where a Status is expected.
//   * No implicit int conversion -> fewer bugs.
//
#include <string_view>

namespace pay::common {

// ---- Currency (ISO-4217 subset for the demo) -------------------------------
enum class Currency { INR, USD, EUR };

inline std::string_view toCode(Currency c) {
    switch (c) {
        case Currency::INR: return "INR";
        case Currency::USD: return "USD";
        case Currency::EUR: return "EUR";
    }
    return "???";
}

// ---- Payment method: drives the Strategy pattern later ---------------------
enum class PaymentMethod { CARD, UPI, WALLET, NET_BANKING };

inline std::string_view toString(PaymentMethod m) {
    switch (m) {
        case PaymentMethod::CARD:        return "CARD";
        case PaymentMethod::UPI:         return "UPI";
        case PaymentMethod::WALLET:      return "WALLET";
        case PaymentMethod::NET_BANKING: return "NET_BANKING";
    }
    return "UNKNOWN";
}

// ---- Transaction lifecycle: drives the State pattern later -----------------
// INITIATED -> AUTHORIZED -> CAPTURED -> SETTLED       (happy path)
//           -> FAILED                                  (auth/capture failed)
// CAPTURED  -> REFUNDED                                (money returned)
enum class TxnStatus {
    INITIATED,
    AUTHORIZED,
    CAPTURED,
    SETTLED,
    FAILED,
    REFUNDED
};

inline std::string_view toString(TxnStatus s) {
    switch (s) {
        case TxnStatus::INITIATED:  return "INITIATED";
        case TxnStatus::AUTHORIZED: return "AUTHORIZED";
        case TxnStatus::CAPTURED:   return "CAPTURED";
        case TxnStatus::SETTLED:    return "SETTLED";
        case TxnStatus::FAILED:     return "FAILED";
        case TxnStatus::REFUNDED:   return "REFUNDED";
    }
    return "UNKNOWN";
}

// ---- Ledger posting direction (double-entry accounting) --------------------
// Every money movement is recorded as a DEBIT on one account and a matching
// CREDIT on another. The two always sum to zero — that invariant is what makes
// a ledger auditable.
enum class Direction { DEBIT, CREDIT };

inline std::string_view toString(Direction d) {
    switch (d) {
        case Direction::DEBIT:  return "DEBIT";
        case Direction::CREDIT: return "CREDIT";
    }
    return "UNKNOWN";
}

// ---- Account type for the double-entry ledger ------------------------------
enum class AccountType { USER_WALLET, MERCHANT, SYSTEM_ESCROW, BANK_GATEWAY };

inline std::string_view toString(AccountType t) {
    switch (t) {
        case AccountType::USER_WALLET:   return "USER_WALLET";
        case AccountType::MERCHANT:      return "MERCHANT";
        case AccountType::SYSTEM_ESCROW: return "SYSTEM_ESCROW";
        case AccountType::BANK_GATEWAY:  return "BANK_GATEWAY";
    }
    return "UNKNOWN";
}

} // namespace pay::common
