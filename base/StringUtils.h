/*
 * Copyright (c) 2021, Krisna Pranav
 * Copyright (c) 2021, Fei Wu <f.eiwu@yahoo.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <base/Forward.h>

namespace Base {

enum class CaseSensitivity {
    CaseInsensitive,
    CaseSensitive,
};

enum class TrimMode {
    Left,
    Right,
    Both
};

enum class TrimWhitespace {
    Yes,
    No,
};

struct MaskSpan {
    size_t start;
    size_t length;

    bool operator==(const MaskSpan& other) const
    {
        return start == other.start && length == other.length;
    }
    bool operator!=(const MaskSpan& other) const
    {
        return !(*this == other);
    }
};

namespace StringUtils {

bool matches(const StringView& str, const StringView& mask, CaseSensitivity = CaseSensitivity::CaseInsensitive, Vector<MaskSpan>* match_spans = nullptr);
template<typename T = int>
Optional<T> convert_to_int(const StringView&, TrimWhitespace = TrimWhitespace::Yes);
template<typename T = unsigned>
Optional<T> convert_to_uint(const StringView&, TrimWhitespace = TrimWhitespace::Yes);
template<typename T = unsigned>
Optional<T> convert_to_uint_from_hex(const StringView&, TrimWhitespace = TrimWhitespace::Yes);
bool equals_ignoring_case(const StringView&, const StringView&);
bool ends_with(const StringView& a, const StringView& b, CaseSensitivity);
bool starts_with(const StringView&, const StringView&, CaseSensitivity);
bool contains(const StringView&, const StringView&, CaseSensitivity);
bool is_whitespace(const StringView&);
StringView trim(const StringView& string, const StringView& characters, TrimMode mode);
StringView trim_whitespace(const StringView& string, TrimMode mode);

Optional<size_t> find(StringView const& haystack, char needle, size_t start = 0);
Optional<size_t> find(StringView const& haystack, StringView const& needle, size_t start = 0);
Optional<size_t> find_last(StringView const& haystack, char needle);
Vector<size_t> find_all(StringView const& haystack, StringView const& needle);
enum class SearchDirection {
    Forward,
    Backward
};
Optional<size_t> find_any_of(StringView const& haystack, StringView const& needles, SearchDirection);

String to_snakecase(const StringView&);

}

}

using Base::CaseSensitivity;
using Base::TrimMode;
using Base::TrimWhitespace;
