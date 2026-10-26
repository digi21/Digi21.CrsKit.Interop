#include "pch.h"
#include "CrsInfo.h"
#include "CrsInfo.g.cpp"

namespace winrt::Digi21::CrsKit::Interop::implementation
{
    CrsInfo::CrsInfo(int32_t code, hstring const& name, hstring const& kind)
        : _code{ code }, _name{ name }, _kind{ kind }
    {
    }

    int32_t CrsInfo::Code() { return _code; }
    hstring CrsInfo::Name() { return _name; }
    hstring CrsInfo::Kind() { return _kind; }
}
