#pragma once
#include "CrsInfo.g.h"

namespace winrt::Digi21::CrsKit::Interop::implementation
{
    struct CrsInfo : CrsInfoT<CrsInfo>
    {
        CrsInfo() = default;
        CrsInfo(int32_t code, hstring const& name, hstring const& kind);

        int32_t Code();
        hstring Name();
        hstring Kind();

    private:
        int32_t _code{};
        hstring _name;
        hstring _kind;
    };
}

// No factory_implementation: CrsInfo has no activation constructor and no static
// members, so CppWinRT generates no activation factory. It is created internally
// via winrt::make<implementation::CrsInfo>() and only returned across the application binary interface (ABI).
