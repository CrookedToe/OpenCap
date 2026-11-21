#include "desktop_view.hpp"
#include "bridge.hpp"

#include <stdexcept>

using namespace opencap;

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR line, int show) {
    const auto args = std::wstring_view(line);
    constexpr std::wstring_view testPrefix = L"--demo --test-session ";
    const bool isolated = args.starts_with(testPrefix);
    const auto testId = isolated ? args.substr(testPrefix.size()) : std::wstring_view{};
    if (isolated && (testId.empty() || testId.size() > 12 ||
        testId.find_first_not_of(L"0123456789") != std::wstring_view::npos)) return 1;
    const bool demo = args == L"--demo" || isolated;
    auto appLock = std::wstring(L"Local\\OpenCap.Desktop.v1");
    auto session = std::wstring(sessionName);
    if (isolated) {
        appLock.append(L".Checks.").append(testId);
        session.append(L".Checks.").append(testId);
    }
    const auto lock = CreateMutexW(nullptr, FALSE, appLock.c_str());
    if (!lock) return 1;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        const auto previous = FindWindowW(L"OpenCapDesktop", nullptr);
        if (previous) { ShowWindow(previous, SW_RESTORE); SetForegroundWindow(previous); }
        CloseHandle(lock);
        return 0;
    }
    int result = 0;
    try {
        result = runDesktop(instance, show, demo, session, isolated);
    } catch (const std::exception& error) {
        if (!isolated) MessageBoxW(nullptr, widen(error.what()).c_str(), L"OpenCap", MB_ICONERROR);
        result = 1;
    }
    CloseHandle(lock);
    return result;
}
