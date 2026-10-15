#pragma once
#include "../pch.h"

// Only one DispatcherQueueController may exist per thread, but renderers are created and
// destroyed repeatedly (tier fallback, the "Force renderer" diagnostics). The UI thread
// therefore owns exactly one controller for the process lifetime and renderers just look
// up the thread's queue.
bool EnsureUiDispatcherQueue();
winrt::Windows::System::DispatcherQueue UiDispatcherQueue();
