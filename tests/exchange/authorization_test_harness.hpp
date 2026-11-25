#pragma once

#ifndef CHRONOS_TEST_ID_SEAM
#error "The in-memory authorization harness is test-only and cannot authorize storage."
#endif

#include "chronos/exchange/canonical_replay_envelope.hpp"

#include <exception>

namespace chronos::exchange {

// A deterministic test double only. It is not a storage authority, durable
// authorization, gateway feature, or a production transaction API.
class AuthorizationTestHarness final {
  public:
    enum class Phase {
        Idle,
        Preparing,
        Prepared,
        AwaitingExternalDecision,
        Authorized,
        Installed,
        Aborted,
        FailedStop,
        Stopped
    };
    enum class TestDecision {
        Permit,
        Refuse,
        Ambiguous,
        WrongCandidate,
        WrongSequence,
        WrongIdentity,
        WrongConfiguration,
        WrongPrefix,
        WrongLength
    };
    enum class Diagnostic { None, Refused, Ambiguous, BindingMismatch, ShutdownPending };
    using Identity = CanonicalReplayEnvelope::Identity;
    using Digest = CanonicalReplayEnvelope::Digest;

    explicit AuthorizationTestHarness(Identity identity) : identity_(identity) {
        if (std::all_of(identity_.begin(), identity_.end(), [](auto v) { return v == 0; }))
            throw std::invalid_argument("test identity must be nonzero");
    }
    AuthorizationTestHarness(const AuthorizationTestHarness &) = delete;
    AuthorizationTestHarness &operator=(const AuthorizationTestHarness &) = delete;
    ~AuthorizationTestHarness() noexcept {
        // An unresolved simulated external decision must be explicitly handled.
        if (phase_ == Phase::AwaitingExternalDecision || phase_ == Phase::Authorized ||
            phase_ == Phase::FailedStop)
            std::terminate();
    }

    [[nodiscard]] Phase phase() const {
        owner_.require_owner();
        return phase_;
    }
    [[nodiscard]] Diagnostic diagnostic() const {
        owner_.require_owner();
        return diagnostic_;
    }
    [[nodiscard]] std::size_t size() const { return owner_.size(); }
    [[nodiscard]] std::vector<std::uint64_t> exact_state() const { return owner_.exact_state(); }
    [[nodiscard]] RestrictedReplay::Bytes canonical_state() const {
        return owner_.canonical_state();
    }
    [[nodiscard]] std::vector<RestrictedReplay::Bytes> records() const { return owner_.records(); }
    [[nodiscard]] std::size_t request_size_for_test() const {
        owner_.require_owner();
        return request_.size();
    }

    void prepare(const RestrictedReplay::Operation &op) {
        require(Phase::Idle);
        phase_ = Phase::Preparing;
        try {
            if (!RestrictedReplay::valid(op))
                throw std::invalid_argument("unsupported test operation");
            if (owner_.count_ >= owner_.capacity_ || owner_.records_[owner_.count_])
                throw std::length_error("test operation capacity");
            const auto prefix = CanonicalReplayEnvelope::seal(owner_, identity_);
            auto candidate = owner_.state_->clone_restricted_isolated();
            const auto outcome = RestrictedReplay::execute(*candidate, op);
            const auto words =
                candidate->isolated_continuation_state(RestrictedReplay::max_state_words);
            auto canonical = candidate->canonical_isolated_state();
            auto legacy = RestrictedReplay::encode(owner_.count_ + 1, op, outcome, words);
            auto record = std::make_unique<RestrictedReplay::InstalledRecord>(
                RestrictedReplay::InstalledRecord{std::move(legacy), std::move(canonical)});
            const auto config = CanonicalReplayEnvelope::config_hash();
            const auto sequence = static_cast<std::uint64_t>(owner_.count_ + 1);
            RestrictedReplay::Bytes request;
            constexpr std::size_t max_request = 140000;
            if (record->legacy.size() > max_request - 256 ||
                record->canonical.size() > max_request - 256 - record->legacy.size())
                throw std::length_error("test authorization request limit");
            request.reserve(256 + record->legacy.size() + record->canonical.size());
            put(request, 0x4348524155544831ULL); // CHRAUTH1
            request.insert(request.end(), identity_.begin(), identity_.end());
            request.insert(request.end(), config.begin(), config.end());
            put(request, sequence);
            put(request, owner_.count_);
            put(request, prefix.bytes.size());
            request.insert(request.end(), prefix.anchor.digest.begin(), prefix.anchor.digest.end());
            put(request, static_cast<std::uint64_t>(op.kind));
            put(request, outcome);
            for (auto arg : op.args)
                put(request, arg);
            put(request, record->legacy.size());
            request.insert(request.end(), record->legacy.begin(), record->legacy.end());
            put(request, record->canonical.size());
            request.insert(request.end(), record->canonical.begin(), record->canonical.end());
            if (request.size() > max_request)
                throw std::length_error("test authorization request limit");
            Token expected{identity_,
                           config,
                           sequence,
                           owner_.count_,
                           prefix.bytes.size(),
                           prefix.anchor.digest,
                           request.size(),
                           journal::sha256(request)};
            candidate_.swap(candidate);
            prepared_.swap(record);
            request_.swap(request);
            expected_ = expected;
            diagnostic_ = Diagnostic::None;
            phase_ = Phase::Prepared;
        } catch (...) {
            phase_ = Phase::Idle;
            throw;
        }
    }
    void abort_before_attempt() {
        require(Phase::Prepared);
        discard_precommit();
        phase_ = Phase::Aborted;
    }
    void begin_external_decision() {
        require(Phase::Prepared);
        phase_ = Phase::AwaitingExternalDecision;
    }
    // This closed, deterministic injection point cannot impersonate a real
    // authority: it exists only in the CHRONOS_TEST_ID_SEAM binary.
    void inject_test_decision(TestDecision decision) {
        require(Phase::AwaitingExternalDecision);
        if (decision == TestDecision::Refuse) {
            discard_precommit();
            diagnostic_ = Diagnostic::Refused;
            phase_ = Phase::Aborted;
            return;
        }
        if (decision == TestDecision::Ambiguous) {
            diagnostic_ = Diagnostic::Ambiguous;
            phase_ = Phase::FailedStop;
            return;
        }
        Token token = expected_; // constructed only by this test double
        switch (decision) {
        case TestDecision::Permit:
            break;
        case TestDecision::WrongCandidate:
            token.request_digest[0] ^= 1;
            break;
        case TestDecision::WrongSequence:
            ++token.sequence;
            break;
        case TestDecision::WrongIdentity:
            token.identity[0] ^= 1;
            break;
        case TestDecision::WrongConfiguration:
            token.config[0] ^= 1;
            break;
        case TestDecision::WrongPrefix:
            token.prefix_digest[0] ^= 1;
            break;
        case TestDecision::WrongLength:
            ++token.request_length;
            break;
        default:
            throw std::logic_error("unknown test decision");
        }
        if (!same(token, expected_)) {
            diagnostic_ = Diagnostic::BindingMismatch;
            phase_ = Phase::FailedStop;
            return;
        }
        token_ = token;
        phase_ = Phase::Authorized;
    }
    [[nodiscard]] bool install() {
        require(Phase::Authorized);
        if (!token_ || !same(*token_, expected_) ||
            journal::sha256(request_) != expected_.request_digest ||
            owner_.count_ + 1 != expected_.sequence || owner_.count_ >= owner_.capacity_ ||
            owner_.records_[owner_.count_] || !candidate_ || !prepared_) {
            diagnostic_ = Diagnostic::BindingMismatch;
            phase_ = Phase::FailedStop;
            return false;
        }
        static_assert(std::is_nothrow_swappable_v<std::unique_ptr<Exchange>>);
        static_assert(
            std::is_nothrow_swappable_v<std::unique_ptr<RestrictedReplay::InstalledRecord>>);
        owner_.state_.swap(candidate_);
        owner_.records_[owner_.count_].swap(prepared_);
        ++owner_.count_;
        token_.reset();
        phase_ = Phase::Installed;
        return true;
    }
    void reset_after_completion() {
        owner_.require_owner();
        if (phase_ != Phase::Installed && phase_ != Phase::Aborted)
            throw std::logic_error("test transaction is not complete");
        candidate_.reset();
        prepared_.reset();
        request_.clear();
        token_.reset();
        diagnostic_ = Diagnostic::None;
        phase_ = Phase::Idle;
    }
    // An authorized simulated decision is installed before a clean stop.
    // Awaiting is ambiguous and therefore becomes fail-stop.
    [[nodiscard]] bool shutdown() {
        owner_.require_owner();
        if (phase_ == Phase::AwaitingExternalDecision) {
            diagnostic_ = Diagnostic::ShutdownPending;
            phase_ = Phase::FailedStop;
            return false;
        }
        if (phase_ == Phase::FailedStop)
            return false;
        if (phase_ == Phase::Authorized && !install())
            return false;
        if (phase_ == Phase::Preparing)
            throw std::logic_error("cannot stop during preparation");
        candidate_.reset();
        prepared_.reset();
        request_.clear();
        token_.reset();
        phase_ = Phase::Stopped;
        return true;
    }
    // Test-only explicit operator handoff: no recovery or rollback is claimed.
    void acknowledge_unresolved_for_test() {
        require(Phase::FailedStop);
        phase_ = Phase::Stopped;
    }

  private:
    struct Token {
        Identity identity{};
        Digest config{};
        std::uint64_t sequence = 0, prefix_terminal = 0, prefix_offset = 0;
        Digest prefix_digest{};
        std::uint64_t request_length = 0;
        Digest request_digest{};
    };
    static bool same(const Token &a, const Token &b) noexcept {
        return a.identity == b.identity && a.config == b.config && a.sequence == b.sequence &&
               a.prefix_terminal == b.prefix_terminal && a.prefix_offset == b.prefix_offset &&
               a.prefix_digest == b.prefix_digest && a.request_length == b.request_length &&
               a.request_digest == b.request_digest;
    }
    static void put(RestrictedReplay::Bytes &b, std::uint64_t value) {
        for (int i = 7; i >= 0; --i)
            b.push_back(static_cast<std::uint8_t>(value >> (i * 8)));
    }
    void require(Phase expected) const {
        owner_.require_owner();
        if (phase_ != expected)
            throw std::logic_error("illegal test authorization transition");
    }
    void discard_precommit() noexcept {
        candidate_.reset();
        prepared_.reset();
        request_.clear();
        token_.reset();
    }
    RestrictedReplay owner_;
    Identity identity_;
    Phase phase_ = Phase::Idle;
    Diagnostic diagnostic_ = Diagnostic::None;
    std::unique_ptr<Exchange> candidate_;
    std::unique_ptr<RestrictedReplay::InstalledRecord> prepared_;
    RestrictedReplay::Bytes request_;
    Token expected_{};
    std::optional<Token> token_;
};
} // namespace chronos::exchange
