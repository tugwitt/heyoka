// Copyright 2020 Francesco Biscani (bluescarni@gmail.com), Dario Izzo (dario.izzo@gmail.com)
//
// This file is part of the heyoka library.
//
// This Source Code Form is subject to the terms of the Mozilla
// Public License v. 2.0. If a copy of the MPL was not distributed
// with this file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <heyoka/config.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <memory>
#include <ostream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include <llvm/IR/Attributes.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Value.h>

#if defined(HEYOKA_HAVE_REAL128)

#include <mp++/real128.hpp>

#endif

#include <heyoka/binary_operator.hpp>
#include <heyoka/detail/llvm_helpers.hpp>
#include <heyoka/detail/string_conv.hpp>
#include <heyoka/detail/type_traits.hpp>
#include <heyoka/expression.hpp>
#include <heyoka/llvm_state.hpp>
#include <heyoka/number.hpp>
#include <heyoka/taylor.hpp>
#include <heyoka/variable.hpp>

namespace heyoka
{

binary_operator::binary_operator(type t, expression e1, expression e2)
    : m_type(t),
      // NOTE: need to use naked new as make_unique won't work with aggregate
      // initialization.
      m_ops(::new std::array<expression, 2>{std::move(e1), std::move(e2)})
{
}

binary_operator::binary_operator(const binary_operator &other)
    : m_type(other.m_type), m_ops(std::make_unique<std::array<expression, 2>>(*other.m_ops))
{
}

binary_operator::binary_operator(binary_operator &&) noexcept = default;

binary_operator::~binary_operator() = default;

binary_operator &binary_operator::operator=(const binary_operator &bo)
{
    if (this != &bo) {
        *this = binary_operator(bo);
    }
    return *this;
}

binary_operator &binary_operator::operator=(binary_operator &&) noexcept = default;

expression &binary_operator::lhs()
{
    assert(m_ops);
    return (*m_ops)[0];
}

expression &binary_operator::rhs()
{
    assert(m_ops);
    return (*m_ops)[1];
}

binary_operator::type &binary_operator::op()
{
    assert(m_type >= type::add && m_type <= type::div);
    return m_type;
}

const expression &binary_operator::lhs() const
{
    assert(m_ops);
    return (*m_ops)[0];
}

const expression &binary_operator::rhs() const
{
    assert(m_ops);
    return (*m_ops)[1];
}

const binary_operator::type &binary_operator::op() const
{
    assert(m_type >= type::add && m_type <= type::div);
    return m_type;
}

void swap(binary_operator &bo0, binary_operator &bo1) noexcept
{
    std::swap(bo0.m_type, bo1.m_type);
    std::swap(bo0.m_ops, bo1.m_ops);
}

std::size_t hash(const binary_operator &bo)
{
    return std::hash<binary_operator::type>{}(bo.op()) + hash(bo.lhs()) + hash(bo.rhs());
}

std::ostream &operator<<(std::ostream &os, const binary_operator &bo)
{
    os << '(' << bo.lhs() << ' ';

    switch (bo.op()) {
        case binary_operator::type::add:
            os << '+';
            break;
        case binary_operator::type::sub:
            os << '-';
            break;
        case binary_operator::type::mul:
            os << '*';
            break;
        case binary_operator::type::div:
            os << '/';
            break;
    }

    return os << ' ' << bo.rhs() << ')';
}

std::vector<std::string> get_variables(const binary_operator &bo)
{
    auto lhs_vars = get_variables(bo.lhs());
    auto rhs_vars = get_variables(bo.rhs());

    lhs_vars.insert(lhs_vars.end(), std::make_move_iterator(rhs_vars.begin()), std::make_move_iterator(rhs_vars.end()));

    std::sort(lhs_vars.begin(), lhs_vars.end());
    lhs_vars.erase(std::unique(lhs_vars.begin(), lhs_vars.end()), lhs_vars.end());

    return lhs_vars;
}

void rename_variables(binary_operator &bo, const std::unordered_map<std::string, std::string> &repl_map)
{
    rename_variables(bo.lhs(), repl_map);
    rename_variables(bo.rhs(), repl_map);
}

bool operator==(const binary_operator &o1, const binary_operator &o2)
{
    return o1.op() == o2.op() && o1.lhs() == o2.lhs() && o1.rhs() == o2.rhs();
}

bool operator!=(const binary_operator &o1, const binary_operator &o2)
{
    return !(o1 == o2);
}

expression subs(const binary_operator &bo, const std::unordered_map<std::string, expression> &smap)
{
    return expression{binary_operator{bo.op(), subs(bo.lhs(), smap), subs(bo.rhs(), smap)}};
}

expression diff(const binary_operator &bo, const std::string &s)
{
    switch (bo.op()) {
        case binary_operator::type::add:
            return diff(bo.lhs(), s) + diff(bo.rhs(), s);
        case binary_operator::type::sub:
            return diff(bo.lhs(), s) - diff(bo.rhs(), s);
        case binary_operator::type::mul:
            return diff(bo.lhs(), s) * bo.rhs() + bo.lhs() * diff(bo.rhs(), s);
        default:
            return (diff(bo.lhs(), s) * bo.rhs() - bo.lhs() * diff(bo.rhs(), s)) / (bo.rhs() * bo.rhs());
    }
}

double eval_dbl(const binary_operator &bo, const std::unordered_map<std::string, double> &map)
{
    switch (bo.op()) {
        case binary_operator::type::add:
            return eval_dbl(bo.lhs(), map) + eval_dbl(bo.rhs(), map);
        case binary_operator::type::sub:
            return eval_dbl(bo.lhs(), map) - eval_dbl(bo.rhs(), map);
        case binary_operator::type::mul:
            return eval_dbl(bo.lhs(), map) * eval_dbl(bo.rhs(), map);
        default:
            return eval_dbl(bo.lhs(), map) / eval_dbl(bo.rhs(), map);
    }
}

void eval_batch_dbl(std::vector<double> &out_values, const binary_operator &bo,
                    const std::unordered_map<std::string, std::vector<double>> &map)
{
    auto tmp = out_values;
    eval_batch_dbl(out_values, bo.lhs(), map);
    eval_batch_dbl(tmp, bo.rhs(), map);
    switch (bo.op()) {
        case binary_operator::type::add:
            std::transform(out_values.begin(), out_values.end(), tmp.begin(), out_values.begin(), std::plus<>());
            break;
        case binary_operator::type::sub:
            std::transform(out_values.begin(), out_values.end(), tmp.begin(), out_values.begin(), std::minus<>());
            break;
        case binary_operator::type::mul:
            std::transform(out_values.begin(), out_values.end(), tmp.begin(), out_values.begin(), std::multiplies<>());
            break;
        default:
            std::transform(out_values.begin(), out_values.end(), tmp.begin(), out_values.begin(), std::divides<>());
            break;
    }
}

void update_node_values_dbl(std::vector<double> &node_values, const binary_operator &bo,
                            const std::unordered_map<std::string, double> &map,
                            const std::vector<std::vector<std::size_t>> &node_connections, std::size_t &node_counter)
{
    const auto node_id = node_counter;
    node_counter++;
    // We have to recurse first as to make sure out is filled before being accessed later.
    update_node_values_dbl(node_values, bo.lhs(), map, node_connections, node_counter);
    update_node_values_dbl(node_values, bo.rhs(), map, node_connections, node_counter);
    switch (bo.op()) {
        case binary_operator::type::add:
            node_values[node_id]
                = node_values[node_connections[node_id][0]] + node_values[node_connections[node_id][1]];
            break;
        case binary_operator::type::sub:
            node_values[node_id]
                = node_values[node_connections[node_id][0]] - node_values[node_connections[node_id][1]];
            break;
        case binary_operator::type::mul:
            node_values[node_id]
                = node_values[node_connections[node_id][0]] * node_values[node_connections[node_id][1]];
            break;
        default:
            node_values[node_id]
                = node_values[node_connections[node_id][0]] / node_values[node_connections[node_id][1]];
            break;
    }
}

void update_grad_dbl(std::unordered_map<std::string, double> &grad, const binary_operator &bo,
                     const std::unordered_map<std::string, double> &map, const std::vector<double> &node_values,
                     const std::vector<std::vector<std::size_t>> &node_connections, std::size_t &node_counter,
                     double acc)
{
    const auto node_id = node_counter;
    node_counter++;
    switch (bo.op()) {
        case binary_operator::type::add:
            // lhs (a + b -> 1)
            update_grad_dbl(grad, bo.lhs(), map, node_values, node_connections, node_counter, acc);
            // rhs (a + b -> 1)
            update_grad_dbl(grad, bo.rhs(), map, node_values, node_connections, node_counter, acc);
            break;

        case binary_operator::type::sub:
            // lhs (a + b -> 1)
            update_grad_dbl(grad, bo.lhs(), map, node_values, node_connections, node_counter, acc);
            // rhs (a + b -> 1)
            update_grad_dbl(grad, bo.rhs(), map, node_values, node_connections, node_counter, -acc);
            break;

        case binary_operator::type::mul:
            // lhs (a*b -> b)
            update_grad_dbl(grad, bo.lhs(), map, node_values, node_connections, node_counter,
                            acc * node_values[node_connections[node_id][1]]);
            // rhs (a*b -> a)
            update_grad_dbl(grad, bo.rhs(), map, node_values, node_connections, node_counter,
                            acc * node_values[node_connections[node_id][0]]);
            break;

        default:
            // lhs (a/b -> 1/b)
            update_grad_dbl(grad, bo.lhs(), map, node_values, node_connections, node_counter,
                            acc / node_values[node_connections[node_id][1]]);
            // rhs (a/b -> -a/b^2)
            update_grad_dbl(grad, bo.rhs(), map, node_values, node_connections, node_counter,
                            -acc * node_values[node_connections[node_id][0]] / node_values[node_connections[node_id][1]]
                                / node_values[node_connections[node_id][1]]);
            break;
    }
}

void update_connections(std::vector<std::vector<std::size_t>> &node_connections, const binary_operator &bo,
                        std::size_t &node_counter)
{
    const auto node_id = node_counter;
    node_counter++;
    node_connections.push_back(std::vector<std::size_t>(2));
    node_connections[node_id][0] = node_counter;
    update_connections(node_connections, bo.lhs(), node_counter);
    node_connections[node_id][1] = node_counter;
    update_connections(node_connections, bo.rhs(), node_counter);
}

namespace detail
{

namespace
{

template <typename T>
llvm::Value *bo_codegen_impl(llvm_state &s, const binary_operator &bo)
{
    auto *l = codegen<T>(s, bo.lhs());
    auto *r = codegen<T>(s, bo.rhs());

    auto &builder = s.builder();

    switch (bo.op()) {
        case binary_operator::type::add:
            return builder.CreateFAdd(l, r, "addtmp");
        case binary_operator::type::sub:
            return builder.CreateFSub(l, r, "subtmp");
        case binary_operator::type::mul:
            return builder.CreateFMul(l, r, "multmp");
        default:
            return builder.CreateFDiv(l, r, "divtmp");
    }
}

} // namespace

} // namespace detail

llvm::Value *codegen_dbl(llvm_state &s, const binary_operator &bo)
{
    return detail::bo_codegen_impl<double>(s, bo);
}

llvm::Value *codegen_ldbl(llvm_state &s, const binary_operator &bo)
{
    return detail::bo_codegen_impl<long double>(s, bo);
}

#if defined(HEYOKA_HAVE_REAL128)

llvm::Value *codegen_f128(llvm_state &s, const binary_operator &bo)
{
    return detail::bo_codegen_impl<mppp::real128>(s, bo);
}

#endif

std::vector<expression>::size_type taylor_decompose_in_place(binary_operator &&bo, std::vector<expression> &u_vars_defs)
{
    if (const auto dres_lhs = taylor_decompose_in_place(std::move(bo.lhs()), u_vars_defs)) {
        // The lhs required decomposition, and its decomposition
        // was placed at index dres_lhs in u_vars_defs. Replace the lhs
        // a u variable pointing at index dres_lhs.
        bo.lhs() = expression{variable{"u_" + detail::li_to_string(dres_lhs)}};
    }

    if (const auto dres_rhs = taylor_decompose_in_place(std::move(bo.rhs()), u_vars_defs)) {
        bo.rhs() = expression{variable{"u_" + detail::li_to_string(dres_rhs)}};
    }

    // Append the binary operator after decomposition
    // of lhs and rhs.
    u_vars_defs.emplace_back(std::move(bo));

    // The decomposition of binary operators
    // results in a new u variable, whose definition
    // we added to u_vars_defs above.
    return u_vars_defs.size() - 1u;
}

namespace detail
{

namespace
{

template <typename T>
llvm::Value *taylor_u_init_bo_impl(llvm_state &s, const binary_operator &bo, const std::vector<llvm::Value *> &arr,
                                   std::uint32_t batch_size)
{
    // Do the Taylor init for lhs and rhs.
    auto l = taylor_u_init<T>(s, bo.lhs(), arr, batch_size);
    auto r = taylor_u_init<T>(s, bo.rhs(), arr, batch_size);

    // Do the codegen for the corresponding operation.
    switch (bo.op()) {
        case binary_operator::type::add:
            return s.builder().CreateFAdd(l, r);
        case binary_operator::type::sub:
            return s.builder().CreateFSub(l, r);
        case binary_operator::type::mul:
            return s.builder().CreateFMul(l, r);
        default:
            return s.builder().CreateFDiv(l, r);
    }
}

} // namespace

} // namespace detail

llvm::Value *taylor_u_init_dbl(llvm_state &s, const binary_operator &bo, const std::vector<llvm::Value *> &arr,
                               std::uint32_t batch_size)
{
    return detail::taylor_u_init_bo_impl<double>(s, bo, arr, batch_size);
}

llvm::Value *taylor_u_init_ldbl(llvm_state &s, const binary_operator &bo, const std::vector<llvm::Value *> &arr,
                                std::uint32_t batch_size)
{
    return detail::taylor_u_init_bo_impl<long double>(s, bo, arr, batch_size);
}

#if defined(HEYOKA_HAVE_REAL128)

llvm::Value *taylor_u_init_f128(llvm_state &s, const binary_operator &bo, const std::vector<llvm::Value *> &arr,
                                std::uint32_t batch_size)
{
    return detail::taylor_u_init_bo_impl<mppp::real128>(s, bo, arr, batch_size);
}

#endif

namespace detail
{

namespace
{

// Derivative of number +- number.
template <bool, typename T>
llvm::Value *bo_taylor_diff_addsub_impl(llvm_state &s, const number &, const number &,
                                        const std::vector<llvm::Value *> &, std::uint32_t, std::uint32_t, std::uint32_t,
                                        std::uint32_t batch_size)
{
    return vector_splat(s.builder(), codegen<T>(s, number{0.}), batch_size);
}

// Derivative of number +- var.
template <bool AddOrSub, typename T>
llvm::Value *bo_taylor_diff_addsub_impl(llvm_state &s, const number &, const variable &var,
                                        const std::vector<llvm::Value *> &arr, std::uint32_t n_uvars,
                                        std::uint32_t order, std::uint32_t, std::uint32_t)
{
    auto ret = taylor_fetch_diff(arr, uname_to_index(var.name()), order, n_uvars);

    if constexpr (AddOrSub) {
        return ret;
    } else {
        // Negate if we are doing a subtraction.
        return s.builder().CreateFNeg(ret);
    }
}

// Derivative of var +- number.
template <bool AddOrSub, typename T>
llvm::Value *bo_taylor_diff_addsub_impl(llvm_state &, const variable &var, const number &,
                                        const std::vector<llvm::Value *> &arr, std::uint32_t n_uvars,
                                        std::uint32_t order, std::uint32_t, std::uint32_t)
{
    return taylor_fetch_diff(arr, uname_to_index(var.name()), order, n_uvars);
}

// Derivative of var +- var.
template <bool AddOrSub, typename T>
llvm::Value *bo_taylor_diff_addsub_impl(llvm_state &s, const variable &var0, const variable &var1,
                                        const std::vector<llvm::Value *> &arr, std::uint32_t n_uvars,
                                        std::uint32_t order, std::uint32_t, std::uint32_t)
{
    auto v0 = taylor_fetch_diff(arr, uname_to_index(var0.name()), order, n_uvars);
    auto v1 = taylor_fetch_diff(arr, uname_to_index(var1.name()), order, n_uvars);

    if constexpr (AddOrSub) {
        return s.builder().CreateFAdd(v0, v1);
    } else {
        return s.builder().CreateFSub(v0, v1);
    }
}

// All the other cases.
template <bool, typename, typename V1, typename V2>
llvm::Value *bo_taylor_diff_addsub_impl(llvm_state &, const V1 &, const V2 &, const std::vector<llvm::Value *> &,
                                        std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t)
{
    assert(false);

    return nullptr;
}

template <typename T>
llvm::Value *bo_taylor_diff_add(llvm_state &s, const binary_operator &bo, const std::vector<llvm::Value *> &arr,
                                std::uint32_t n_uvars, std::uint32_t order, std::uint32_t idx, std::uint32_t batch_size)
{
    return std::visit(
        [&](const auto &v1, const auto &v2) {
            return bo_taylor_diff_addsub_impl<true, T>(s, v1, v2, arr, n_uvars, order, idx, batch_size);
        },
        bo.lhs().value(), bo.rhs().value());
}

template <typename T>
llvm::Value *bo_taylor_diff_sub(llvm_state &s, const binary_operator &bo, const std::vector<llvm::Value *> &arr,
                                std::uint32_t n_uvars, std::uint32_t order, std::uint32_t idx, std::uint32_t batch_size)
{
    return std::visit(
        [&](const auto &v1, const auto &v2) {
            return bo_taylor_diff_addsub_impl<false, T>(s, v1, v2, arr, n_uvars, order, idx, batch_size);
        },
        bo.lhs().value(), bo.rhs().value());
}

// Derivative of number * number.
template <typename T>
llvm::Value *bo_taylor_diff_mul_impl(llvm_state &s, const number &, const number &, const std::vector<llvm::Value *> &,
                                     std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t batch_size)
{
    return vector_splat(s.builder(), codegen<T>(s, number{0.}), batch_size);
}

// Derivative of var * number.
template <typename T>
llvm::Value *bo_taylor_diff_mul_impl(llvm_state &s, const variable &var, const number &num,
                                     const std::vector<llvm::Value *> &arr, std::uint32_t n_uvars, std::uint32_t order,
                                     std::uint32_t, std::uint32_t batch_size)
{
    auto &builder = s.builder();

    auto ret = taylor_fetch_diff(arr, uname_to_index(var.name()), order, n_uvars);
    auto mul = vector_splat(builder, codegen<T>(s, num), batch_size);

    return builder.CreateFMul(mul, ret);
}

// Derivative of number * var.
template <typename T>
llvm::Value *bo_taylor_diff_mul_impl(llvm_state &s, const number &num, const variable &var,
                                     const std::vector<llvm::Value *> &arr, std::uint32_t n_uvars, std::uint32_t order,
                                     std::uint32_t idx, std::uint32_t batch_size)
{
    return bo_taylor_diff_mul_impl<T>(s, var, num, arr, n_uvars, order, idx, batch_size);
}

// Derivative of var * var.
template <typename T>
llvm::Value *bo_taylor_diff_mul_impl(llvm_state &s, const variable &var0, const variable &var1,
                                     const std::vector<llvm::Value *> &arr, std::uint32_t n_uvars, std::uint32_t order,
                                     std::uint32_t, std::uint32_t)
{
    // Fetch the indices of the u variables.
    const auto u_idx0 = uname_to_index(var0.name());
    const auto u_idx1 = uname_to_index(var1.name());

    // NOTE: iteration in the [0, order] range
    // (i.e., order inclusive).
    std::vector<llvm::Value *> sum;
    auto &builder = s.builder();
    for (std::uint32_t j = 0; j <= order; ++j) {
        auto v0 = taylor_fetch_diff(arr, u_idx0, order - j, n_uvars);
        auto v1 = taylor_fetch_diff(arr, u_idx1, j, n_uvars);

        // Add v0*v1 to the sum.
        sum.push_back(builder.CreateFMul(v0, v1));
    }

    return pairwise_sum(builder, sum);
}

// All the other cases.
template <typename, typename V1, typename V2>
llvm::Value *bo_taylor_diff_mul_impl(llvm_state &, const V1 &, const V2 &, const std::vector<llvm::Value *> &,
                                     std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t)
{
    assert(false);

    return nullptr;
}

template <typename T>
llvm::Value *bo_taylor_diff_mul(llvm_state &s, const binary_operator &bo, const std::vector<llvm::Value *> &arr,
                                std::uint32_t n_uvars, std::uint32_t order, std::uint32_t idx, std::uint32_t batch_size)
{
    return std::visit(
        [&](const auto &v1, const auto &v2) {
            return bo_taylor_diff_mul_impl<T>(s, v1, v2, arr, n_uvars, order, idx, batch_size);
        },
        bo.lhs().value(), bo.rhs().value());
}

// Derivative of number / number.
template <typename T>
llvm::Value *bo_taylor_diff_div_impl(llvm_state &s, const number &, const number &, const std::vector<llvm::Value *> &,
                                     std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t batch_size)
{
    return vector_splat(s.builder(), codegen<T>(s, number{0.}), batch_size);
}

// Derivative of variable / variable or number / variable. These two cases
// are quite similar, so we handle them together.
template <typename T, typename U,
          std::enable_if_t<std::disjunction_v<std::is_same<U, number>, std::is_same<U, variable>>, int> = 0>
llvm::Value *bo_taylor_diff_div_impl(llvm_state &s, const U &nv, const variable &var1,
                                     const std::vector<llvm::Value *> &arr, std::uint32_t n_uvars, std::uint32_t order,
                                     std::uint32_t idx, std::uint32_t)
{
    // Fetch the index of var1.
    const auto u_idx1 = uname_to_index(var1.name());

    // NOTE: iteration in the [1, order] range
    // (i.e., order inclusive).
    auto &builder = s.builder();
    std::vector<llvm::Value *> sum;
    for (std::uint32_t j = 1; j <= order; ++j) {
        auto v0 = taylor_fetch_diff(arr, idx, order - j, n_uvars);
        auto v1 = taylor_fetch_diff(arr, u_idx1, j, n_uvars);

        // Add v0*v1 to the sum.
        sum.push_back(builder.CreateFMul(v0, v1));
    }

    // Init the return value as the result of the sum.
    auto ret_acc = pairwise_sum(builder, sum);

    // Load the divisor for the quotient formula.
    // This is the zero-th order derivative of var1.
    auto div = taylor_fetch_diff(arr, u_idx1, 0, n_uvars);

    if constexpr (std::is_same_v<U, number>) {
        // nv is a number. Negate the accumulator
        // and divide it by the divisor.
        return builder.CreateFDiv(builder.CreateFNeg(ret_acc), div);
    } else {
        // nv is a variable. We need to fetch its
        // derivative of order 'order' from the array of derivatives.
        auto diff_nv_v = taylor_fetch_diff(arr, uname_to_index(nv.name()), order, n_uvars);

        // Produce the result: (diff_nv_v - ret_acc) / div.
        return builder.CreateFDiv(builder.CreateFSub(diff_nv_v, ret_acc), div);
    }
}

// Derivative of variable / number.
template <typename T>
llvm::Value *bo_taylor_diff_div_impl(llvm_state &s, const variable &var, const number &num,
                                     const std::vector<llvm::Value *> &arr, std::uint32_t n_uvars, std::uint32_t order,
                                     std::uint32_t, std::uint32_t batch_size)
{
    auto &builder = s.builder();

    auto ret = taylor_fetch_diff(arr, uname_to_index(var.name()), order, n_uvars);
    auto div = vector_splat(builder, codegen<T>(s, num), batch_size);

    return builder.CreateFDiv(ret, div);
}

// All the other cases.
template <typename, typename V1, typename V2>
llvm::Value *bo_taylor_diff_div_impl(llvm_state &, const V1 &, const V2 &, const std::vector<llvm::Value *> &,
                                     std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t)
{
    assert(false);

    return nullptr;
}

template <typename T>
llvm::Value *bo_taylor_diff_div(llvm_state &s, const binary_operator &bo, const std::vector<llvm::Value *> &arr,
                                std::uint32_t n_uvars, std::uint32_t order, std::uint32_t idx, std::uint32_t batch_size)
{
    return std::visit(
        [&](const auto &v1, const auto &v2) {
            return bo_taylor_diff_div_impl<T>(s, v1, v2, arr, n_uvars, order, idx, batch_size);
        },
        bo.lhs().value(), bo.rhs().value());
}

template <typename T>
llvm::Value *taylor_diff_bo_impl(llvm_state &s, const binary_operator &bo, const std::vector<llvm::Value *> &arr,
                                 std::uint32_t n_uvars, std::uint32_t order, std::uint32_t idx,
                                 std::uint32_t batch_size)
{
    // NOTE: some of the implementations
    // require order to be at least 1 in order
    // to be able to do pairwise summation.
    // NOTE: also not much use in allowing zero-order
    // derivatives, which in general might complicate
    // the implementation.
    if (order == 0u) {
        throw std::invalid_argument(
            "Cannot compute the Taylor derivative of order 0 of a binary operator (the order must be at least one)");
    }

    // lhs and rhs must be u vars or numbers.
    auto check_arg = [](const expression &e) {
        std::visit(
            [](const auto &v) {
                using type = detail::uncvref_t<decltype(v)>;

                if constexpr (std::is_same_v<type, variable>) {
                    // The expression is a variable. Check that it
                    // is a u variable.
                    const auto &var_name = v.name();
                    if (var_name.rfind("u_", 0) != 0) {
                        throw std::invalid_argument(
                            "Invalid variable name '" + var_name
                            + "' encountered in the Taylor diff phase for a binary operator expression (the name "
                              "must be in the form 'u_n', where n is a non-negative integer)");
                    }
                } else if constexpr (!std::is_same_v<type, number>) {
                    // Not a variable and not a number.
                    throw std::invalid_argument(
                        "An invalid expression type was passed to the Taylor diff phase of a binary operator (the "
                        "expression must be either a variable or a number, but it is neither)");
                }
            },
            e.value());
    };

    check_arg(bo.lhs());
    check_arg(bo.rhs());

    switch (bo.op()) {
        case binary_operator::type::add:
            return bo_taylor_diff_add<T>(s, bo, arr, n_uvars, order, idx, batch_size);
        case binary_operator::type::sub:
            return bo_taylor_diff_sub<T>(s, bo, arr, n_uvars, order, idx, batch_size);
        case binary_operator::type::mul:
            return bo_taylor_diff_mul<T>(s, bo, arr, n_uvars, order, idx, batch_size);
        default:
            return bo_taylor_diff_div<T>(s, bo, arr, n_uvars, order, idx, batch_size);
    }
}

} // namespace

} // namespace detail

llvm::Value *taylor_diff_dbl(llvm_state &s, const binary_operator &bo, const std::vector<llvm::Value *> &arr,
                             std::uint32_t n_uvars, std::uint32_t order, std::uint32_t idx, std::uint32_t batch_size)
{
    return detail::taylor_diff_bo_impl<double>(s, bo, arr, n_uvars, order, idx, batch_size);
}

llvm::Value *taylor_diff_ldbl(llvm_state &s, const binary_operator &bo, const std::vector<llvm::Value *> &arr,
                              std::uint32_t n_uvars, std::uint32_t order, std::uint32_t idx, std::uint32_t batch_size)
{
    return detail::taylor_diff_bo_impl<long double>(s, bo, arr, n_uvars, order, idx, batch_size);
}

#if defined(HEYOKA_HAVE_REAL128)

llvm::Value *taylor_diff_f128(llvm_state &s, const binary_operator &bo, const std::vector<llvm::Value *> &arr,
                              std::uint32_t n_uvars, std::uint32_t order, std::uint32_t idx, std::uint32_t batch_size)
{
    return detail::taylor_diff_bo_impl<mppp::real128>(s, bo, arr, n_uvars, order, idx, batch_size);
}

#endif

namespace detail
{

namespace
{

template <typename T>
llvm::Value *taylor_c_u_init_bo_impl(llvm_state &s, const binary_operator &bo, llvm::Value *diff_arr,
                                     std::uint32_t batch_size)
{
    // Do the Taylor init for lhs and rhs.
    auto l = taylor_c_u_init<T>(s, bo.lhs(), diff_arr, batch_size);
    auto r = taylor_c_u_init<T>(s, bo.rhs(), diff_arr, batch_size);

    // Do the codegen for the corresponding operation.
    auto &builder = s.builder();
    switch (bo.op()) {
        case binary_operator::type::add:
            return builder.CreateFAdd(l, r);
        case binary_operator::type::sub:
            return builder.CreateFSub(l, r);
        case binary_operator::type::mul:
            return builder.CreateFMul(l, r);
        default:
            return builder.CreateFDiv(l, r);
    }
}

} // namespace

} // namespace detail

llvm::Value *taylor_c_u_init_dbl(llvm_state &s, const binary_operator &bo, llvm::Value *diff_arr,
                                 std::uint32_t batch_size)
{
    return detail::taylor_c_u_init_bo_impl<double>(s, bo, diff_arr, batch_size);
}

llvm::Value *taylor_c_u_init_ldbl(llvm_state &s, const binary_operator &bo, llvm::Value *diff_arr,
                                  std::uint32_t batch_size)
{
    return detail::taylor_c_u_init_bo_impl<long double>(s, bo, diff_arr, batch_size);
}

#if defined(HEYOKA_HAVE_REAL128)

llvm::Value *taylor_c_u_init_f128(llvm_state &s, const binary_operator &bo, llvm::Value *diff_arr,
                                  std::uint32_t batch_size)
{
    return detail::taylor_c_u_init_bo_impl<mppp::real128>(s, bo, diff_arr, batch_size);
}

#endif

namespace detail
{

namespace
{

// Derivative of number +- number.
template <bool, typename T>
llvm::Value *bo_taylor_c_diff_addsub_impl(llvm_state &s, const number &, const number &, llvm::Value *, std::uint32_t,
                                          llvm::Value *, std::uint32_t, std::uint32_t batch_size)
{
    return vector_splat(s.builder(), codegen<T>(s, number{0.}), batch_size);
}

// Derivative of number +- var.
template <bool AddOrSub, typename T>
llvm::Value *bo_taylor_c_diff_addsub_impl(llvm_state &s, const number &, const variable &var, llvm::Value *diff_arr,
                                          std::uint32_t n_uvars, llvm::Value *order, std::uint32_t, std::uint32_t)
{
    auto &builder = s.builder();

    auto ret = taylor_c_load_diff(s, diff_arr, n_uvars, order, builder.getInt32(uname_to_index(var.name())));

    if constexpr (AddOrSub) {
        return ret;
    } else {
        // Negate if we are doing a subtraction.
        return builder.CreateFNeg(ret);
    }
}

// Derivative of var +- number.
template <bool AddOrSub, typename T>
llvm::Value *bo_taylor_c_diff_addsub_impl(llvm_state &s, const variable &var, const number &, llvm::Value *diff_arr,
                                          std::uint32_t n_uvars, llvm::Value *order, std::uint32_t, std::uint32_t)
{
    return taylor_c_load_diff(s, diff_arr, n_uvars, order, s.builder().getInt32(uname_to_index(var.name())));
}

// Derivative of var +- var.
template <bool AddOrSub, typename T>
llvm::Value *bo_taylor_c_diff_addsub_impl(llvm_state &s, const variable &var0, const variable &var1,
                                          llvm::Value *diff_arr, std::uint32_t n_uvars, llvm::Value *order,
                                          std::uint32_t, std::uint32_t)
{
    auto &builder = s.builder();

    auto v0 = taylor_c_load_diff(s, diff_arr, n_uvars, order, builder.getInt32(uname_to_index(var0.name())));
    auto v1 = taylor_c_load_diff(s, diff_arr, n_uvars, order, builder.getInt32(uname_to_index(var1.name())));

    if constexpr (AddOrSub) {
        return builder.CreateFAdd(v0, v1);
    } else {
        return builder.CreateFSub(v0, v1);
    }
}

// All the other cases.
template <bool, typename, typename V1, typename V2>
llvm::Value *bo_taylor_c_diff_addsub_impl(llvm_state &, const V1 &, const V2 &, llvm::Value *, std::uint32_t,
                                          llvm::Value *, std::uint32_t, std::uint32_t)
{
    assert(false);

    return nullptr;
}

template <typename T>
llvm::Value *bo_taylor_c_diff_add(llvm_state &s, const binary_operator &bo, llvm::Value *diff_arr,
                                  std::uint32_t n_uvars, llvm::Value *order, std::uint32_t idx,
                                  std::uint32_t batch_size)
{
    return std::visit(
        [&](const auto &v1, const auto &v2) {
            return bo_taylor_c_diff_addsub_impl<true, T>(s, v1, v2, diff_arr, n_uvars, order, idx, batch_size);
        },
        bo.lhs().value(), bo.rhs().value());
}

template <typename T>
llvm::Value *bo_taylor_c_diff_sub(llvm_state &s, const binary_operator &bo, llvm::Value *diff_arr,
                                  std::uint32_t n_uvars, llvm::Value *order, std::uint32_t idx,
                                  std::uint32_t batch_size)
{
    return std::visit(
        [&](const auto &v1, const auto &v2) {
            return bo_taylor_c_diff_addsub_impl<false, T>(s, v1, v2, diff_arr, n_uvars, order, idx, batch_size);
        },
        bo.lhs().value(), bo.rhs().value());
}

// Derivative of number * number.
template <typename T>
llvm::Value *bo_taylor_c_diff_mul_impl(llvm_state &s, const number &, const number &, llvm::Value *, std::uint32_t,
                                       llvm::Value *, std::uint32_t, std::uint32_t batch_size)
{
    return vector_splat(s.builder(), codegen<T>(s, number{0.}), batch_size);
}

// Derivative of var * number.
template <typename T>
llvm::Value *bo_taylor_c_diff_mul_impl(llvm_state &s, const variable &var, const number &num, llvm::Value *diff_arr,
                                       std::uint32_t n_uvars, llvm::Value *order, std::uint32_t,
                                       std::uint32_t batch_size)
{
    auto &builder = s.builder();

    auto ret = taylor_c_load_diff(s, diff_arr, n_uvars, order, builder.getInt32(uname_to_index(var.name())));
    auto mul = vector_splat(builder, codegen<T>(s, num), batch_size);

    return builder.CreateFMul(ret, mul);
}

// Derivative of number * var.
template <typename T>
llvm::Value *bo_taylor_c_diff_mul_impl(llvm_state &s, const number &num, const variable &var, llvm::Value *diff_arr,
                                       std::uint32_t n_uvars, llvm::Value *order, std::uint32_t idx,
                                       std::uint32_t batch_size)
{
    return bo_taylor_c_diff_mul_impl<T>(s, var, num, diff_arr, n_uvars, order, idx, batch_size);
}

// Derivative of var * var.
template <typename T>
llvm::Value *bo_taylor_c_diff_mul_impl(llvm_state &s, const variable &var0, const variable &var1, llvm::Value *diff_arr,
                                       std::uint32_t n_uvars, llvm::Value *order, std::uint32_t,
                                       std::uint32_t batch_size)
{
    auto &module = s.module();
    auto &builder = s.builder();
    auto &context = s.context();

    // Fetch the pointee type of diff_arr.
    auto val_t = pointee_type(diff_arr);

    // Get the function name for the current fp type, batch size and n_uvars.
    // NOTE: need the mangling on n_uvars too because it affects the indexing
    // into diff_arr.
    const auto fname = "heyoka_taylor_diff_mul_" + taylor_mangle_suffix(val_t) + "_n_uvars_" + li_to_string(n_uvars);

    // The function arguments:
    // - indices of the variables,
    // - derivative order,
    // - diff array.
    std::vector<llvm::Type *> fargs{llvm::Type::getInt32Ty(context), llvm::Type::getInt32Ty(context),
                                    llvm::Type::getInt32Ty(context), diff_arr->getType()};

    // Try to see if we already created the function.
    auto f = module.getFunction(fname);

    if (f == nullptr) {
        // The function was not created before, do it now.

        // Fetch the current insertion block.
        auto orig_bb = builder.GetInsertBlock();

        // The return type is the pointee type of diff_arr.
        auto *ft = llvm::FunctionType::get(val_t, fargs, false);
        // Create the function
        f = llvm::Function::Create(ft, llvm::Function::InternalLinkage, fname, &module);
        assert(f != nullptr);
        f->addFnAttr(llvm::Attribute::NoInline);

        // Fetch the function arguments.
        auto idx0 = f->args().begin();
        auto idx1 = f->args().begin() + 1;
        auto ord = f->args().begin() + 2;
        auto diff_ptr = f->args().begin() + 3;

        // Create a new basic block to start insertion into.
        auto bb = llvm::BasicBlock::Create(context, "entry", f);
        builder.SetInsertPoint(bb);

        // Create the accumulator.
        auto acc = builder.CreateAlloca(val_t);
        builder.CreateStore(vector_splat(builder, codegen<T>(s, number{0.}), batch_size), acc);

        // Run the loop.
        llvm_loop_u32(s, builder.getInt32(0), builder.CreateAdd(ord, builder.getInt32(1)), [&](llvm::Value *j) {
            auto b_nj = taylor_c_load_diff(s, diff_ptr, n_uvars, builder.CreateSub(ord, j), idx0);
            auto cj = taylor_c_load_diff(s, diff_ptr, n_uvars, j, idx1);
            builder.CreateStore(builder.CreateFAdd(builder.CreateLoad(acc), builder.CreateFMul(b_nj, cj)), acc);
        });

        // Create the return value.
        builder.CreateRet(builder.CreateLoad(acc));

        // Verify.
        s.verify_function(f);

        // Restore the original insertion block.
        builder.SetInsertPoint(orig_bb);
    } else {
        // The function was created before. Check if the signatures match.
        // NOTE: there could be a mismatch if the derivative function was created
        // and then optimised - optimisation might remove arguments which are compile-time
        // constants.
        if (!compare_function_signature(f, val_t, fargs)) {
            throw std::invalid_argument(
                "Inconsistent function signature for the Taylor derivative of multiplication in compact mode detected");
        }
    }

    // Invoke the function and return the result.
    return builder.CreateCall(f, {builder.getInt32(uname_to_index(var0.name())),
                                  builder.getInt32(uname_to_index(var1.name())), order, diff_arr});
}

// All the other cases.
template <typename, typename V1, typename V2>
llvm::Value *bo_taylor_c_diff_mul_impl(llvm_state &, const V1 &, const V2 &, llvm::Value *, std::uint32_t,
                                       llvm::Value *, std::uint32_t, std::uint32_t)
{
    assert(false);

    return nullptr;
}

template <typename T>
llvm::Value *bo_taylor_c_diff_mul(llvm_state &s, const binary_operator &bo, llvm::Value *diff_arr,
                                  std::uint32_t n_uvars, llvm::Value *order, std::uint32_t idx,
                                  std::uint32_t batch_size)
{
    return std::visit(
        [&](const auto &v1, const auto &v2) {
            return bo_taylor_c_diff_mul_impl<T>(s, v1, v2, diff_arr, n_uvars, order, idx, batch_size);
        },
        bo.lhs().value(), bo.rhs().value());
}

// Derivative of number / number.
template <typename T>
llvm::Value *bo_taylor_c_diff_div_impl(llvm_state &s, const number &, const number &, llvm::Value *, std::uint32_t,
                                       llvm::Value *, std::uint32_t, std::uint32_t batch_size)
{
    return vector_splat(s.builder(), codegen<T>(s, number{0.}), batch_size);
}

// Derivative of variable / variable or number / variable. These two cases
// are quite similar, so we handle them together.
template <typename T, typename U,
          std::enable_if_t<std::disjunction_v<std::is_same<U, number>, std::is_same<U, variable>>, int> = 0>
llvm::Value *bo_taylor_c_diff_div_impl(llvm_state &s, const U &nv, const variable &var1, llvm::Value *diff_arr,
                                       std::uint32_t n_uvars, llvm::Value *order, std::uint32_t idx,
                                       std::uint32_t batch_size)
{
    auto &module = s.module();
    auto &builder = s.builder();
    auto &context = s.context();

    // Fetch the pointee type of diff_arr.
    auto val_t = pointee_type(diff_arr);

    // Get the function name for the current fp type, batch size and n_uvars.
    const auto fname = "heyoka_taylor_diff_div_" + taylor_mangle_suffix(val_t) + "_n_uvars_" + li_to_string(n_uvars);

    // The function arguments:
    // - indices of the variables,
    // - derivative order,
    // - diff array.
    std::vector<llvm::Type *> fargs{llvm::Type::getInt32Ty(context), llvm::Type::getInt32Ty(context),
                                    llvm::Type::getInt32Ty(context), diff_arr->getType()};

    // Try to see if we already created the function.
    auto f = module.getFunction(fname);

    if (f == nullptr) {
        // The function was not created before, do it now.

        // Fetch the current insertion block.
        auto orig_bb = builder.GetInsertBlock();

        // The return type is the pointee type of diff_arr.
        auto *ft = llvm::FunctionType::get(val_t, fargs, false);
        // Create the function
        f = llvm::Function::Create(ft, llvm::Function::InternalLinkage, fname, &module);
        assert(f != nullptr);
        f->addFnAttr(llvm::Attribute::NoInline);

        // Fetch the function arguments.
        auto idx0 = f->args().begin();
        auto idx1 = f->args().begin() + 1;
        auto ord = f->args().begin() + 2;
        auto diff_ptr = f->args().begin() + 3;

        // Create a new basic block to start insertion into.
        auto bb = llvm::BasicBlock::Create(context, "entry", f);
        builder.SetInsertPoint(bb);

        // Create the accumulator.
        auto acc = builder.CreateAlloca(val_t);
        builder.CreateStore(vector_splat(builder, codegen<T>(s, number{0.}), batch_size), acc);

        // Run the loop.
        llvm_loop_u32(s, builder.getInt32(1), builder.CreateAdd(ord, builder.getInt32(1)), [&](llvm::Value *j) {
            auto cj = taylor_c_load_diff(s, diff_ptr, n_uvars, j, idx0);
            auto a_nj = taylor_c_load_diff(s, diff_ptr, n_uvars, builder.CreateSub(ord, j), idx1);
            builder.CreateStore(builder.CreateFAdd(builder.CreateLoad(acc), builder.CreateFMul(cj, a_nj)), acc);
        });

        // Create the return value.
        builder.CreateRet(builder.CreateLoad(acc));

        // Verify.
        s.verify_function(f);

        // Restore the original insertion block.
        builder.SetInsertPoint(orig_bb);
    } else {
        // The function was created before. Check if the signatures match.
        // NOTE: there could be a mismatch if the derivative function was created
        // and then optimised - optimisation might remove arguments which are compile-time
        // constants.
        if (!compare_function_signature(f, val_t, fargs)) {
            throw std::invalid_argument(
                "Inconsistent function signature for the Taylor derivative of division in compact mode detected");
        }
    }

    // Invoke the function to compute the result of the sum.
    llvm::Value *ret = builder.CreateCall(
        f, {builder.getInt32(uname_to_index(var1.name())), builder.getInt32(idx), order, diff_arr});

    if constexpr (std::is_same_v<U, number>) {
        // nv is a number. Negate the accumulator.
        ret = builder.CreateFNeg(ret);
    } else {
        // nv is a variable. Subtract ret from the
        // derivative of order 'order' of nv.
        ret = builder.CreateFSub(
            taylor_c_load_diff(s, diff_arr, n_uvars, order, builder.getInt32(uname_to_index(nv.name()))), ret);
    }

    // Compute and return the result.
    return builder.CreateFDiv(ret, taylor_c_load_diff(s, diff_arr, n_uvars, builder.getInt32(0),
                                                      builder.getInt32(uname_to_index(var1.name()))));
}

// All the other cases.
template <typename, typename V1, typename V2>
llvm::Value *bo_taylor_c_diff_div_impl(llvm_state &, const V1 &, const V2 &, llvm::Value *, std::uint32_t,
                                       llvm::Value *, std::uint32_t, std::uint32_t)
{
    assert(false);

    return nullptr;
}

template <typename T>
llvm::Value *bo_taylor_c_diff_div(llvm_state &s, const binary_operator &bo, llvm::Value *diff_arr,
                                  std::uint32_t n_uvars, llvm::Value *order, std::uint32_t idx,
                                  std::uint32_t batch_size)
{
    return std::visit(
        [&](const auto &v1, const auto &v2) {
            return bo_taylor_c_diff_div_impl<T>(s, v1, v2, diff_arr, n_uvars, order, idx, batch_size);
        },
        bo.lhs().value(), bo.rhs().value());
}

template <typename T>
llvm::Value *taylor_c_diff_bo_impl(llvm_state &s, const binary_operator &bo, llvm::Value *arr, std::uint32_t n_uvars,
                                   llvm::Value *order, std::uint32_t idx, std::uint32_t batch_size)
{
    // lhs and rhs must be u vars or numbers.
    auto check_arg = [](const expression &e) {
        std::visit(
            [](const auto &v) {
                using type = detail::uncvref_t<decltype(v)>;

                if constexpr (std::is_same_v<type, variable>) {
                    // The expression is a variable. Check that it
                    // is a u variable.
                    const auto &var_name = v.name();
                    if (var_name.rfind("u_", 0) != 0) {
                        throw std::invalid_argument("Invalid variable name '" + var_name
                                                    + "' encountered in the Taylor diff phase for a binary operator "
                                                      "expression in compact mode (the name "
                                                      "must be in the form 'u_n', where n is a non-negative integer)");
                    }
                } else if constexpr (!std::is_same_v<type, number>) {
                    // Not a variable and not a number.
                    throw std::invalid_argument("An invalid expression type was passed to the Taylor diff phase of a "
                                                "binary operator in compact mode (the "
                                                "expression must be either a variable or a number, but it is neither)");
                }
            },
            e.value());
    };

    check_arg(bo.lhs());
    check_arg(bo.rhs());

    switch (bo.op()) {
        case binary_operator::type::add:
            return bo_taylor_c_diff_add<T>(s, bo, arr, n_uvars, order, idx, batch_size);
        case binary_operator::type::sub:
            return bo_taylor_c_diff_sub<T>(s, bo, arr, n_uvars, order, idx, batch_size);
        case binary_operator::type::mul:
            return bo_taylor_c_diff_mul<T>(s, bo, arr, n_uvars, order, idx, batch_size);
        default:
            return bo_taylor_c_diff_div<T>(s, bo, arr, n_uvars, order, idx, batch_size);
    }
}

} // namespace

} // namespace detail

llvm::Value *taylor_c_diff_dbl(llvm_state &s, const binary_operator &bo, llvm::Value *arr, std::uint32_t n_uvars,
                               llvm::Value *order, std::uint32_t idx, std::uint32_t batch_size)
{
    return detail::taylor_c_diff_bo_impl<double>(s, bo, arr, n_uvars, order, idx, batch_size);
}

llvm::Value *taylor_c_diff_ldbl(llvm_state &s, const binary_operator &bo, llvm::Value *arr, std::uint32_t n_uvars,
                                llvm::Value *order, std::uint32_t idx, std::uint32_t batch_size)
{
    return detail::taylor_c_diff_bo_impl<long double>(s, bo, arr, n_uvars, order, idx, batch_size);
}

#if defined(HEYOKA_HAVE_REAL128)

llvm::Value *taylor_c_diff_f128(llvm_state &s, const binary_operator &bo, llvm::Value *arr, std::uint32_t n_uvars,
                                llvm::Value *order, std::uint32_t idx, std::uint32_t batch_size)
{
    return detail::taylor_c_diff_bo_impl<mppp::real128>(s, bo, arr, n_uvars, order, idx, batch_size);
}

#endif

} // namespace heyoka
