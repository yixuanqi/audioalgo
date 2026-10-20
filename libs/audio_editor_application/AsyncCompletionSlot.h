#pragma once

#include <cstdint>
#include <functional>
#include <utility>

namespace audioalgo {

// Owns one asynchronous completion and gives each replacement a distinct token.
template <class Result>
class AsyncCompletionSlot {
public:
    using Token = std::uint64_t;
    using Completion = std::function<void(Result)>;

    [[nodiscard]] Token begin(Completion completion) {
        if (closed_)
            return 0;
        advance();
        completion_ = std::move(completion);
        return generation_;
    }

    void invalidate() noexcept {
        advance();
        completion_ = {};
    }

    // Permanently prevents new completions after the owning view starts closing.
    // A reopened view owns a new slot, so stale callbacks stay scoped to this one.
    void close() noexcept {
        if (closed_)
            return;
        invalidate();
        closed_ = true;
    }

    [[nodiscard]] Completion take(Token token) {
        if (token == 0 || token != generation_)
            return {};
        auto completion = std::move(completion_);
        completion_ = {};
        return completion;
    }

private:
    void advance() noexcept {
        ++generation_;
        if (generation_ == 0)
            ++generation_;
    }

    Token generation_ = 0;
    Completion completion_;
    bool closed_ = false;
};

} // namespace audioalgo
