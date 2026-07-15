#pragma once
//
// Result.hpp — a tiny Result<T> / error type.
//
// LLD lesson: In a payment system we do NOT use exceptions for *expected*
// business failures (insufficient balance, fraud rejected, idempotency replay).
// Exceptions are for *programmer errors* / truly exceptional conditions.
// Expected failures are part of the API contract, so we return them explicitly.
//
// This is the same idea as Rust's Result<T,E> or std::expected (C++23).
// We hand-roll a small version so it compiles on C++20.
//
#include <string>
#include <utility>
#include <variant>
#include <stdexcept>

namespace pay::common {

// Machine-readable error code (good for metrics/alerting) + human message.
enum class ErrorCode {
    NONE,
    VALIDATION_FAILED,
    INSUFFICIENT_FUNDS,
    ACCOUNT_NOT_FOUND,
    DUPLICATE_REQUEST,      // idempotency replay
    FRAUD_REJECTED,
    INVALID_STATE_TRANSITION,
    GATEWAY_ERROR,
    INTERNAL_ERROR
};

struct Error {
    ErrorCode   code;
    std::string message;

    Error(ErrorCode c, std::string msg) : code(c), message(std::move(msg)) {}
};

// Result<T>: holds either a value T (success) or an Error (failure).
template <typename T>
class Result {
public:
    static Result ok(T value)        { return Result(std::move(value)); }
    static Result fail(Error e)      { return Result(std::move(e)); }
    static Result fail(ErrorCode c, std::string msg) { return Result(Error(c, std::move(msg))); }

    bool isOk()  const { return std::holds_alternative<T>(data_); }
    bool isErr() const { return !isOk(); }
    explicit operator bool() const { return isOk(); }

    // Access the value. Throws if you forgot to check isOk() — fail loud in dev.
    const T& value() const {
        if (isErr()) throw std::logic_error("Result::value() called on an error result");
        return std::get<T>(data_);
    }
    T& value() {
        if (isErr()) throw std::logic_error("Result::value() called on an error result");
        return std::get<T>(data_);
    }

    const Error& error() const {
        if (isOk()) throw std::logic_error("Result::error() called on an ok result");
        return std::get<Error>(data_);
    }

private:
    explicit Result(T v)     : data_(std::move(v)) {}
    explicit Result(Error e) : data_(std::move(e)) {}
    std::variant<T, Error> data_;
};

// Specialisation for operations that return nothing on success (void-like).
class Status {
public:
    static Status ok() { return Status(Error(ErrorCode::NONE, "")); }
    static Status fail(ErrorCode c, std::string msg) { return Status(Error(c, std::move(msg))); }

    bool isOk()  const { return err_.code == ErrorCode::NONE; }
    bool isErr() const { return !isOk(); }
    explicit operator bool() const { return isOk(); }
    const Error& error() const { return err_; }

private:
    explicit Status(Error e) : err_(std::move(e)) {}
    Error err_;
};

} // namespace pay::common
