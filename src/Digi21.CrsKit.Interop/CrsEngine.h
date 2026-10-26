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
    };
}

namespace winrt::Digi21::CrsKit::Interop::factory_implementation
{
    struct CrsEngine : CrsEngineT<CrsEngine, implementation::CrsEngine>
    {
    };
}
