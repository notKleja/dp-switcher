#pragma once
#include <windows.h>

// Custom window messages used to route background-thread results back to the UI thread's
// WndProc, kept in one place so the numeric offsets can't drift out of sync between files.
inline constexpr UINT WM_APP_TRAYICON = WM_APP + 1;       // Shell_NotifyIconW callback
inline constexpr UINT WM_APP_DDC_RESULT_MSG = WM_APP + 2; // DdcWorker result ready
inline constexpr UINT WM_APP_RENDER_ANIM_DONE_MSG = WM_APP + 3; // async renderer animation finished
