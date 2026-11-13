#include "app/Shipping.h"

#include "core/vfs/Vfs.h"
#include "third_party/json/json.hpp"

#include <Windows.h>
#include <ShlObj.h>
#include <shellapi.h>

#include <cctype>
#include <cstring>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

namespace shipping
{
namespace
{
    constexpr const char* kAllowed[] = {
        "--window", "--level", "--log-level", "--user-dir",
        "--shot", "--shot-delay", "--cam-pos", "--cam-rot", "--wind-freeze", "--time", "--play", "--crash-selftest",
    };
}

std::string FilterCommandLine(const wchar_t* fullCommandLine, std::vector<std::string>& dropped)
{
    std::string kept;
    if (!fullCommandLine) { return kept; }
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(fullCommandLine, &argc);
    if (!argv) { return kept; }
    for (int i = 1; i < argc; ++i) // argv[0] is the exe
    {
        const std::string arg = vfs::Narrow(argv[i]);
        const std::size_t eq = arg.find('=');
        const std::string name = arg.substr(0, eq);
        bool allowed = false;
        for (const char* a : kAllowed) { allowed = allowed || name == a; }
        if (!allowed)
        {
            dropped.push_back(arg);
            continue;
        }
        if (!kept.empty()) { kept.push_back(' '); }
        if (eq != std::string::npos && arg.find(' ') != std::string::npos)
        {
            kept += name + "=\"" + arg.substr(eq + 1) + "\"";
        }
        else
        {
            kept += arg;
        }
    }
    LocalFree(argv);
    return kept;
}

std::filesystem::path ExeFolder()
{
    std::wstring path(32768, L'\0');
    const DWORD n = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    path.resize(n);
    return std::filesystem::path(path).parent_path();
}

std::filesystem::path DefaultUserFolder(const wchar_t* product)
{
    PWSTR local = nullptr;
    std::filesystem::path folder;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &local)) && local)
    {
        folder = std::filesystem::path(local) / product;
    }
    CoTaskMemFree(local);
    return folder;
}

bool PrepareUserFolder(const std::filesystem::path& folder, std::wstring& error)
{
    if (folder.empty())
    {
        error = L"no folder for the user's files (%LOCALAPPDATA% not found)";
        return false;
    }
    std::error_code ec;
    for (const wchar_t* part : { L"", L"settings", L"cache", L"logs", L"crashes", L"bot_memory" })
    {
        std::filesystem::create_directories(folder / part, ec); // vfs-ok: the user's writable folder
        if (ec)
        {
            error = L"cannot create " + (folder / part).wstring() + L": " + vfs::Widen(ec.message());
            return false;
        }
    }
    if (!SetCurrentDirectoryW(folder.c_str()))
    {
        error = L"cannot enter " + folder.wstring();
        return false;
    }
    return true;
}

void ShowMessageTimed(const std::wstring& title, const std::wstring& text, bool error, unsigned timeoutMs)
{
    if (timeoutMs == 0)
    {
        wchar_t value[32] = {};
        if (GetEnvironmentVariableW(L"TEST_CUBE_MESSAGE_TIMEOUT_MS", value, 32) > 0) { timeoutMs = static_cast<unsigned>(_wtoi(value)); }
    }
    const UINT style = MB_OK | MB_SETFOREGROUND | MB_TOPMOST | (error ? MB_ICONERROR : MB_ICONINFORMATION);
    using TimeoutBox = int(WINAPI*)(HWND, LPCWSTR, LPCWSTR, UINT, WORD, DWORD);
    const HMODULE user32 = GetModuleHandleW(L"user32.dll");
    const auto timeoutBox = user32 ? reinterpret_cast<TimeoutBox>(GetProcAddress(user32, "MessageBoxTimeoutW")) : nullptr;
    if (timeoutMs != 0 && timeoutBox)
    {
        timeoutBox(nullptr, text.c_str(), title.c_str(), style, 0, timeoutMs);
        return;
    }
    MessageBoxW(nullptr, text.c_str(), title.c_str(), style);
}

void ShowMessage(const std::wstring& title, const std::wstring& text, bool error)
{
    ShowMessageTimed(title, text, error, 0);
}

bool ReadBootConfig(BootConfig& out, std::string& error)
{
    std::string text;
    if (!vfs::ReadText("config/boot.json", text))
    {
        error = "no config/boot.json in the packages";
        return false;
    }
    const nlohmann::json json = nlohmann::json::parse(text, nullptr, false, true);
    if (json.is_discarded() || !json.is_object())
    {
        error = "config/boot.json is not a JSON object";
        return false;
    }
    out.product = json.value("product", std::string());
    out.version = json.value("version", std::string());
    out.bootLevel = json.value("bootLevel", std::string());
    out.startMode = json.value("startMode", std::string("play"));
    return true;
}
}
