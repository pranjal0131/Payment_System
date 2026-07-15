#pragma once
//
// Money.hpp  —  A Value Object representing an amount of money.
//
// LLD lesson: This is the classic "Value Object" pattern.
//   * It is IMMUTABLE  -> once created it never changes (thread-safe by design).
//   * It has NO identity -> two Money objects with same amount+currency are equal.
//   * It encapsulates an INVARIANT -> we NEVER store money as float/double.
//
// WHY no double?  0.1 + 0.2 != 0.3 in floating point. In a payment system that
// rounding error is real lost money. So we store the amount in the smallest
// indivisible unit ("minor units"): paise for INR, cents for USD. 100 paise = 1 INR.
//
#include <cstdint>
#include <string>
#include <stdexcept>
#include <ostream>
#include "payment/common/Enums.hpp"

namespace pay::common {

class Money {
public:
    // Factory-style named constructors make intent explicit at the call site.
    // Money::fromMinor(150, INR)  -> 1.50 INR
    static Money fromMinor(std::int64_t minorUnits, Currency ccy) {
        return Money(minorUnits, ccy);
    }
    // Money::fromMajor(1.50, INR) -> rounds to 150 paise (helper for demos/UI).
    static Money fromMajor(double majorUnits, Currency ccy) {
        // +/- 0.5 rounding to nearest minor unit.
        std::int64_t minor = static_cast<std::int64_t>(
            majorUnits * 100.0 + (majorUnits >= 0 ? 0.5 : -0.5));
        return Money(minor, ccy);
    }
    static Money zero(Currency ccy) { return Money(0, ccy); }

    std::int64_t minorUnits() const { return minorUnits_; }
    Currency     currency()   const { return currency_; }
    bool         isZero()     const { return minorUnits_ == 0; }
    bool         isPositive() const { return minorUnits_ > 0; }
    bool         isNegative() const { return minorUnits_ < 0; }

    // --- Arithmetic. Returns NEW objects (immutability). Currency must match. ---
    Money operator+(const Money& o) const { assertSameCcy(o); return Money(minorUnits_ + o.minorUnits_, currency_); }
    Money operator-(const Money& o) const { assertSameCcy(o); return Money(minorUnits_ - o.minorUnits_, currency_); }

    // Scale by an integer factor (e.g. quantity). Kept integer-only on purpose
    // so we never introduce fractional paise.
    Money operator*(std::int64_t factor) const { return Money(minorUnits_ * factor, currency_); }

    bool operator==(const Money& o) const { return minorUnits_ == o.minorUnits_ && currency_ == o.currency_; }
    bool operator!=(const Money& o) const { return !(*this == o); }
    bool operator<(const Money& o)  const { assertSameCcy(o); return minorUnits_ <  o.minorUnits_; }
    bool operator<=(const Money& o) const { assertSameCcy(o); return minorUnits_ <= o.minorUnits_; }
    bool operator>(const Money& o)  const { assertSameCcy(o); return minorUnits_ >  o.minorUnits_; }
    bool operator>=(const Money& o) const { assertSameCcy(o); return minorUnits_ >= o.minorUnits_; }

    // "1.50 INR" style for logs/UI.
    std::string toString() const {
        std::int64_t whole = minorUnits_ / 100;
        std::int64_t frac  = minorUnits_ % 100;
        if (frac < 0) frac = -frac;
        std::string s = std::to_string(whole) + ".";
        if (frac < 10) s += "0";
        s += std::to_string(frac) + " " + std::string(toCode(currency_));
        return s;
    }

    friend std::ostream& operator<<(std::ostream& os, const Money& m) {
        return os << m.toString();
    }

private:
    Money(std::int64_t minor, Currency ccy) : minorUnits_(minor), currency_(ccy) {}

    void assertSameCcy(const Money& o) const {
        if (currency_ != o.currency_)
            throw std::invalid_argument("Money currency mismatch: cannot mix " +
                std::string(toCode(currency_)) + " and " + std::string(toCode(o.currency_)));
    }

    std::int64_t minorUnits_;  // amount in paise/cents — never a float
    Currency     currency_;
};

} // namespace pay::common
