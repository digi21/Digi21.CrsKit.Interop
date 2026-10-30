#pragma once
#include "CrsEngine.g.h"

namespace winrt::Digi21::CrsKit::Interop::implementation
{
    struct CrsEngine : CrsEngineT<CrsEngine>
    {
        CrsEngine() = default;

        static void Initialize(hstring const& epsgDatabaseFilePath, hstring const& gridFilesDirectoryPath);
        static bool IsInitialized();
        static hstring GetEpsgVersion();
        static winrt::Windows::Foundation::Collections::IVector<winrt::Digi21::CrsKit::Interop::CrsInfo>
            EnumerateCoordinateSystems(hstring const& kind);
        static hstring GetName(int32_t code);
        static hstring GetAreaOfUse(int32_t code);
        static hstring GetWkt(int32_t code);
        static winrt::Digi21::CrsKit::Interop::CrsDetails GetDetails(int32_t code);
        static hstring GetWktWithAxisOrder(int32_t code, winrt::Digi21::CrsKit::Interop::AxisOrder order);
        static hstring GetCatalogueAxisNamesLabel(int32_t code);
        static hstring GetCompoundWkt(int32_t horizontalCode, int32_t verticalCode);
        static hstring GetCrsNameFromWkt(hstring const& wkt);
        static hstring DescribeAxes(hstring const& wkt);
        static winrt::Digi21::CrsKit::Interop::Transformation CreateTransformation(int32_t sourceCode, int32_t targetCode);
        static winrt::Digi21::CrsKit::Interop::Transformation CreateTransformationFromWkt(
            hstring const& sourceWkt, hstring const& targetWkt);
        static winrt::Windows::Foundation::Collections::IVectorView<winrt::Digi21::CrsKit::Interop::CoordinateOperationInfo>
            GetCandidateOperations(hstring const& sourceWkt, hstring const& targetWkt);
        static winrt::Digi21::CrsKit::Interop::Transformation CreateTransformationFromWktWithOperation(
            hstring const& sourceWkt, hstring const& targetWkt, int32_t operationCode);
        static bool IsValidWkt(hstring const& wkt);
        static bool AreEquivalent(hstring const& firstWkt, hstring const& secondWkt);
        static hstring GetHorizontalWkt(hstring const& wkt);
        static hstring GetVerticalWkt(hstring const& wkt);
        static bool IsHorizontalComponentLocal(hstring const& wkt);
        static bool IsVerticalComponentLocal(hstring const& wkt);
        static hstring GetUnknownLocalWkt();
        static int32_t GetEpsgCode(hstring const& wkt);
        static hstring GetWktFromGeoKeys(uint16_t modelType, uint16_t projectedCrsCode, uint16_t geographicCrsCode, uint16_t projectionCode);
        static winrt::Digi21::CrsKit::Interop::CrsUnit GetFirstAxisUnit(hstring const& wkt);
        static winrt::Digi21::CrsKit::Interop::AxisOrientation GetFirstAxisOrientation(hstring const& wkt);
        static winrt::Digi21::CrsKit::Interop::Transformation CreateEastNorthToWgs84LatitudeLongitude(hstring const& sourceWkt, double eastInSourceUnits, double northInSourceUnits);
        static winrt::Windows::Foundation::Collections::IVectorView<winrt::Digi21::CrsKit::Interop::CoordinateOperationInfo>
            GetEastNorthToWgs84CandidateOperations(hstring const& sourceWkt);
        static winrt::Digi21::CrsKit::Interop::Transformation CreateEastNorthToWgs84LatitudeLongitudeWithOperation(
            hstring const& sourceWkt, int32_t operationCode);
    };
}

namespace winrt::Digi21::CrsKit::Interop::factory_implementation
{
    struct CrsEngine : CrsEngineT<CrsEngine, implementation::CrsEngine>
    {
    };
}
