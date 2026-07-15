#pragma once
//
// FileTransactionRepository.hpp — a DURABLE adapter for the TransactionRepository
// port, backed by a plain text file.
//
// WHY this adapter exists: the in-memory repo loses everything when the process
// dies. This one persists to disk, so transactions survive a restart. Crucially,
// it implements the *same* `TransactionRepository` port — so swapping it in is a
// ONE-LINE change at the composition root (main.cpp) and the PaymentService code
// does not change at all. That is the whole promise of hexagonal architecture,
// demonstrated concretely.
//
// It also unlocks a real-world behaviour: **idempotency replay across restarts**.
// A captured transaction is on disk, so a retried request after a crash still
// finds the original and refuses to double-charge.
//
// Design: kept deliberately lightweight — no database, no external library, just
// <fstream>. One tab-separated line per transaction. On startup we load the file
// into an in-memory index; each save rewrites the file (last-write-wins). That is
// plenty for a demo/learning adapter; a production file store would append + fsync
// or, more realistically, be replaced by a Postgres adapter behind this same port.
//
#include <fstream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>

#include "payment/ports/Ports.hpp"

namespace pay::adapters {

class FileTransactionRepository final : public ports::TransactionRepository {
public:
    explicit FileTransactionRepository(std::string path)
        : path_(std::move(path)) {
        loadFromDisk();   // rebuild the in-memory index from the file on startup
    }

    std::optional<domain::Transaction> find(const std::string& txnId) override {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = index_.find(txnId);
        return it == index_.end() ? std::nullopt
                                  : std::optional<domain::Transaction>(it->second);
    }

    void save(const domain::Transaction& txn) override {
        std::lock_guard<std::mutex> lk(mtx_);
        index_.insert_or_assign(txn.id(), txn);
        flushToDisk();    // durability: the write is on disk before we return
    }

    // How many transactions are currently persisted (handy for tests/demo).
    std::size_t size() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return index_.size();
    }

private:
    // Field delimiter. Chosen because ids/keys in this system never contain tabs.
    static constexpr char DELIM = '\t';

    // ---- Serialization: one line per transaction ----
    static std::string serialize(const domain::Transaction& t) {
        std::ostringstream os;
        os << t.id()             << DELIM
           << t.idempotencyKey() << DELIM
           << t.payerAccountId() << DELIM
           << t.payeeAccountId() << DELIM
           << t.amount().minorUnits()                     << DELIM
           << static_cast<int>(t.amount().currency())     << DELIM
           << static_cast<int>(t.method())                << DELIM
           << static_cast<int>(t.status())                << DELIM
           << t.createdAtEpoch();
        return os.str();
    }

    static std::optional<domain::Transaction> deserialize(const std::string& line) {
        std::istringstream is(line);
        std::string id, key, payer, payee, minor, ccy, method, status, created;
        if (!std::getline(is, id,     DELIM)) return std::nullopt;
        if (!std::getline(is, key,    DELIM)) return std::nullopt;
        if (!std::getline(is, payer,  DELIM)) return std::nullopt;
        if (!std::getline(is, payee,  DELIM)) return std::nullopt;
        if (!std::getline(is, minor,  DELIM)) return std::nullopt;
        if (!std::getline(is, ccy,    DELIM)) return std::nullopt;
        if (!std::getline(is, method, DELIM)) return std::nullopt;
        if (!std::getline(is, status, DELIM)) return std::nullopt;
        if (!std::getline(is, created,DELIM)) return std::nullopt;

        using namespace pay::common;
        Money amount = Money::fromMinor(std::stoll(minor),
                                        static_cast<Currency>(std::stoi(ccy)));
        return domain::Transaction::rehydrate(
            id, key, payer, payee, amount,
            static_cast<PaymentMethod>(std::stoi(method)),
            static_cast<TxnStatus>(std::stoi(status)),
            std::stoll(created));
    }

    void loadFromDisk() {
        std::ifstream in(path_);
        if (!in) return;   // no file yet == empty repo, not an error
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty()) continue;
            if (auto t = deserialize(line)) index_.insert_or_assign(t->id(), *t);
        }
    }

    // Rewrite the whole file from the in-memory index. Caller holds mtx_.
    void flushToDisk() {
        std::ofstream out(path_, std::ios::trunc);
        for (const auto& [_, t] : index_) out << serialize(t) << '\n';
    }

    std::string        path_;
    mutable std::mutex mtx_;
    std::unordered_map<std::string, domain::Transaction> index_;
};

} // namespace pay::adapters
