// Copyright 2020 Francesco Biscani (bluescarni@gmail.com), Dario Izzo (dario.izzo@gmail.com)
//
// This file is part of the heyoka library.
//
// This Source Code Form is subject to the terms of the Mozilla
// Public License v. 2.0. If a copy of the MPL was not distributed
// with this file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <cstddef>
#include <ostream>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include <llvm/IR/Value.h>

#include <heyoka/binary_operator.hpp>
#include <heyoka/detail/assert_nonnull_ret.hpp>
#include <heyoka/detail/type_traits.hpp>
#include <heyoka/expression.hpp>
#include <heyoka/function.hpp>
#include <heyoka/llvm_state.hpp>
#include <heyoka/number.hpp>
#include <heyoka/variable.hpp>

namespace heyoka
{

expression::expression(number n) : m_value(std::move(n)) {}

expression::expression(variable var) : m_value(std::move(var)) {}

expression::expression(binary_operator bo) : m_value(std::move(bo)) {}

expression::expression(function f) : m_value(std::move(f)) {}

expression::expression(const expression &) = default;

expression::expression(expression &&) noexcept = default;

expression::~expression() = default;

expression &expression::operator=(const expression &) = default;

expression &expression::operator=(expression &&) noexcept = default;

expression::value_type &expression::value()
{
    return m_value;
}

const expression::value_type &expression::value() const
{
    return m_value;
}

inline namespace literals
{

expression operator""_dbl(long double x)
{
    return expression{number{static_cast<double>(x)}};
}

expression operator""_dbl(unsigned long long n)
{
    return expression{number{static_cast<double>(n)}};
}

expression operator""_ldbl(long double x)
{
    return expression{number{x}};
}

expression operator""_ldbl(unsigned long long n)
{
    return expression{number{static_cast<long double>(n)}};
}

expression operator""_var(const char *s, std::size_t n)
{
    return expression{variable{std::string{s, n}}};
}

} // namespace literals

std::vector<std::string> get_variables(const expression &e)
{
    return std::visit([](const auto &arg) { return get_variables(arg); }, e.value());
}

void rename_variables(expression &e, const std::unordered_map<std::string, std::string> &repl_map)
{
    std::visit([&repl_map](auto &arg) { rename_variables(arg, repl_map); }, e.value());
}

void swap(expression &ex0, expression &ex1) noexcept
{
    using std::swap;
    swap(ex0.value(), ex1.value());
}

std::size_t hash(const expression &ex)
{
    return std::visit([](const auto &v) { return hash(v); }, ex.value());
}

std::ostream &operator<<(std::ostream &os, const expression &e)
{
    return std::visit([&os](const auto &arg) -> std::ostream & { return os << arg; }, e.value());
}

expression operator+(expression e)
{
    return e;
}

expression operator-(expression e)
{
    return expression{number{-1.}} * std::move(e);
}

expression operator+(expression e1, expression e2)
{
    auto visitor = [](auto &&v1, auto &&v2) {
        using type1 = detail::uncvref_t<decltype(v1)>;
        using type2 = detail::uncvref_t<decltype(v2)>;

        if constexpr (std::is_same_v<type1, number> && std::is_same_v<type2, number>) {
            // Both are numbers, add them.
            return expression{std::forward<decltype(v1)>(v1) + std::forward<decltype(v2)>(v2)};
        } else if constexpr (std::is_same_v<type1, number>) {
            // e1 number, e2 symbolic.
            if (is_zero(v1)) {
                // 0 + e2 = e2.
                return expression{std::forward<decltype(v2)>(v2)};
            }
            // NOTE: fall through the standard case if e1 is not zero.
        } else if constexpr (std::is_same_v<type2, number>) {
            // e1 symbolic, e2 number.
            if (is_zero(v2)) {
                // e1 + 0 = e1.
                return expression{std::forward<decltype(v1)>(v1)};
            }
            // NOTE: fall through the standard case if e2 is not zero.
        }

        // The standard case.
        return expression{binary_operator{binary_operator::type::add, expression{std::forward<decltype(v1)>(v1)},
                                          expression{std::forward<decltype(v2)>(v2)}}};
    };

    return std::visit(visitor, std::move(e1.value()), std::move(e2.value()));
}

expression operator-(expression e1, expression e2)
{
    auto visitor = [](auto &&v1, auto &&v2) {
        using type1 = detail::uncvref_t<decltype(v1)>;
        using type2 = detail::uncvref_t<decltype(v2)>;

        if constexpr (std::is_same_v<type1, number> && std::is_same_v<type2, number>) {
            // Both are numbers, subtract them.
            return expression{std::forward<decltype(v1)>(v1) - std::forward<decltype(v2)>(v2)};
        } else if constexpr (std::is_same_v<type1, number>) {
            // e1 number, e2 symbolic.
            if (is_zero(v1)) {
                // 0 - e2 = -e2.
                return -expression{std::forward<decltype(v2)>(v2)};
            }
            // NOTE: fall through the standard case if e1 is not zero.
        } else if constexpr (std::is_same_v<type2, number>) {
            // e1 symbolic, e2 number.
            if (is_zero(v2)) {
                // e1 - 0 = e1.
                return expression{std::forward<decltype(v1)>(v1)};
            }
            // NOTE: fall through the standard case if e2 is not zero.
        }

        // The standard case.
        return expression{binary_operator{binary_operator::type::sub, expression{std::forward<decltype(v1)>(v1)},
                                          expression{std::forward<decltype(v2)>(v2)}}};
    };

    return std::visit(visitor, std::move(e1.value()), std::move(e2.value()));
}

expression operator*(expression e1, expression e2)
{
    auto visitor = [](auto &&v1, auto &&v2) {
        using type1 = detail::uncvref_t<decltype(v1)>;
        using type2 = detail::uncvref_t<decltype(v2)>;

        if constexpr (std::is_same_v<type1, number> && std::is_same_v<type2, number>) {
            // Both are numbers, multiply them.
            return expression{std::forward<decltype(v1)>(v1) * std::forward<decltype(v2)>(v2)};
        } else if constexpr (std::is_same_v<type1, number>) {
            // e1 number, e2 symbolic.
            if (is_zero(v1)) {
                // 0 * e2 = 0.
                return expression{number{0.}};
            } else if (is_one(v1)) {
                // 1 * e2 = e2.
                return expression{std::forward<decltype(v2)>(v2)};
            }
            // NOTE: fall through the standard case if e1 is not zero or one.
        } else if constexpr (std::is_same_v<type2, number>) {
            // e1 symbolic, e2 number.
            if (is_zero(v2)) {
                // e1 * 0 = 0.
                return expression{number{0.}};
            } else if (is_one(v2)) {
                // e1 * 1 = e1.
                return expression{std::forward<decltype(v1)>(v1)};
            }
            // NOTE: fall through the standard case if e1 is not zero or one.
        }

        // The standard case.
        return expression{binary_operator{binary_operator::type::mul, expression{std::forward<decltype(v1)>(v1)},
                                          expression{std::forward<decltype(v2)>(v2)}}};
    };

    return std::visit(visitor, std::move(e1.value()), std::move(e2.value()));
}

expression operator/(expression e1, expression e2)
{
    auto visitor = [](auto &&v1, auto &&v2) {
        using type1 = detail::uncvref_t<decltype(v1)>;
        using type2 = detail::uncvref_t<decltype(v2)>;

        if constexpr (std::is_same_v<type1, number> && std::is_same_v<type2, number>) {
            // Both are numbers, divide them.
            return expression{std::forward<decltype(v1)>(v1) / std::forward<decltype(v2)>(v2)};
        } else if constexpr (std::is_same_v<type2, number>) {
            // e1 is symbolic, e2 a number.
            if (is_one(v2) == 1) {
                // e1 / 1 = e1.
                return expression{std::forward<decltype(v1)>(v1)};
            } else if (is_negative_one(v2)) {
                // e1 / -1 = -e1.
                return -expression{std::forward<decltype(v1)>(v1)};
            } else {
                // e1 / x = e1 * 1/x.
                return expression{std::forward<decltype(v1)>(v1)}
                       * expression{number{1.} / std::forward<decltype(v2)>(v2)};
            }
        } else {
            // The standard case.
            return expression{binary_operator{binary_operator::type::div, expression{std::forward<decltype(v1)>(v1)},
                                              expression{std::forward<decltype(v2)>(v2)}}};
        }
    };

    return std::visit(visitor, std::move(e1.value()), std::move(e2.value()));
}

bool operator==(const expression &e1, const expression &e2)
{
    auto visitor = [](const auto &v1, const auto &v2) {
        using type1 = detail::uncvref_t<decltype(v1)>;
        using type2 = detail::uncvref_t<decltype(v2)>;

        if constexpr (std::is_same_v<type1, type2>) {
            return v1 == v2;
        } else {
            return false;
        }
    };

    return std::visit(visitor, e1.value(), e2.value());
}

bool operator!=(const expression &e1, const expression &e2)
{
    return !(e1 == e2);
}

expression diff(const expression &e, const std::string &s)
{
    return std::visit([&s](const auto &arg) { return diff(arg, s); }, e.value());
}

expression subs(const expression &e, const std::unordered_map<std::string, expression> &smap)
{
    return std::visit([&smap](const auto &arg) { return subs(arg, smap); }, e.value());
}

double eval_dbl(const expression &e, const std::unordered_map<std::string, double> &map)
{
    return std::visit([&map](const auto &arg) { return eval_dbl(arg, map); }, e.value());
}

void eval_batch_dbl(std::vector<double> &retval, const expression &e,
                    const std::unordered_map<std::string, std::vector<double>> &map)
{
    std::visit([&map, &retval](const auto &arg) { eval_batch_dbl(retval, arg, map); }, e.value());
}

std::vector<std::vector<std::size_t>> compute_connections(const expression &e)
{
    std::vector<std::vector<std::size_t>> node_connections;
    std::size_t node_counter = 0u;
    update_connections(node_connections, e, node_counter);
    return node_connections;
}

void update_connections(std::vector<std::vector<std::size_t>> &node_connections, const expression &e,
                        std::size_t &node_counter)
{
    std::visit([&node_connections,
                &node_counter](const auto &arg) { update_connections(node_connections, arg, node_counter); },
               e.value());
}

std::vector<double> compute_node_values_dbl(const expression &e, const std::unordered_map<std::string, double> &map,
                                            const std::vector<std::vector<std::size_t>> &node_connections)
{
    std::vector<double> node_values(node_connections.size());
    std::size_t node_counter = 0u;
    update_node_values_dbl(node_values, e, map, node_connections, node_counter);
    return node_values;
}

void update_node_values_dbl(std::vector<double> &node_values, const expression &e,
                            const std::unordered_map<std::string, double> &map,
                            const std::vector<std::vector<std::size_t>> &node_connections, std::size_t &node_counter)
{
    std::visit([&map, &node_values, &node_connections, &node_counter](
                   const auto &arg) { update_node_values_dbl(node_values, arg, map, node_connections, node_counter); },
               e.value());
}

std::unordered_map<std::string, double> compute_grad_dbl(const expression &e,
                                                         const std::unordered_map<std::string, double> &map,
                                                         const std::vector<std::vector<std::size_t>> &node_connections)
{
    std::unordered_map<std::string, double> grad;
    auto node_values = compute_node_values_dbl(e, map, node_connections);
    std::size_t node_counter = 0u;
    update_grad_dbl(grad, e, map, node_values, node_connections, node_counter);
    return grad;
}

void update_grad_dbl(std::unordered_map<std::string, double> &grad, const expression &e,
                     const std::unordered_map<std::string, double> &map, const std::vector<double> &node_values,
                     const std::vector<std::vector<std::size_t>> &node_connections, std::size_t &node_counter,
                     double acc)
{
    std::visit(
        [&map, &grad, &node_values, &node_connections, &node_counter, &acc](const auto &arg) {
            update_grad_dbl(grad, arg, map, node_values, node_connections, node_counter, acc);
        },
        e.value());
}

llvm::Value *codegen_dbl(llvm_state &s, const expression &e)
{
    heyoka_assert_nonnull_ret(std::visit([&s](const auto &arg) { return codegen_dbl(s, arg); }, e.value()));
}

llvm::Value *codegen_ldbl(llvm_state &s, const expression &e)
{
    heyoka_assert_nonnull_ret(std::visit([&s](const auto &arg) { return codegen_ldbl(s, arg); }, e.value()));
}

} // namespace heyoka
