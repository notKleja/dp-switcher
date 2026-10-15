#pragma once
#include <windows.h>

// Plain data shared between the DDC worker, the controller, and the test harness. Kept free
// of the heavy pch.h so the controller (and its tests) compile without C++/WinRT.
struct DdcRequest {
    enum Kind { Read, Write } kind;
    HMONITOR mon;
    BYTE vcp;
    DWORD value; // Write only
    UINT64 id;
};

struct DdcResult {
    UINT64 id;
    HRESULT hr;
    DWORD current;
    DWORD maximum;
};
