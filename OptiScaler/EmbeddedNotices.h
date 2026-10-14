#pragma once

#include <string_view>

namespace EmbeddedNotices
{
// UTF-8 text backed by this DLL's read-only resource; valid for its lifetime.
std::string_view Text();
} // namespace EmbeddedNotices
