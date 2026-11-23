#include "desktop_view.hpp"
#include "bridge.hpp"

#include <stdexcept>
#include <cwchar>

#ifdef _MSC_VER
#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#endif

using namespace opencap;

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR line, int show) {
    const auto args = std::wstring_view(line);
    constexpr std::wstring_view testPrefix = L"--demo --test-session ";
    constexpr std::wstring_view steamPrefix = L"--steamvr-session ";
    const bool isolated = args.starts_with(testPrefix);
    const auto testOptions = isolated ? args.substr(testPrefix.size()) : std::wstring_view{};
    const auto separator = testOptions.find(L' ');
    const auto testId = testOptions.substr(0, separator);
    const auto ownerOption = isolated
        ? (separator == std::wstring_view::npos ? std::wstring_view{} : testOptions.substr(separator+1))
        : args;
    if (isolated && (testId.empty() || testId.size() > 12 ||
        testId.find_first_not_of(L"0123456789") != std::wstring_view::npos)) return 1;
    const bool demo = args == L"--demo" || isolated;
    const bool steamStarted = ownerOption.starts_with(steamPrefix);
    struct OwnerHandle {
        HANDLE value{};
        ~OwnerHandle() { if (value) CloseHandle(value); }
    } owner;
    if (steamStarted) {
        const auto id = ownerOption.substr(steamPrefix.size());
        if (id.empty() || id.size() > 10 || id.find_first_not_of(L"0123456789") != std::wstring_view::npos) return 1;
        owner.value = OpenProcess(SYNCHRONIZE, FALSE, std::wcstoul(id.data(), nullptr, 10));
        if (!owner.value) return 1;
    }
    auto appLock = std::wstring(L"Local\\OpenCap.Desktop.v1");
    auto session = std::wstring(sessionName);
    if (isolated) {
        appLock.append(L".Checks.").append(testId);
        session.append(L".Checks.").append(testId);
    }
    const auto lock = CreateMutexW(nullptr, FALSE, appLock.c_str());
    if (!lock) return 1;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (!steamStarted) {
            const auto previous = FindWindowW(L"OpenCapDesktop", nullptr);
            if (previous) { ShowWindow(previous, SW_RESTORE); SetForegroundWindow(previous); }
        }
        CloseHandle(lock);
        return 0;
    }
    int result = 0;
    try {
        result = runDesktop(instance, show, demo, session, isolated, owner.value);
    } catch (const std::exception& error) {
        if (!isolated) MessageBoxW(nullptr, widen(error.what()).c_str(), L"OpenCap", MB_ICONERROR);
        result = 1;
    }
    CloseHandle(lock);
    return result;
}
