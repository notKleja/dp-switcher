#pragma once
#include "../pch.h"

namespace abi_fx = ABI::Windows::Graphics::Effects;

// {1FEB6D69-2FE6-4AC9-8C58-1D7F93E7A6A5}
inline constexpr GUID kClsidD2D1GaussianBlur =
{ 0x1feb6d69, 0x2fe6, 0x4ac9, { 0x8c, 0x58, 0x1d, 0x7f, 0x93, 0xe7, 0xa6, 0xa5 } };

// Minimal hand-rolled IGraphicsEffect implementing the D2D Gaussian blur, so
// Compositor::CreateEffectFactory can build an animatable CompositionEffectBrush without
// depending on Win2D. GetNamedPropertyMapping is what makes "Blur.BlurAmount" animatable.
struct GaussianBlurEffect : winrt::implements<GaussianBlurEffect,
    winrt::Windows::Graphics::Effects::IGraphicsEffect,
    winrt::Windows::Graphics::Effects::IGraphicsEffectSource,
    abi_fx::IGraphicsEffectD2D1Interop>
{
    float  BlurAmount = 0.0f;
    UINT32 Optimization = 1;   // D2D1_GAUSSIANBLUR_OPTIMIZATION_BALANCED
    UINT32 BorderMode = 1;     // D2D1_BORDER_MODE_HARD  (avoid dark fringe at screen edges)
    winrt::Windows::Graphics::Effects::IGraphicsEffectSource Source{ nullptr };

    // --- IGraphicsEffect ---
    winrt::hstring Name() const { return m_name; }
    void Name(winrt::hstring const& v) { m_name = v; }

    // --- IGraphicsEffectD2D1Interop ---
    HRESULT STDMETHODCALLTYPE GetEffectId(GUID* id) override {
        if (!id) return E_POINTER;
        *id = kClsidD2D1GaussianBlur;
        return S_OK;
    }

    // REQUIRED for animation: CreateEffectFactory(effect, {L"Blur.BlurAmount"}) resolves names here.
    HRESULT STDMETHODCALLTYPE GetNamedPropertyMapping(LPCWSTR name, UINT* index,
        abi_fx::GRAPHICS_EFFECT_PROPERTY_MAPPING* mapping) override {
        if (!name || !index || !mapping) return E_POINTER;
        if (_wcsicmp(name, L"BlurAmount") == 0) *index = 0; // D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION
        else if (_wcsicmp(name, L"Optimization") == 0) *index = 1; // D2D1_GAUSSIANBLUR_PROP_OPTIMIZATION
        else if (_wcsicmp(name, L"BorderMode") == 0) *index = 2; // D2D1_GAUSSIANBLUR_PROP_BORDER_MODE
        else return E_INVALIDARG;
        *mapping = abi_fx::GRAPHICS_EFFECT_PROPERTY_MAPPING_DIRECT;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetPropertyCount(UINT* count) override {
        if (!count) return E_POINTER;
        *count = 3;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetProperty(UINT index,
        ABI::Windows::Foundation::IPropertyValue** value) override try {
        if (!value) return E_POINTER;
        *value = nullptr;
        using winrt::Windows::Foundation::PropertyValue;
        winrt::Windows::Foundation::IInspectable boxed;
        switch (index) {
        case 0: boxed = PropertyValue::CreateSingle(BlurAmount);   break;
        case 1: boxed = PropertyValue::CreateUInt32(Optimization); break;
        case 2: boxed = PropertyValue::CreateUInt32(BorderMode);   break;
        default: return E_INVALIDARG;
        }
        auto pv = boxed.as<winrt::Windows::Foundation::IPropertyValue>();
        *value = static_cast<ABI::Windows::Foundation::IPropertyValue*>(winrt::detach_abi(pv));
        return S_OK;
    } catch (...) { return winrt::to_hresult(); }

    HRESULT STDMETHODCALLTYPE GetSource(UINT index, abi_fx::IGraphicsEffectSource** source) override {
        if (!source) return E_POINTER;
        *source = nullptr;
        if (index != 0 || !Source) return E_INVALIDARG;
        void* raw = nullptr;
        winrt::copy_to_abi(Source, raw);                 // AddRef'd
        *source = static_cast<abi_fx::IGraphicsEffectSource*>(raw);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetSourceCount(UINT* count) override {
        if (!count) return E_POINTER;
        *count = 1;
        return S_OK;
    }

private:
    winrt::hstring m_name{ L"Blur" };
};
