#include "pch.h"

#include "EmbeddedNotices.h"

namespace EmbeddedNotices
{
std::string_view Text()
{
    static const auto text = []() -> std::string_view
    {
        const auto resource = FindResourceW(dllModule, MAKEINTRESOURCEW(31110), MAKEINTRESOURCEW(10));
        if (resource == nullptr)
            return "Embedded license text is unavailable in this build.";

        const auto size = SizeofResource(dllModule, resource);
        const auto loaded = LoadResource(dllModule, resource);
        const auto data = loaded != nullptr ? static_cast<const char*>(LockResource(loaded)) : nullptr;
        if (data == nullptr || size == 0)
            return "Embedded license text is unavailable in this build.";
        return { data, size };
    }();
    return text;
}
} // namespace EmbeddedNotices
