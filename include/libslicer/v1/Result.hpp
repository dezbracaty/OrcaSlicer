#pragma once

#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace libslicer::v1 {

enum class ErrorCode {
    invalid_argument,
    not_found,
    conflict,
    busy,
    unsupported,
    io,
    resource_limit_exceeded,
    invalid_configuration,
    cancelled,
    slicing_failed,
    internal
};

enum class Severity { warning, error };

struct Diagnostic {
    ErrorCode   code;
    Severity    severity;
    std::string message;
    std::string field;
};

namespace detail {
struct ResultAccess;
}

template<class T> class Result {
public:
    bool has_value() const noexcept { return state_ && state_->value.has_value(); }

    std::optional<ErrorCode> error_code() const noexcept
    {
        return state_ ? state_->error_code : std::optional<ErrorCode>{ErrorCode::internal};
    }

    T &value() &
    {
        ensure_value();
        return *state_->value;
    }

    const T &value() const &
    {
        ensure_value();
        return *state_->value;
    }

    T &&value() &&
    {
        ensure_value();
        return std::move(*state_->value);
    }

    const std::vector<Diagnostic> &diagnostics() const noexcept
    {
        static const std::vector<Diagnostic> empty;
        return state_ ? state_->diagnostics : empty;
    }

private:
    struct State {
        std::optional<T>          value;
        std::optional<ErrorCode>  error_code;
        std::vector<Diagnostic>   diagnostics;
    };

    explicit Result(std::shared_ptr<State> state) : state_(std::move(state)) {}

    void ensure_value() const
    {
        if (!has_value())
            throw std::logic_error("Result::value() called on a failed result");
    }

    std::shared_ptr<State> state_;
    friend struct detail::ResultAccess;
};

template<> class Result<void> {
public:
    bool has_value() const noexcept { return state_ && state_->succeeded; }

    std::optional<ErrorCode> error_code() const noexcept
    {
        return state_ ? state_->error_code : std::optional<ErrorCode>{ErrorCode::internal};
    }

    void value() const
    {
        if (!has_value())
            throw std::logic_error("Result<void>::value() called on a failed result");
    }

    const std::vector<Diagnostic> &diagnostics() const noexcept
    {
        static const std::vector<Diagnostic> empty;
        return state_ ? state_->diagnostics : empty;
    }

private:
    struct State {
        bool                     succeeded {false};
        std::optional<ErrorCode> error_code;
        std::vector<Diagnostic>  diagnostics;
    };

    explicit Result(std::shared_ptr<State> state) : state_(std::move(state)) {}

    std::shared_ptr<State> state_;
    friend struct detail::ResultAccess;
};

namespace detail {

struct ResultAccess {
    template<class T>
    static Result<T> success(T value, std::vector<Diagnostic> diagnostics = {})
    {
        using State = typename Result<T>::State;
        auto state = std::make_shared<State>();
        state->value.emplace(std::move(value));
        state->diagnostics = std::move(diagnostics);
        return Result<T>(std::move(state));
    }

    template<class T>
    static Result<T> failure(ErrorCode code, std::string message, std::string field,
                             std::vector<Diagnostic> diagnostics = {})
    {
        using State = typename Result<T>::State;
        auto state = std::make_shared<State>();
        state->error_code = code;
        state->diagnostics.reserve(diagnostics.size() + 1);
        state->diagnostics.push_back({code, Severity::error, std::move(message), std::move(field)});
        state->diagnostics.insert(state->diagnostics.end(),
                                  std::make_move_iterator(diagnostics.begin()),
                                  std::make_move_iterator(diagnostics.end()));
        return Result<T>(std::move(state));
    }

    static Result<void> success(std::vector<Diagnostic> diagnostics = {})
    {
        auto state = std::make_shared<Result<void>::State>();
        state->succeeded = true;
        state->diagnostics = std::move(diagnostics);
        return Result<void>(std::move(state));
    }

    static Result<void> failure(ErrorCode code, std::string message, std::string field,
                                std::vector<Diagnostic> diagnostics = {})
    {
        auto state = std::make_shared<Result<void>::State>();
        state->error_code = code;
        state->diagnostics.reserve(diagnostics.size() + 1);
        state->diagnostics.push_back({code, Severity::error, std::move(message), std::move(field)});
        state->diagnostics.insert(state->diagnostics.end(),
                                  std::make_move_iterator(diagnostics.begin()),
                                  std::make_move_iterator(diagnostics.end()));
        return Result<void>(std::move(state));
    }
};

} // namespace detail
} // namespace libslicer::v1
