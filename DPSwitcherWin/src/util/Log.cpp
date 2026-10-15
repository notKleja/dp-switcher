#include "Log.h"
#include <windows.h>
#include <shlobj.h>
#include <cstdio>
#include <cstdarg>
#include <mutex>
#include <string>

namespace {
    constexpr int kRotatedFilesKept = 3;           // log.1.txt .. log.3.txt
    constexpr ULONGLONG kRotateAtBytes = 1u << 20;  // ~1MB

    std::wstring g_logPath;
    std::mutex g_logMutex;

    std::wstring NowTimestamp() {
        SYSTEMTIME st;
        GetLocalTime(&st);
        wchar_t buf[64];
        swprintf_s(buf, L"%04d-%02d-%02dT%02d:%02d:%02d.%03d",
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
        return buf;
    }

    bool EnsureDirectory(const std::wstring& dir) {
        CreateDirectoryW(dir.c_str(), nullptr);
        DWORD attrs = GetFileAttributesW(dir.c_str());
        return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY);
    }

    std::wstring RotatedName(const std::wstring& dir, int index) {
        return dir + L"\\log." + std::to_wstring(index) + L".txt";
    }

    void RotateIfLarge(const std::wstring& dir, const std::wstring& path) {
        WIN32_FILE_ATTRIBUTE_DATA fad{};
        if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) return;
        ULARGE_INTEGER size;
        size.HighPart = fad.nFileSizeHigh;
        size.LowPart = fad.nFileSizeLow;
        if (size.QuadPart <= kRotateAtBytes) return;

        DeleteFileW(RotatedName(dir, kRotatedFilesKept).c_str());
        for (int i = kRotatedFilesKept - 1; i >= 1; --i) {
            MoveFileExW(RotatedName(dir, i).c_str(), RotatedName(dir, i + 1).c_str(), MOVEFILE_REPLACE_EXISTING);
        }
        MoveFileExW(path.c_str(), RotatedName(dir, 1).c_str(), MOVEFILE_REPLACE_EXISTING);
    }

    bool ProbeWritable(const std::wstring& path) {
        HANDLE h = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        CloseHandle(h);
        return true;
    }
}

bool LogInit() {
    std::lock_guard<std::mutex> lock(g_logMutex);
    g_logPath.clear();

    std::wstring candidates[2];
    PWSTR localAppData = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &localAppData))) {
        candidates[0] = std::wstring(localAppData) + L"\\DPSwitcher";
        CoTaskMemFree(localAppData);
    }
    wchar_t temp[MAX_PATH]{};
    if (GetTempPathW(MAX_PATH, temp) > 0) {
        candidates[1] = std::wstring(temp) + L"DPSwitcher"; // GetTempPath ends with a backslash
    }

    for (const auto& dir : candidates) {
        if (dir.empty() || !EnsureDirectory(dir)) continue;
        std::wstring path = dir + L"\\log.txt";
        RotateIfLarge(dir, path);
        if (!ProbeWritable(path)) continue;
        g_logPath = path;
        return true;
    }
    OutputDebugStringW(L"DPSwitcher: no writable log location; logging to debugger only\r\n");
    return false;
}

const wchar_t* LogPath() {
    return g_logPath.c_str();
}

void LogLine(const wchar_t* fmt, ...) {
    wchar_t msg[2048];
    va_list args;
    va_start(args, fmt);
    // _TRUNCATE: an oversized line is cut, never a crash (vswprintf_s would invoke the
    // invalid-parameter handler and terminate the process in release builds).
    _vsnwprintf_s(msg, _countof(msg), _TRUNCATE, fmt, args);
    va_end(args);

    std::wstring line = L"[" + NowTimestamp() + L"] " + msg + L"\r\n";
    OutputDebugStringW(line.c_str());

    std::lock_guard<std::mutex> lock(g_logMutex);
    if (g_logPath.empty()) return;
    HANDLE h = CreateFileW(g_logPath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    // Narrow write must never crash the app; logging is best-effort.
    int needed = WideCharToMultiByte(CP_UTF8, 0, line.c_str(), (int)line.size(), nullptr, 0, nullptr, nullptr);
    if (needed > 0) {
        std::string utf8(needed, '\0');
        WideCharToMultiByte(CP_UTF8, 0, line.c_str(), (int)line.size(), utf8.data(), needed, nullptr, nullptr);
        WriteFile(h, utf8.data(), (DWORD)utf8.size(), &written, nullptr);
    }
    CloseHandle(h);
}
