/*
 * Copyright (C) 2012-2022 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE. 
 */

#pragma once

#include <format>
#include <iostream>

template <size_t Extent>
struct std::formatter<std::span<const LChar, Extent>> : std::formatter<std::string_view> {
    auto format(const std::span<const LChar, Extent>& span, std::format_context& ctx) const
    {
        auto charSpan = spanReinterpretCast<const char>(span);
        return std::formatter<string_view>::format(std::string_view(charSpan.begin(), charSpan.end()), ctx);
    }
};

namespace WTF {

inline constexpr void checkForCharPointers()
{
}

template<typename Arg>
inline constexpr void checkForCharPointers(Arg&&)
{
    static_assert(!std::is_pointer_v<Arg> || !std::is_same_v<std::remove_cv_t<std::remove_pointer_t<Arg>>, char>, "char* is not bounds safe");
}

template<typename Arg, typename... Args>
inline constexpr void checkForCharPointers(Arg&& arg, Args&&...args)
{
    checkForCharPointers(arg);
    checkForCharPointers(std::forward<Args>(args)...);
}

template<typename... Args>
void print(const std::format_string<Args...> message, Args&&...args)
{
    checkForCharPointers(args...);
    std::cout << std::format(message, std::forward<Args>(args)...);
}

} // namespace WTF

using WTF::print;
