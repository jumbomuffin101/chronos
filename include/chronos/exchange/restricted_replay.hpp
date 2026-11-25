#pragma once

#include "chronos/exchange/operation_record.hpp"

#include <array>
#include <bit>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <vector>

namespace chronos::exchange {

// Experimental in-memory domain only. No gateway attachment or durable format.
class RestrictedReplay final {
    friend class CanonicalReplayEnvelope;
#ifdef CHRONOS_TEST_ID_SEAM
    friend class AuthorizationTestHarness;
#endif
    struct Context {};
    struct InstalledRecord {
        std::vector<std::uint8_t> legacy;
        std::vector<std::uint8_t> canonical;
    };
    static const std::shared_ptr<const Context> &context() {
        static thread_local const auto value = std::make_shared<const Context>();
        return value;
    }

  public:
    static constexpr std::size_t max_operations = 128;
    static constexpr std::size_t max_state_words = 8192;
    enum class Kind : std::uint64_t {
        Open = 1,
        Risk = 2,
        Submit = 3,
        Cancel = 4,
        Consume = 5,
        Close = 6
    };
    struct Operation {
        Kind kind;
        // Exact per-kind schema and reserved-zero requirements are documented.
        std::array<std::uint64_t, 12> args{};
    };
    using Bytes = std::vector<std::uint8_t>;
    struct Applied {
        bool committed;
        std::uint64_t outcome;
    };

    explicit RestrictedReplay(std::size_t capacity = max_operations)
        : owner_(context()), state_(fresh()), capacity_(capacity) {
        if (capacity > max_operations)
            throw std::invalid_argument("restricted operation capacity");
    }
    RestrictedReplay(const RestrictedReplay &) = delete;
    RestrictedReplay &operator=(const RestrictedReplay &) = delete;
    RestrictedReplay(RestrictedReplay &&) = delete;
    RestrictedReplay &operator=(RestrictedReplay &&) = delete;

    // All allocations, execution, record encoding and verification precede
    // publication. False commit is a deliberate abort, not an admitted record.
    [[nodiscard]] Applied apply(const Operation &op, bool commit = true) {
        require_owner();
        if (!valid(op))
            throw std::invalid_argument("unsupported restricted operation");
        if (count_ >= capacity_)
            throw std::length_error("restricted operation capacity exhausted");
        if (records_[count_])
            throw std::logic_error("restricted record slot already occupied");
        auto candidate = state_->clone_restricted_isolated();
        const auto outcome = execute(*candidate, op);
        const auto words = candidate->isolated_continuation_state(max_state_words);
        if (!commit)
            return {false, outcome};
        auto canonical = candidate->canonical_isolated_state(canonical_limit_);
        auto encoded = encode(count_ + 1, op, outcome, words);
        auto prepared = std::make_unique<InstalledRecord>(
            InstalledRecord{std::move(encoded), std::move(canonical)});
        // All allocations and checks are complete. No callback or observer
        // can run between the two nonthrowing swaps and count publication.
        static_assert(std::is_nothrow_swappable_v<std::unique_ptr<Exchange>>);
        static_assert(std::is_nothrow_swappable_v<std::unique_ptr<InstalledRecord>>);
        static_assert(std::is_nothrow_destructible_v<Exchange>);
        state_.swap(candidate);
        records_[count_].swap(prepared);
        ++count_;
        return {true, outcome};
    }
    [[nodiscard]] std::vector<Bytes> records() const {
        require_owner();
        std::vector<Bytes> copy;
        copy.reserve(count_);
        for (std::size_t i = 0; i < count_; ++i)
            copy.push_back(records_[i]->legacy);
        return copy;
    }
    [[nodiscard]] std::vector<std::uint64_t> exact_state() const {
        require_owner();
        return state_->isolated_continuation_state(max_state_words);
    }
    [[nodiscard]] std::size_t size() const {
        require_owner();
        return count_;
    }
    [[nodiscard]] Bytes canonical_state() const {
        require_owner();
        return state_->canonical_isolated_state();
    }
    [[nodiscard]] Bytes recorded_canonical(std::size_t index) const {
        require_owner();
        if (index >= count_)
            throw std::out_of_range("restricted record index");
        return records_[index]->canonical;
    }
#ifdef CHRONOS_TEST_ID_SEAM
    inline static thread_local std::size_t execution_count_for_test = 0;
    void set_canonical_limit_for_test(std::size_t limit) {
        require_owner();
        canonical_limit_ = limit;
    }
#endif
    // expected_count is supplied independently to detect a missing suffix.
    [[nodiscard]] static bool verify(std::span<const Bytes> records, std::size_t expected_count) {
        if (records.size() != expected_count || expected_count > max_operations)
            return false;
        try {
            (void)rebuild(records, expected_count);
            return true;
        } catch (const std::invalid_argument &) {
            return false;
        } catch (const std::length_error &) {
            return false;
        } catch (const std::logic_error &) {
            return false;
        }
    }

  private:
    struct Record {
        std::uint64_t sequence;
        Operation op;
        std::uint64_t outcome;
        std::vector<std::uint64_t> state;
    };
    static constexpr std::uint64_t magic = 0x4348524f50523031ULL; // CHROPR01
    static constexpr std::size_t header_words = 17;

    [[nodiscard]] static std::unique_ptr<Exchange> fresh() {
        return Exchange::isolated(16, 128, 128, 128, 0, 0);
    }
    void require_owner() const {
        if (owner_ != context())
            throw std::logic_error("restricted exchange requires creating context");
    }
    [[nodiscard]] static bool valid(const Operation &op) noexcept {
        std::size_t used = 0;
        switch (op.kind) {
        case Kind::Open:
        case Kind::Close:
            used = 1;
            break;
        case Kind::Cancel:
            used = 3;
            break;
        case Kind::Consume:
            used = 2;
            break;
        case Kind::Submit:
            used = 8;
            if (op.args[3] > std::numeric_limits<std::uint32_t>::max() || op.args[4] > 255 ||
                op.args[5] > 255)
                return false;
            break;
        case Kind::Risk:
            used = 12;
            if (op.args[4] > std::numeric_limits<std::size_t>::max() || op.args[7] > 4)
                return false;
            for (std::size_t i = 0; i < 4; ++i)
                if ((i >= op.args[7] && op.args[8 + i] != 0) ||
                    op.args[8 + i] > std::numeric_limits<std::uint32_t>::max())
                    return false;
            break;
        default:
            return false;
        }
        for (std::size_t i = used; i < op.args.size(); ++i)
            if (op.args[i] != 0)
                return false;
        return true;
    }
    [[nodiscard]] static std::uint64_t result_code(Result result) {
        const auto it =
            std::find(operation_result_codes.begin(), operation_result_codes.end(), result);
        if (it == operation_result_codes.end())
            throw std::logic_error("unrepresented exchange result");
        return 16 + static_cast<std::uint64_t>(it - operation_result_codes.begin());
    }
    [[nodiscard]] static std::uint64_t execute(Exchange &exchange, const Operation &op) {
#ifdef CHRONOS_TEST_ID_SEAM
        ++execution_count_for_test;
#endif
        const auto &a = op.args;
        switch (op.kind) {
        case Kind::Open:
            return exchange.open_session(a[0]) ? 1 : 2;
        case Kind::Risk: {
            RiskLimits limits;
            limits.max_order_quantity = a[1];
            limits.max_order_notional = a[2];
            limits.max_open_notional = a[3];
            limits.max_open_orders = static_cast<std::size_t>(a[4]);
            limits.max_abs_position = a[5];
            limits.max_market_price = std::bit_cast<std::int64_t>(a[6]);
            for (std::size_t i = 0; i < a[7]; ++i)
                limits.allowed_symbols.push_back(static_cast<std::uint32_t>(a[8 + i]));
            return exchange.configure_risk(a[0], limits) ? 1 : 2;
        }
        case Kind::Submit:
            return result_code(
                exchange.submit(a[0], a[1], a[2], core::SymbolId{static_cast<std::uint32_t>(a[3])},
                                static_cast<core::Side>(a[4]), static_cast<core::OrderType>(a[5]),
                                std::bit_cast<std::int64_t>(a[6]), a[7]));
        case Kind::Cancel:
            return result_code(exchange.cancel(a[0], a[1], a[2]));
        case Kind::Consume: {
            if (a[0] == 0 || a[1] == 0)
                return 4;
            const auto report = exchange.front(a[0]);
            if (!report)
                return 2;
            if (report->sequence != a[1])
                return 3;
            exchange.pop(a[0]);
            return 1;
        }
        case Kind::Close: {
            const auto before = exchange.isolated_continuation_state(max_state_words);
            exchange.close_session(a[0]);
            return before == exchange.isolated_continuation_state(max_state_words) ? 2 : 1;
        }
        }
        throw std::invalid_argument("unsupported restricted operation");
    }
    static void put(Bytes &out, std::uint64_t value) {
        for (int i = 7; i >= 0; --i)
            out.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
    }
    [[nodiscard]] static Bytes encode(std::size_t sequence, const Operation &op,
                                      std::uint64_t outcome, std::span<const std::uint64_t> state) {
        if (state.empty() || state.size() > max_state_words)
            throw std::length_error("restricted state limit");
        Bytes bytes;
        bytes.reserve((header_words + state.size()) * 8);
        put(bytes, magic);
        put(bytes, sequence);
        put(bytes, static_cast<std::uint64_t>(op.kind));
        put(bytes, outcome);
        for (const auto word : op.args)
            put(bytes, word);
        put(bytes, state.size());
        for (const auto word : state)
            put(bytes, word);
        return bytes;
    }
    [[nodiscard]] static Record decode(std::span<const std::uint8_t> bytes) {
        if (bytes.size() < header_words * 8 ||
            bytes.size() > (header_words + max_state_words) * 8 || bytes.size() % 8 != 0)
            throw std::invalid_argument("restricted record length");
        std::size_t pos = 0;
        const auto get = [&] {
            std::uint64_t value = 0;
            for (int i = 0; i < 8; ++i)
                value = (value << 8) | bytes[pos++];
            return value;
        };
        if (get() != magic)
            throw std::invalid_argument("restricted record version");
        Record r{};
        r.sequence = get();
        r.op.kind = static_cast<Kind>(get());
        r.outcome = get();
        for (auto &word : r.op.args)
            word = get();
        const auto count = get();
        if (!valid(r.op) || count == 0 || count > max_state_words ||
            bytes.size() != (header_words + count) * 8)
            throw std::invalid_argument("restricted record fields");
        r.state.reserve(static_cast<std::size_t>(count));
        for (std::uint64_t i = 0; i < count; ++i)
            r.state.push_back(get());
        return r;
    }
    [[nodiscard]] static std::unique_ptr<Exchange> rebuild(std::span<const Bytes> records,
                                                           std::size_t count) {
        auto candidate = fresh();
        for (std::size_t i = 0; i < count; ++i) {
            const auto expected = decode(records[i]);
            if (expected.sequence != i + 1 ||
                execute(*candidate, expected.op) != expected.outcome ||
                candidate->isolated_continuation_state(max_state_words) != expected.state)
                throw std::logic_error("restricted replay diverged");
        }
        return candidate;
    }
    std::shared_ptr<const Context> owner_;
    std::unique_ptr<Exchange> state_;
    std::size_t capacity_;
    std::size_t count_ = 0;
    std::size_t canonical_limit_ = 65536;
    std::array<std::unique_ptr<InstalledRecord>, max_operations> records_{};
};
} // namespace chronos::exchange
