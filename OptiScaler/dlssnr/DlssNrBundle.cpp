#include "pch.h"

#include "DlssNrBundle.h"

#include <Util.h>
#include <array>
#include <bcrypt.h>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <string_view>

#pragma comment(lib, "bcrypt.lib")

namespace DlssNrBundle
{
namespace
{
struct Payload
{
    WORD ResourceId;
    const wchar_t* FileName;
    DWORD Bytes;
    std::string_view Sha256;
};

constexpr Payload kForwarder { 31101, L"nvngx.dll_dlssnr.dll", 152064,
                               "89766bcb38a9caea9a2e3f767923e6cda8b9b51a6a66ca14ac9473d734e12efa" };
constexpr Payload kModel { 31102, L"nvngx_dlssnr.dll", 165840496,
                           "e67dee209320cdafe0e93e45675d7aa34323a53acc57a72b2e40a181581c989a" };
constexpr wchar_t kBundleId[] = L"bc5a4c3f9f92213b7440cfd7153949811221aed162baa16441eca9db51846124";

std::mutex s_mutex;
bool s_attempted = false;
bool s_embedded = false;
bool s_ready = false;
std::filesystem::path s_directory;

class Sha256
{
    BCRYPT_HASH_HANDLE _hash = nullptr;

  public:
    Sha256()
    {
        if (BCryptCreateHash(BCRYPT_SHA256_ALG_HANDLE, &_hash, nullptr, 0, nullptr, 0, 0) != 0)
            throw std::runtime_error("Cannot initialise NR input verification");
    }
    ~Sha256() { BCryptDestroyHash(_hash); }
    Sha256(const Sha256&) = delete;
    Sha256& operator=(const Sha256&) = delete;

    bool Update(const void* data, DWORD bytes) { return BCryptHashData(_hash, (PUCHAR) data, bytes, 0) == 0; }

    bool Matches(std::string_view expected)
    {
        std::array<unsigned char, 32> digest {};
        if (BCryptFinishHash(_hash, digest.data(), static_cast<ULONG>(digest.size()), 0) != 0 || expected.size() != 64)
            return false;
        constexpr char hex[] = "0123456789abcdef";
        for (size_t i = 0; i < digest.size(); ++i)
            if (expected[i * 2] != hex[digest[i] >> 4] || expected[i * 2 + 1] != hex[digest[i] & 15])
                return false;
        return true;
    }
};

bool VerifiedFile(const std::filesystem::path& path, const Payload& payload)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file || file.tellg() != static_cast<std::streamoff>(payload.Bytes))
        return false;
    file.seekg(0);
    Sha256 hash;
    std::array<char, 65536> buffer {};
    while (file)
    {
        file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto bytes = file.gcount();
        if (bytes > 0 && !hash.Update(buffer.data(), static_cast<DWORD>(bytes)))
            return false;
    }
    return file.eof() && hash.Matches(payload.Sha256);
}

void Install(const Payload& payload, HRSRC resource)
{
    if (resource == nullptr || SizeofResource(dllModule, resource) != payload.Bytes)
        throw std::runtime_error("The embedded NR pair is incomplete or has an unexpected size");
    const auto loaded = LoadResource(dllModule, resource);
    const void* data = loaded != nullptr ? LockResource(loaded) : nullptr;
    Sha256 hash;
    if (data == nullptr || !hash.Update(data, payload.Bytes) || !hash.Matches(payload.Sha256))
        throw std::runtime_error("The embedded NR pair does not match the supplied DLLs");

    const auto destination = s_directory / payload.FileName;
    if (VerifiedFile(destination, payload))
        return;

    const std::filesystem::path temporary = destination.wstring() + L"." + std::to_wstring(GetCurrentProcessId()) +
                                            L"." + std::to_wstring(GetCurrentThreadId()) + L".tmp";
    try
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        file.write(static_cast<const char*>(data), payload.Bytes);
        file.close();
        if (!file)
            throw std::runtime_error("Cannot extract the embedded NR runtime");
        if (!MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        {
            // Another game process can finish installing the same immutable pair first.
            if (!VerifiedFile(destination, payload))
                throw std::runtime_error("Cannot install the embedded NR runtime");
        }
        else if (!VerifiedFile(destination, payload))
            throw std::runtime_error("The extracted NR runtime failed verification");
        DeleteFileW(temporary.c_str());
    }
    catch (...)
    {
        DeleteFileW(temporary.c_str());
        throw;
    }
}

std::optional<std::filesystem::path> Resolve(const Payload& payload)
{
    if (!Ensure())
        return std::nullopt;
    if (s_embedded)
        return s_directory / payload.FileName;

    auto path = Util::FindFilePath(Util::DllPath().remove_filename(), payload.FileName);
    if (!path.has_value())
        path = Util::FindFilePath(Util::ExePath().remove_filename(), payload.FileName);
    return path;
}
} // namespace

bool Ensure()
{
    std::lock_guard lock(s_mutex);
    if (s_attempted)
        return s_ready;
    s_attempted = true;

    const auto forwarder = FindResourceW(dllModule, MAKEINTRESOURCEW(kForwarder.ResourceId), MAKEINTRESOURCEW(10));
    const auto model = FindResourceW(dllModule, MAKEINTRESOURCEW(kModel.ResourceId), MAKEINTRESOURCEW(10));
    s_embedded = forwarder != nullptr || model != nullptr;
    if (!s_embedded)
    {
        s_ready = true;
        return true;
    }

    try
    {
        s_directory = Util::DllPath().remove_filename() / L"OptiScaler" / L"dlssnr" / kBundleId;
        std::filesystem::create_directories(s_directory);
        Install(kForwarder, forwarder);
        Install(kModel, model);
        s_ready = true;
        LOG_INFO("DLSS-NR: supplied 310.8 runtime pair verified in {}", s_directory.string());
    }
    catch (const std::exception& error)
    {
        LOG_ERROR("DLSS-NR: embedded runtime unavailable: {}", error.what());
    }
    return s_ready;
}

std::optional<std::filesystem::path> GetForwarder() { return Resolve(kForwarder); }
std::optional<std::filesystem::path> GetModel() { return Resolve(kModel); }
} // namespace DlssNrBundle
