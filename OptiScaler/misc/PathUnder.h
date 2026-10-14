#pragma once

// Whether one Windows path lies inside another, by spelling (Util::IsSubpath). No file system access: a host test
// drives it (tests/path_under_smoke.cpp).

#include <windows.h>

#include <filesystem>

namespace pathrule
{
// Path components as Windows compares them: ordinal, without case
inline bool SameComponent(const std::filesystem::path& a, const std::filesystem::path& b)
{
    const auto& x = a.native();
    const auto& y = b.native();
    return CompareStringOrdinal(x.c_str(), (int) x.size(), y.c_str(), (int) y.size(), TRUE) == CSTR_EQUAL;
}

// `path` is below `base` (not `base` itself): every component of `base` starts `path`, any case or separator, `.`
// and `..` resolved by spelling. A trailing separator is no component.
inline bool IsUnder(const std::filesystem::path& path, const std::filesystem::path& base)
{
    const auto p = path.lexically_normal();
    const auto b = base.lexically_normal();

    if (b.empty())
        return false;

    auto it = p.begin();

    for (const auto& part : b)
    {
        if (part.empty())
            continue;

        if (it == p.end() || !SameComponent(*it, part))
            return false;

        ++it;
    }

    for (; it != p.end(); ++it)
    {
        if (!it->empty())
            return true;
    }

    return false;
}
} // namespace pathrule
