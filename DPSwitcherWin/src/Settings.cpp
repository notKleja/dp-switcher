#include "Settings.h"
#include <vector>

namespace {
    constexpr wchar_t kAppKey[] = L"Software\\DPSwitcher";
    constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    constexpr wchar_t kRunValue[] = L"DPSwitcher";

    std::wstring ReadString(const wchar_t* key, const wchar_t* name) {
        DWORD size = 0;
        if (RegGetValueW(HKEY_CURRENT_USER, key, name, RRF_RT_REG_SZ, nullptr, nullptr, &size) != ERROR_SUCCESS || size < sizeof(wchar_t)) {
            return L"";
        }
        std::vector<wchar_t> buf(size / sizeof(wchar_t) + 1, L'\0');
        if (RegGetValueW(HKEY_CURRENT_USER, key, name, RRF_RT_REG_SZ, nullptr, buf.data(), &size) != ERROR_SUCCESS) {
            return L"";
        }
        return std::wstring(buf.data()); // RegGetValue guarantees termination; the +1 is belt and braces
    }

    DWORD ReadDword(const wchar_t* key, const wchar_t* name, DWORD def) {
        DWORD value = def, size = sizeof(value);
        if (RegGetValueW(HKEY_CURRENT_USER, key, name, RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS) {
            return def;
        }
        return value;
    }

    bool WriteString(const wchar_t* key, const wchar_t* name, const std::wstring& value) {
        HKEY h;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, key, 0, nullptr, 0, KEY_WRITE, nullptr, &h, nullptr) != ERROR_SUCCESS) return false;
        LONG res = RegSetValueExW(h, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
            static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(h);
        return res == ERROR_SUCCESS;
    }

    bool WriteDword(const wchar_t* key, const wchar_t* name, DWORD value) {
        HKEY h;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, key, 0, nullptr, 0, KEY_WRITE, nullptr, &h, nullptr) != ERROR_SUCCESS) return false;
        LONG res = RegSetValueExW(h, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
        RegCloseKey(h);
        return res == ERROR_SUCCESS;
    }

    bool DeleteValue(const wchar_t* key, const wchar_t* name) {
        HKEY h;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, key, 0, KEY_WRITE, &h) != ERROR_SUCCESS) return true; // nothing to delete
        LONG res = RegDeleteValueW(h, name);
        RegCloseKey(h);
        return res == ERROR_SUCCESS || res == ERROR_FILE_NOT_FOUND;
    }

    std::wstring QuotedExePath() {
        wchar_t path[MAX_PATH]{};
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        return L"\"" + std::wstring(path) + L"\"";
    }
}

Settings LoadSettings() {
    Settings s;
    s.monitorDevicePath = ReadString(kAppKey, L"MonitorDevicePath");
    s.revealGraceMs = ReadDword(kAppKey, L"RevealGraceMs", 0);
    s.forcedRenderer = ReadString(kAppKey, L"Renderer");
    s.ddcCacheHandles = ReadDword(kAppKey, L"DdcCacheHandles", 1) != 0;
    return s;
}

void SaveMonitorDevicePath(const std::wstring& path) { WriteString(kAppKey, L"MonitorDevicePath", path); }
void SaveRevealGraceMs(UINT ms) { WriteDword(kAppKey, L"RevealGraceMs", ms); }
void SaveForcedRenderer(const std::wstring& tier) { WriteString(kAppKey, L"Renderer", tier); }

bool IsAutostartEnabled() {
    return ReadString(kRunKey, kRunValue) == QuotedExePath();
}

bool SetAutostartEnabled(bool enabled) {
    return enabled ? WriteString(kRunKey, kRunValue, QuotedExePath()) : DeleteValue(kRunKey, kRunValue);
}
