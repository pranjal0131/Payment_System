#pragma once
//
// PaymentStrategy.hpp — the STRATEGY pattern for payment methods.
//
// LLD lesson: authorizing a payment differs per method (CARD talks to a card
// network, UPI to a UPI PSP, WALLET is an internal balance move, NET_BANKING
// redirects to a bank). The naive design is one giant `switch (method)` sprinkled
// through the service — impossible to test in isolation and it must be edited
// every time a new method is added (violates the Open/Closed Principle).
//
// Strategy fixes this: define ONE interface (authorize/capture/refund) and one
// implementation per method. The service depends only on the interface, so
// adding AMEX or a BNPL provider means adding a class — no existing code changes.
//
// This mirrors how a real PSP (Stripe/Razorpay) is structured internally.
//
#include <memory>
#include <string>

#include "payment/common/Result.hpp"
#include "payment/domain/Transaction.hpp"

namespace pay::patterns {

using common::Result;
using common::Status;
using common::ErrorCode;
using domain::Transaction;

// The reference a gateway hands back for a successful authorization/capture.
// In production this is the acquirer/PSP transaction id used for reconciliation.
struct GatewayRef {
    std::string provider;   // "VISA", "UPI:hdfc", "WALLET", ...
    std::string reference;  // opaque id from that provider
};

// The Strategy interface. Pure virtual = a PORT the domain codes against.
class PaymentStrategy {
public:
    virtual ~PaymentStrategy() = default;

    // Human-readable method name for logs/metrics.
    virtual std::string name() const = 0;

    // Reserve/authorize the funds with the underlying provider.
    // Returns a GatewayRef on success, a typed Error on decline.
    virtual Result<GatewayRef> authorize(const Transaction& txn) = 0;

    // Capture previously authorized funds (money actually moves).
    virtual Status capture(const Transaction& txn, const GatewayRef& ref) = 0;

    // Return captured funds to the payer.
    virtual Status refund(const Transaction& txn, const GatewayRef& ref) = 0;
};

using PaymentStrategyPtr = std::shared_ptr<PaymentStrategy>;

// ---------------------------------------------------------------------------
// Concrete strategies. Each is intentionally small and self-contained. In the
// real system these wrap an SDK/HTTP client behind a port; here they simulate a
// provider so the flow runs end-to-end offline. Note there is NO shared mutable
// state, so a single strategy instance is safe to share across threads.
// ---------------------------------------------------------------------------

class CardPaymentStrategy final : public PaymentStrategy {
public:
    std::string name() const override { return "CARD"; }

    Result<GatewayRef> authorize(const Transaction& txn) override {
        // A real impl would tokenize the PAN, run 3-D Secure, call the network.
        // We approve and mint a deterministic-looking reference.
        return Result<GatewayRef>::ok(GatewayRef{"VISA", "auth_" + txn.id()});
    }
    Status capture(const Transaction&, const GatewayRef&) override { return Status::ok(); }
    Status refund (const Transaction&, const GatewayRef&) override { return Status::ok(); }
};

class UpiPaymentStrategy final : public PaymentStrategy {
public:
    std::string name() const override { return "UPI"; }

    Result<GatewayRef> authorize(const Transaction& txn) override {
        // Real impl: collect/intent request to the PSP, await callback.
        return Result<GatewayRef>::ok(GatewayRef{"UPI", "upi_" + txn.id()});
    }
    Status capture(const Transaction&, const GatewayRef&) override { return Status::ok(); }
    Status refund (const Transaction&, const GatewayRef&) override { return Status::ok(); }
};

class WalletPaymentStrategy final : public PaymentStrategy {
public:
    std::string name() const override { return "WALLET"; }

    Result<GatewayRef> authorize(const Transaction& txn) override {
        // Wallet is an internal balance — no external provider. Authorization is
        // implicit; the actual balance move happens in the ledger/service layer.
        return Result<GatewayRef>::ok(GatewayRef{"WALLET", "wlt_" + txn.id()});
    }
    Status capture(const Transaction&, const GatewayRef&) override { return Status::ok(); }
    Status refund (const Transaction&, const GatewayRef&) override { return Status::ok(); }
};

class NetBankingPaymentStrategy final : public PaymentStrategy {
public:
    std::string name() const override { return "NET_BANKING"; }

    Result<GatewayRef> authorize(const Transaction& txn) override {
        // Real impl: redirect the user to their bank, await the return callback.
        return Result<GatewayRef>::ok(GatewayRef{"NETBANK", "nb_" + txn.id()});
    }
    Status capture(const Transaction&, const GatewayRef&) override { return Status::ok(); }
    Status refund (const Transaction&, const GatewayRef&) override { return Status::ok(); }
};

} // namespace pay::patterns
