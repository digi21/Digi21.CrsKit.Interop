#pragma once
#include "Transformation.g.h"

namespace winrt::Digi21::CrsKit::Interop::implementation
{
    struct Transformation : TransformationT<Transformation>
    {
        Transformation() = default;
        explicit Transformation(
            std::shared_ptr<::CrsKit::CoordinateTransformations::ICoordinateTransformation> const& transform);

        hstring SourceName();
        hstring TargetName();
        int32_t SourceDimension();
        int32_t TargetDimension();
        bool IsIdentity();
        com_array<double> Transform(array_view<double const> point);
        com_array<double> TransformPoints(array_view<double const> flatPoints);

    private:
        std::shared_ptr<::CrsKit::CoordinateTransformations::ICoordinateTransformation> _coordinateTransformation;
        std::shared_ptr<::CrsKit::CoordinateTransformations::IMathTransform> _mathTransform;
    };
}
