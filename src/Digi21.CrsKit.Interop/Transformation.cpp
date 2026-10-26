#include "pch.h"
#include "Transformation.h"
#include "Transformation.g.cpp"

using namespace CrsKit::CoordinateSystems;
using namespace CrsKit::CoordinateTransformations;

namespace winrt::Digi21::CrsKit::Interop::implementation
{
    Transformation::Transformation(std::shared_ptr<ICoordinateTransformation> const& transform)
        : _coordinateTransformation{ transform }
        , _mathTransform{ transform ? transform->GetMathTransform() : nullptr }
    {
        if (!_coordinateTransformation || !_mathTransform)
            throw winrt::hresult_error(
                E_FAIL, L"Could not build a transformation between the selected coordinate systems.");
    }

    hstring Transformation::SourceName()
    {
        auto const cs = _coordinateTransformation->GetSourceCS();
        return cs ? winrt::to_hstring(cs->GetName()) : hstring{};
    }

    hstring Transformation::TargetName()
    {
        auto const cs = _coordinateTransformation->GetTargetCS();
        return cs ? winrt::to_hstring(cs->GetName()) : hstring{};
    }

    int32_t Transformation::SourceDimension() { return _mathTransform->GetSourceDimension(); }
    int32_t Transformation::TargetDimension() { return _mathTransform->GetTargetDimension(); }
    bool Transformation::IsIdentity() { return _mathTransform->GetIsIdentity(); }

    com_array<double> Transformation::Transform(array_view<double const> point)
    {
        std::vector<double> const in{ point.begin(), point.end() };
        auto const out = _mathTransform->Transform(in);
        return com_array<double>{ out.begin(), out.end() };
    }

    com_array<double> Transformation::TransformPoints(array_view<double const> flatPoints)
    {
        std::span<double const> const source{ flatPoints.data(), flatPoints.size() };
        auto const out = _mathTransform->TransformPoints(source);
        return com_array<double>{ out.begin(), out.end() };
    }
}
