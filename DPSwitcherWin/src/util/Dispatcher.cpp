#include "Dispatcher.h"
#include "Log.h"

namespace {
    winrt::Windows::System::DispatcherQueueController g_controller{ nullptr };
}

bool EnsureUiDispatcherQueue() {
    if (winrt::Windows::System::DispatcherQueue::GetForCurrentThread()) return true;
    DispatcherQueueOptions opt{ sizeof(opt), DQTYPE_THREAD_CURRENT, DQTAT_COM_STA };
    HRESULT hr = CreateDispatcherQueueController(opt,
        reinterpret_cast<ABI::Windows::System::IDispatcherQueueController**>(winrt::put_abi(g_controller)));
    if (FAILED(hr)) {
        Log(L"CreateDispatcherQueueController failed 0x%08X", hr);
        return false;
    }
    return true;
}

winrt::Windows::System::DispatcherQueue UiDispatcherQueue() {
    return winrt::Windows::System::DispatcherQueue::GetForCurrentThread();
}
