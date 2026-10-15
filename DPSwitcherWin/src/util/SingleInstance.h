#pragma once
#include <windows.h>

// Holds a named mutex for the process lifetime. A second launch sees ERROR_ALREADY_EXISTS
// and exits instead of adding a second tray icon, overlay, and DDC poller on the same bus.
class SingleInstanceGuard {
public:
    ~SingleInstanceGuard() {
        if (m_mutex) {
            if (m_owned) ReleaseMutex(m_mutex);
            CloseHandle(m_mutex);
        }
    }

    bool Acquire(const wchar_t* name) {
        m_mutex = CreateMutexW(nullptr, TRUE, name);
        if (!m_mutex) return false;
        m_owned = (GetLastError() != ERROR_ALREADY_EXISTS);
        return m_owned;
    }

private:
    HANDLE m_mutex = nullptr;
    bool m_owned = false;
};
