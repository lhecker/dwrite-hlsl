// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include <cstdint>

template<typename T>
struct vec2
{
    T x{};
    T y{};

    operator T*() noexcept
    {
        return &x;
    }

    operator T*() const noexcept
    {
        return &x;
    }
};

template<typename T>
struct vec4
{
    union
    {
        T x{};
        T r;
    };
    union
    {
        T y{};
        T g;
    };
    union
    {
        T z{};
        T b;
    };
    union
    {
        T w{};
        T a;
    };

    operator T*() noexcept
    {
        return &x;
    }

    operator T*() const noexcept
    {
        return &x;
    }
};

using u32 = uint32_t;
using u32x2 = vec2<u32>;
using u32x4 = vec4<u32>;

using f32 = float;
using f32x2 = vec2<f32>;
using f32x4 = vec4<f32>;
