#pragma once
#include <unknwn.h>                    // BEFORE winrt: enables classic-COM interfaces in winrt::implements
#include <windows.h>
#include <dwmapi.h>
#include <shellscalingapi.h>
#include <DispatcherQueue.h>           // CreateDispatcherQueueController
#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <d2d1_3.h>
#include <d2d1effects_2.h>
#include <dcomp.h>
#include <PhysicalMonitorEnumerationAPI.h>
#include <LowLevelMonitorConfigurationAPI.h>
#include <shellapi.h>
#include <commctrl.h>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.Numerics.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.h>
#include <winrt/Windows.UI.Composition.h>
#include <winrt/Windows.UI.Composition.Desktop.h>
#include <winrt/Windows.Graphics.Effects.h>

#include <windows.ui.composition.interop.h>      // ABI::...::Desktop::ICompositorDesktopInterop
#include <windows.graphics.effects.interop.h>    // ABI::Windows::Graphics::Effects::IGraphicsEffectD2D1Interop

#include <wrl/client.h>
#include <string>
#include <memory>
#include <functional>
#include <chrono>
#include <atomic>
#include <thread>
#include <mutex>
#include <optional>

template <typename T>
using com_ptr = winrt::com_ptr<T>;
