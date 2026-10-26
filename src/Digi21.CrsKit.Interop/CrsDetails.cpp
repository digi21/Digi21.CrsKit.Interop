#include "pch.h"
#include "CrsDetails.h"
#include "CrsDetails.g.cpp"

namespace winrt::Digi21::CrsKit::Interop::implementation
{
    CrsDetails::CrsDetails(int32_t code, hstring const& name, hstring const& kind, int32_t axisCount,
                           hstring const& areaOfUse, hstring const& datumName, hstring const& datumOrigin,
                           hstring const& primeMeridianName, hstring const& ellipsoidName)
        : _code{ code }, _name{ name }, _kind{ kind }, _axisCount{ axisCount }, _areaOfUse{ areaOfUse }
        , _datumName{ datumName }, _datumOrigin{ datumOrigin }, _primeMeridianName{ primeMeridianName }
        , _ellipsoidName{ ellipsoidName }
    {
    }

    int32_t CrsDetails::Code() { return _code; }
    hstring CrsDetails::Name() { return _name; }
    hstring CrsDetails::Kind() { return _kind; }
    int32_t CrsDetails::AxisCount() { return _axisCount; }
    hstring CrsDetails::AreaOfUse() { return _areaOfUse; }
    hstring CrsDetails::DatumName() { return _datumName; }
    hstring CrsDetails::DatumOrigin() { return _datumOrigin; }
    hstring CrsDetails::PrimeMeridianName() { return _primeMeridianName; }
    hstring CrsDetails::EllipsoidName() { return _ellipsoidName; }
}
