/*
 * Copyright (c) 2021, Krisna Pranav
 *
 * SPDX-License-Identifier: BSD-2-Clause
*/


#pragma once

namespace AK::Detail {

template<bool B, class T = void>
struct EnableIf {
};

template<class T>
struct EnableIf<true, T> {
    using Type = T;
};

template<class T, T v>
struct IntegralConstant {
    static constexpr T value = v;
    using ValueType = T;
    using Type = IntegralConstant;
    constexpr operator ValueType() const { return value; }
    constexpr ValueType operator()() const { return value; }
};

using FalseType = IntegralConstant<bool, false>;
using TrueType = IntegralConstant<bool, true>;

template<class T>
using AddConst = const T;

template<class T>
struct __RemoveConst {
    using Type = T;
};

template<class T>
struct __RemoveConst<const T> {
    using Type = T;
};
template<class T>
using RemoveConst = typename __RemoveConst<T>::Type;

template<class T>
struct __RemoveVolatile {
    using Type = T;
};

template<class T>
struct __RemoveVolatile<volatile T> {
    using Type = T;
};

template<typename T>
using RemoveVolatile = typename __RemoveVolatile<T>::Type;

template<class T>
using RemoveCV = RemoveVolatile<RemoveConst<T>>;

template<typename...>
using VoidType = void;

template<class T>
inline constexpr bool IsLvalueReference = false;

template<class T>
inline constexpr bool IsLvalueReference<T&> = true;

template<class T>
inline constexpr bool __IsPointerHelper = false;

template<class T>
inline constexpr bool __IsPointerHelper<T*> = true;

template<class T>
inline constexpr bool IsPointer = __IsPointerHelper<RemoveCV<T>>;

template<class>
inline constexpr bool IsFunction = false;
template<class Ret, class... Args>
inline constexpr bool IsFunction<Ret(Args...)> = true;
template<class Ret, class... Args>
inline constexpr bool IsFunction<Ret(Args..., ...)> = true;
template<class Ret, class... Args>
inline constexpr bool IsFunction<Ret(Args...) const> = true;
template<class Ret, class... Args>
inline constexpr bool IsFunction<Ret(Args..., ...) const> = true;
template<class Ret, class... Args>
inline constexpr bool IsFunction<Ret(Args...) volatile> = true;
template<class Ret, class... Args>
inline constexpr bool IsFunction<Ret(Args..., ...) volatile> = true;
template<class Ret, class... Args>
inline constexpr bool IsFunction<Ret(Args...) const volatile> = true;
template<class Ret, class... Args>
inline constexpr bool IsFunction<Ret(Args..., ...) const volatile> = true;
template<class Ret, class... Args>
inline constexpr bool IsFunction<Ret(Args...)&> = true;
template<class Ret, class... Args>
inline constexpr bool IsFunction<Ret(Args..., ...)&> = true;
template<class Ret, class... Args>
inline constexpr bool IsFunction<Ret(Args...) const&> = true;
template<class Ret, class... Args>
inline constexpr bool IsFunction<Ret(Args..., ...) const&> = true;
template<class Ret, class... Args>
inline constexpr bool IsFunction<Ret(Args...) volatile&> = true;
template<class Ret, class... Args>
inline constexpr bool IsFunction<Ret(Args..., ...) volatile&> = true;
template<class Ret, class... Args>
inline constexpr bool IsFunction<Ret(Args...) const volatile&> = true;
template<class Ret, class... Args>
inline constexpr bool IsFunction<Ret(Args..., ...) const volatile&> = true;
template<class Ret, class... Args>
inline constexpr bool IsFunction<Ret(Args...) &&> = true;
template<class Ret, class... Args>
inline constexpr bool IsFunction<Ret(Args..., ...) &&> = true;
template<class Ret, class... Args>
inline constexpr bool IsFunction<Ret(Args...) const&&> = true;
template<class Ret, class... Args>
inline constexpr bool IsFunction<Ret(Args..., ...) const&&> = true;
template<class Ret, class... Args>
inline constexpr bool IsFunction<Ret(Args...) volatile&&> = true;
template<class Ret, class... Args>
inline constexpr bool IsFunction<Ret(Args..., ...) volatile&&> = true;
template<class Ret, class... Args>
inline constexpr bool IsFunction<Ret(Args...) const volatile&&> = true;
template<class Ret, class... Args>
inline constexpr bool IsFunction<Ret(Args..., ...) const volatile&&> = true;

template<class T>
inline constexpr bool IsRvalueReference = false;
template<class T>
inline constexpr bool IsRvalueReference<T&&> = true;

template<class T>
struct __RemovePointer {
    using Type = T;
};
template<class T>
struct __RemovePointer<T*> {
    using Type = T;
};
template<class T>
struct __RemovePointer<T* const> {
    using Type = T;
};
template<class T>
struct __RemovePointer<T* volatile> {
    using Type = T;
};
template<class T>
struct __RemovePointer<T* const volatile> {
    using Type = T;
};
template<typename T>
using RemovePointer = typename __RemovePointer<T>::Type;

template<typename T, typename U>
inline constexpr bool IsSame = false;

template<typename T>
inline constexpr bool IsSame<T, T> = true;

template<bool condition, class TrueType, class FalseType>
struct __Conditional {
    using Type = TrueType;
};

template<class TrueType, class FalseType>
struct __Conditional<false, TrueType, FalseType> {
    using Type = FalseType;
};

template<bool condition, class TrueType, class FalseType>
using Conditional = typename __Conditional<condition, TrueType, FalseType>::Type;

template<typename T>
inline constexpr bool IsNullPointer = IsSame<decltype(nullptr), RemoveCV<T>>;

template<typename T>
struct __RemoveReference {
    using Type = T;
};

template<class T>
struct __RemoveReference<T&> {
    using Type = T;
};
template<class T>
struct __RemoveReference<T&&> {
    using Type = T;
};

template<typename T>
using RemoveReference = typename __RemoveReference<T>::Type;

template<typename T>
using RemoveCVReference = RemoveCV<RemoveReference<T>>;

template<typename T>
struct __MakeUnsigned {
    using Type = void;
};
template<>
struct __MakeUnsigned<signed char> {
    using Type = unsigned char;
};
template<>
struct __MakeUnsigned<short> {
    using Type = unsigned short;
};
template<>
struct __MakeUnsigned<int> {
    using Type = unsigned int;
};
template<>
struct __MakeUnsigned<long> {
    using Type = unsigned long;
};
template<>
struct __MakeUnsigned<long long> {
    using Type = unsigned long long;
};
template<>
struct __MakeUnsigned<unsigned char> {
    using Type = unsigned char;
};
template<>
struct __MakeUnsigned<unsigned short> {
    using Type = unsigned short;
};
template<>
struct __MakeUnsigned<unsigned int> {
    using Type = unsigned int;
};
template<>
struct __MakeUnsigned<unsigned long> {
    using Type = unsigned long;
};
template<>
struct __MakeUnsigned<unsigned long long> {
    using Type = unsigned long long;
};
template<>
struct __MakeUnsigned<char> {
    using Type = unsigned char;
};
template<>
struct __MakeUnsigned<char8_t> {
    using Type = char8_t;
};
template<>
struct __MakeUnsigned<char16_t> {
    using Type = char16_t;
};

}