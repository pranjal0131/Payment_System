#pragma once
//
// PaymentStrategyFactory.hpp — the FACTORY pattern.
//
// LLD lesson: something has to decide *which* concrete strategy to build for a
// given PaymentMethod. If the service does `if (method == CARD) new Card... else
// if ...`, that selection logic leaks into business code and is duplicated
// wherever a strategy is needed. The Factory centralizes construction behind one
// call: `factory.create(method)`. Callers stay ignorant of concrete classes.
//
// Design choice: the strategies here are stateless, so we build each concrete
// strategy ONCE and hand back a shared_ptr (flyweight-style reuse). If a strategy
// ever held per-request state we'd construct a fresh one per call instead.
//
#include <memory>
#include <unordered_map>

#include "payment/common/Enums.hpp"
#include "payment/patterns/PaymentStrategy.hpp"

namespace pay::patterns {

using common::PaymentMethod;

class PaymentStrategyFactory {
public:
    PaymentStrategyFactory() {
        // Register one reusable instance per method. Adding a method is a single
        // line here — the Open/Closed extension point of the whole scheme.
        registry_[PaymentMethod::CARD]        = std::make_shared<CardPaymentStrategy>();
        registry_[PaymentMethod::UPI]         = std::make_shared<UpiPaymentStrategy>();
        registry_[PaymentMethod::WALLET]      = std::make_shared<WalletPaymentStrategy>();
        registry_[PaymentMethod::NET_BANKING] = std::make_shared<NetBankingPaymentStrategy>();
    }

    // Returns the strategy for a method, or nullptr if unsupported.
    PaymentStrategyPtr create(PaymentMethod method) const {
        auto it = registry_.find(method);
        return it == registry_.end() ? nullptr : it->second;
    }

private:
    std::unordered_map<PaymentMethod, PaymentStrategyPtr> registry_;
};

} // namespace pay::patterns
