// Copyright 2020 Francesco Biscani (bluescarni@gmail.com), Dario Izzo (dario.izzo@gmail.com)
//
// This file is part of the heyoka library.
//
// This Source Code Form is subject to the terms of the Mozilla
// Public License v. 2.0. If a copy of the MPL was not distributed
// with this file, You can obtain one at http://mozilla.org/MPL/2.0/.

#ifndef HEYOKA_DETAIL_ASSERT_NONNULL_RET_HPP
#define HEYOKA_DETAIL_ASSERT_NONNULL_RET_HPP

#include <cassert>

#define heyoka_assert_nonnull_ret(expr)                                                                                \
    do {                                                                                                               \
        auto ret = expr;                                                                                               \
        assert(ret != nullptr);                                                                                        \
        return ret;                                                                                                    \
    } while (false)

#endif