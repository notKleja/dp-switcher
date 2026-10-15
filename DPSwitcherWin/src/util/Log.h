#pragma once

// Returns false when no writable log location could be found (the app keeps running; lines
// still go to OutputDebugString).
bool LogInit();
const wchar_t* LogPath(); // empty when LogInit failed
void LogLine(const wchar_t* fmt, ...);

#define Log(...) LogLine(__VA_ARGS__)
