#include "pch.h"
#include "CrsEngine.h"
#include "CrsEngine.g.cpp"
#include "CrsInfo.h"
#include "CrsDetails.h"
#include "CoordinateOperationInfo.h"
#include "Transformation.h"

using namespace CrsKit;
using namespace CrsKit::CoordinateSystems;
using namespace CrsKit::CoordinateTransformations;

namespace
{
    std::once_flag g_initFlag;
    std::atomic<bool> g_initialized{ false };

    // Converts a CrsKit std::exception to winrt::hresult_error with E_FAIL and the original error message.
    [[noreturn]] void ThrowWinrtError(std::exception const& e)
    {
        throw winrt::hresult_error(E_FAIL, winrt::to_hstring(e.what()));
    }

    void EnsureInitialized()
    {
        if (!g_initialized.load())
            throw winrt::hresult_error(
                E_FAIL, L"CrsEngine.Initialize must be called before using the coordinate engine.");
    }

    std::shared_ptr<CoordinateSystem> MakeCoordinateSystem(int code)
    {
        auto coordinateSystem = GetCoordinateSystemAuthorityFactory()->CreateCoordinateSystem(code);
        if (!coordinateSystem)
            throw winrt::hresult_error(
                E_FAIL, winrt::to_hstring(std::format("Unknown or unsupported EPSG coordinate system: {}.", code)));
        return coordinateSystem;
    }

    // Marshal one native candidate operation into its projected read-only view.
    winrt::Digi21::CrsKit::Interop::CoordinateOperationInfo MakeOperationInfo(CoordinateOperation const& candidateOperation)
    {
        winrt::Windows::Foundation::IReference<double> accuracy{ nullptr };
        if (candidateOperation.Accuracy.has_value())
            accuracy = winrt::box_value(candidateOperation.Accuracy.value())
                           .as<winrt::Windows::Foundation::IReference<double>>();

        std::string gridFiles;
        for (auto const& file : candidateOperation.GridFiles)
        {
            if (!gridFiles.empty()) gridFiles += ", ";
            gridFiles += file;
        }

        return winrt::make<winrt::Digi21::CrsKit::Interop::implementation::CoordinateOperationInfo>(
            candidateOperation.Code,
            winrt::to_hstring(candidateOperation.Name),
            winrt::to_hstring(candidateOperation.Type),
            accuracy,
            winrt::to_hstring(candidateOperation.Scope.value_or("")),
            winrt::to_hstring(candidateOperation.Remarks.value_or("")),
            candidateOperation.MethodCode,
            winrt::to_hstring(candidateOperation.AreaOfUse),
            winrt::to_hstring(gridFiles));
    }

    // Returns true if wkt is one element NAME[...] whose brackets and quotes are balanced and that has nothing after it.
    // crskit's parser can fail with an access violation on some truncated texts, such as "]" or a PROJCS cut inside AXIS.
    bool IsCompleteWktElement(std::wstring_view const wkt)
    {
        size_t wktCharacterIndex{};
        while (wktCharacterIndex < wkt.size() && iswalpha(wkt[wktCharacterIndex]))
            ++wktCharacterIndex;
        if (0 == wktCharacterIndex || wktCharacterIndex >= wkt.size() || L'[' != wkt[wktCharacterIndex])
            return false;

        int depth{};
        bool insideQuotes{};
        for (; wktCharacterIndex < wkt.size(); ++wktCharacterIndex)
        {
            auto const character = wkt[wktCharacterIndex];
            if (L'"' == character)
                insideQuotes = !insideQuotes;
            else if (insideQuotes)
                continue;
            else if (L'[' == character)
                ++depth;
            else if (L']' == character && 0 == --depth)
                return wktCharacterIndex == wkt.size() - 1;
        }
        return false;
    }

    std::shared_ptr<CoordinateSystem> CreateCoordinateSystemFromWkt(winrt::hstring const& wkt)
    {
        auto coordinateSystem = GetCoordinateSystemFactory()->CreateFromWkt(winrt::to_string(wkt));
        if (!coordinateSystem)
            throw winrt::hresult_error(E_FAIL, L"The WKT is not a valid coordinate reference system.");
        return coordinateSystem;
    }

    template <class... Visitors> struct Overloaded : Visitors... { using Visitors::operator()...; };

    std::shared_ptr<CoordinateSystem> ApplyAxisOrder(
        std::shared_ptr<CoordinateSystem> const& coordinateSystem, winrt::Digi21::CrsKit::Interop::AxisOrder order)
    {
        switch (order)
        {
        case winrt::Digi21::CrsKit::Interop::AxisOrder::EastNorth:
            return GetCoordinateSystemFactory()->ModifyWithAxisEastNorth(coordinateSystem);
        case winrt::Digi21::CrsKit::Interop::AxisOrder::NorthEast:
            return GetCoordinateSystemFactory()->ModifyWithAxisNorthEast(coordinateSystem);
        default:
            return coordinateSystem;
        }
    }
}

namespace winrt::Digi21::CrsKit::Interop::implementation
{
    void CrsEngine::Initialize(hstring const& epsgDatabaseFilePath, hstring const& gridFilesDirectoryPath)
    {
        try
        {
            std::call_once(g_initFlag, [&]()
            {
                ::CrsKit::Initialize(std::make_shared<Epsg::SQliteProvider>(winrt::to_string(epsgDatabaseFilePath)));
                if (!gridFilesDirectoryPath.empty())
                    ::CrsKit::GetDefaultContext()->dataDirectory = winrt::to_string(gridFilesDirectoryPath);
                g_initialized.store(true);
            });
        }
        catch (winrt::hresult_error const&) { throw; }
        catch (std::exception const& e) { ThrowWinrtError(e); }
    }

    bool CrsEngine::IsInitialized()
    {
        return g_initialized.load();
    }

    hstring CrsEngine::GetEpsgVersion()
    {
        try { return winrt::to_hstring(::CrsKit::GetEpsgVersion()); }
        catch (...) { return {}; }
    }

    winrt::Windows::Foundation::Collections::IVector<winrt::Digi21::CrsKit::Interop::CrsInfo>
        CrsEngine::EnumerateCoordinateSystems(hstring const& kind)
    {
        EnsureInitialized();
        try
        {
            auto const coordinateSystemNamesByCode = GetCoordinateSystemAuthorityFactory()->EnumerateCoordinateSystems(winrt::to_string(kind));
            auto result = winrt::single_threaded_vector<winrt::Digi21::CrsKit::Interop::CrsInfo>();
            for (auto const& [code, name] : coordinateSystemNamesByCode)
                result.Append(winrt::make<implementation::CrsInfo>(code, winrt::to_hstring(name), kind));
            return result;
        }
        catch (winrt::hresult_error const&) { throw; }
        catch (std::exception const& e) { ThrowWinrtError(e); }
    }

    hstring CrsEngine::GetName(int32_t code)
    {
        EnsureInitialized();
        try { return winrt::to_hstring(MakeCoordinateSystem(code)->GetName()); }
        catch (winrt::hresult_error const&) { throw; }
        catch (std::exception const& e) { ThrowWinrtError(e); }
    }

    hstring CrsEngine::GetAreaOfUse(int32_t code)
    {
        EnsureInitialized();
        try { return winrt::to_hstring(GetCoordinateSystemAuthorityFactory()->GetDescriptionAreaApplicationCrs(code)); }
        catch (winrt::hresult_error const&) { throw; }
        catch (std::exception const& e) { ThrowWinrtError(e); }
    }

    hstring CrsEngine::GetWkt(int32_t code)
    {
        EnsureInitialized();
        try { return winrt::to_hstring(MakeCoordinateSystem(code)->GetWkt()); }
        catch (winrt::hresult_error const&) { throw; }
        catch (std::exception const& e) { ThrowWinrtError(e); }
    }

    winrt::Digi21::CrsKit::Interop::CrsDetails CrsEngine::GetDetails(int32_t code)
    {
        EnsureInitialized();
        try
        {
            auto const coordinateSystemAuthorityFactory = GetCoordinateSystemAuthorityFactory();
            auto getStringOrEmpty = [](auto getString) -> hstring
            {
                try { return winrt::to_hstring(getString()); } catch (...) { return {}; }
            };

            hstring const name = getStringOrEmpty([&] { return MakeCoordinateSystem(code)->GetName(); });
            hstring const kind = getStringOrEmpty([&] { return coordinateSystemAuthorityFactory->GetKindOfCoordinateSystem(code); });
            hstring const areaOfUse = getStringOrEmpty([&] { return coordinateSystemAuthorityFactory->GetDescriptionAreaApplicationCrs(code); });

            int32_t axisCount{};
            try
            {
                axisCount = static_cast<int32_t>(
                    coordinateSystemAuthorityFactory->GetAxisOfCoordinateSystem(coordinateSystemAuthorityFactory->GetCodeOfCoordinateSystemAssociatedWithCrs(code)).size());
            }
            catch (...) {}

            hstring datumName, datumOrigin, primeMeridianName, ellipsoidName;
            try
            {
                int const datumCode = coordinateSystemAuthorityFactory->GetCodeOfDatumAssociatedWithCrs(code);
                datumName = getStringOrEmpty([&] { return coordinateSystemAuthorityFactory->GetNameOfDatum(datumCode); });
                datumOrigin = getStringOrEmpty([&] { return coordinateSystemAuthorityFactory->GetOriginDescriptionOfDatum(datumCode); });
                ellipsoidName = getStringOrEmpty([&] { return coordinateSystemAuthorityFactory->GetNameOfEllipsoid(coordinateSystemAuthorityFactory->GetCodeOfEllipsoidAssociatedWithDatum(datumCode)); });
            }
            catch (...) {}
            primeMeridianName = getStringOrEmpty([&] { return coordinateSystemAuthorityFactory->GetNameOfPrimeMeridian(coordinateSystemAuthorityFactory->GetCodeOfPrimeMeridianAssociatedWithCrs(code)); });

            return winrt::make<implementation::CrsDetails>(
                code, name, kind, axisCount, areaOfUse, datumName, datumOrigin, primeMeridianName, ellipsoidName);
        }
        catch (winrt::hresult_error const&) { throw; }
        catch (std::exception const& e) { ThrowWinrtError(e); }
    }

    hstring CrsEngine::GetWktWithAxisOrder(int32_t code, winrt::Digi21::CrsKit::Interop::AxisOrder order)
    {
        EnsureInitialized();
        try { return winrt::to_hstring(ApplyAxisOrder(MakeCoordinateSystem(code), order)->GetWkt()); }
        catch (winrt::hresult_error const&) { throw; }
        catch (std::exception const& e) { ThrowWinrtError(e); }
    }

    hstring CrsEngine::GetCatalogueAxisNamesLabel(int32_t code)
    {
        EnsureInitialized();
        try
        {
            auto const coordinateSystem = MakeCoordinateSystem(code);
            // GetCatalogueAxisNamesLabel joins with "-" the non-empty catalogue names it can read from up to
            // the first two axes.
            int const dimension = coordinateSystem->GetDimension();
            int const inspectedAxisCount = dimension < 2 ? dimension : 2;
            std::string label;
            for (int axisIndex = 0; axisIndex < inspectedAxisCount; ++axisIndex)
            {
                std::string name;
                try { name = coordinateSystem->GetAxis(axisIndex).GetName(); } catch (...) {}
                if (name.empty()) continue;
                if (!label.empty()) label += "-";
                label += name;
            }
            return winrt::to_hstring(label);
        }
        catch (...) { return {}; }
    }

    hstring CrsEngine::GetCompoundWkt(int32_t horizontalCode, int32_t verticalCode)
    {
        EnsureInitialized();
        try
        {
            auto const compound =
                GetCoordinateSystemAuthorityFactory()->CreateCompoundCoordinateSystem(horizontalCode, verticalCode);
            if (!compound)
                throw winrt::hresult_error(E_FAIL, L"Could not build the compound coordinate system.");
            return winrt::to_hstring(compound->GetWkt());
        }
        catch (winrt::hresult_error const&) { throw; }
        catch (std::exception const& e) { ThrowWinrtError(e); }
    }

    hstring CrsEngine::GetCrsNameFromWkt(hstring const& wkt)
    {
        EnsureInitialized();
        try
        {
            auto const coordinateSystem = GetCoordinateSystemFactory()->CreateFromWkt(winrt::to_string(wkt));
            if (!coordinateSystem)
                throw winrt::hresult_error(E_FAIL, L"The WKT is not a valid coordinate reference system.");
            return winrt::to_hstring(coordinateSystem->GetName());
        }
        catch (winrt::hresult_error const&) { throw; }
        catch (std::exception const& e) { ThrowWinrtError(e); }
    }

    hstring CrsEngine::DescribeAxes(hstring const& wkt)
    {
        EnsureInitialized();
        try
        {
            auto const coordinateSystem = GetCoordinateSystemFactory()->CreateFromWkt(winrt::to_string(wkt));
            if (!coordinateSystem)
                throw winrt::hresult_error(E_FAIL, L"The WKT is not a valid coordinate reference system.");

            auto getUnitName = [](AnyUnit const& unit) -> std::string
            {
                return std::visit([](auto const& concreteUnit) { return concreteUnit.GetName(); }, unit);
            };

            std::string result;
            int const dimension = coordinateSystem->GetDimension();
            for (int axisIndex = 0; axisIndex < dimension; ++axisIndex)
            {
                std::string axisName, unitName;
                try { axisName = coordinateSystem->GetAxis(axisIndex).GetName(); } catch (...) {}
                try { unitName = getUnitName(coordinateSystem->GetUnits(axisIndex)); } catch (...) {}
                if (axisName.empty()) axisName = std::format("Axis {}", axisIndex + 1);

                if (!result.empty()) result += ", ";
                result += axisName;
                if (!unitName.empty()) result += std::format(" ({})", unitName);
            }
            return winrt::to_hstring(result);
        }
        catch (winrt::hresult_error const&) { throw; }
        catch (std::exception const& e) { ThrowWinrtError(e); }
    }

    winrt::Digi21::CrsKit::Interop::Transformation CrsEngine::CreateTransformation(int32_t sourceCode, int32_t targetCode)
    {
        EnsureInitialized();
        try
        {
            auto const source = MakeCoordinateSystem(sourceCode);
            auto const target = MakeCoordinateSystem(targetCode);
            auto const coordinateTransformation = GetCoordinateTransformationFactory()->CreateFromCoordinateSystems(source, target);
            return winrt::make<implementation::Transformation>(coordinateTransformation);
        }
        catch (winrt::hresult_error const&) { throw; }
        catch (std::exception const& e) { ThrowWinrtError(e); }
    }

    winrt::Digi21::CrsKit::Interop::Transformation CrsEngine::CreateTransformationFromWkt(
        hstring const& sourceWkt, hstring const& targetWkt)
    {
        EnsureInitialized();
        try
        {
            auto const source = GetCoordinateSystemFactory()->CreateFromWkt(winrt::to_string(sourceWkt));
            auto const target = GetCoordinateSystemFactory()->CreateFromWkt(winrt::to_string(targetWkt));
            auto const coordinateTransformation = GetCoordinateTransformationFactory()->CreateFromCoordinateSystems(source, target);
            return winrt::make<implementation::Transformation>(coordinateTransformation);
        }
        catch (winrt::hresult_error const&) { throw; }
        catch (std::exception const& e) { ThrowWinrtError(e); }
    }

    winrt::Windows::Foundation::Collections::IVectorView<winrt::Digi21::CrsKit::Interop::CoordinateOperationInfo>
        CrsEngine::GetCandidateOperations(hstring const& sourceWkt, hstring const& targetWkt)
    {
        EnsureInitialized();
        try
        {
            auto const source = GetCoordinateSystemFactory()->CreateFromWkt(winrt::to_string(sourceWkt));
            auto const target = GetCoordinateSystemFactory()->CreateFromWkt(winrt::to_string(targetWkt));

            // CrsKit invokes selectOperation only when more than one operation applies. The callback stores the candidate
            // operations; this method discards the transformation that CrsKit then builds.
            std::vector<CoordinateOperation> candidateOperations;
            bool hasReceivedCandidateOperations{ false };
            CoordinateTransformationOptions options{};
            options.selectOperation =
                [&candidateOperations, &hasReceivedCandidateOperations](std::string const&, std::string const&,
                                         std::vector<CoordinateOperation> const& availableCandidateOperations) -> int
                {
                    candidateOperations = availableCandidateOperations;
                    hasReceivedCandidateOperations = true;
                    return availableCandidateOperations.empty() ? 0 : availableCandidateOperations.front().Code; // CrsKit builds the discarded transformation with the first operation
                };

            try
            {
                (void)GetCoordinateTransformationFactory()->CreateFromCoordinateSystems(source, target, options);
            }
            catch (...)
            {
                // After CrsKit has passed the candidate operations, a failure to create the transformation with the first candidate operation does not propagate:
                // another candidate may be usable. The failure propagates only if CrsKit did not invoke the callback.
                if (!hasReceivedCandidateOperations) throw;
            }

            auto result = winrt::single_threaded_vector<winrt::Digi21::CrsKit::Interop::CoordinateOperationInfo>();
            for (auto const& candidateOperation : candidateOperations)
                result.Append(MakeOperationInfo(candidateOperation));
            return result.GetView();
        }
        catch (winrt::hresult_error const&) { throw; }
        catch (std::exception const& e) { ThrowWinrtError(e); }
    }

    winrt::Digi21::CrsKit::Interop::Transformation CrsEngine::CreateTransformationFromWktWithOperation(
        hstring const& sourceWkt, hstring const& targetWkt, int32_t operationCode)
    {
        EnsureInitialized();
        try
        {
            auto const source = GetCoordinateSystemFactory()->CreateFromWkt(winrt::to_string(sourceWkt));
            auto const target = GetCoordinateSystemFactory()->CreateFromWkt(winrt::to_string(targetWkt));

            CoordinateTransformationOptions options{};
            options.selectOperation =
                [operationCode](std::string const&, std::string const&,
                                std::vector<CoordinateOperation> const&) -> int { return operationCode; };

            auto const coordinateTransformation = GetCoordinateTransformationFactory()->CreateFromCoordinateSystems(source, target, options);
            return winrt::make<implementation::Transformation>(coordinateTransformation);
        }
        catch (winrt::hresult_error const&) { throw; }
        catch (std::exception const& e) { ThrowWinrtError(e); }
    }

    bool CrsEngine::IsValidWkt(hstring const& wkt)
    {
        EnsureInitialized();
        if (!IsCompleteWktElement(wkt))
            return false;
        try { return nullptr != GetCoordinateSystemFactory()->CreateFromWkt(winrt::to_string(wkt)); }
        catch (...) { return false; }
    }

    bool CrsEngine::AreEquivalent(hstring const& firstWkt, hstring const& secondWkt)
    {
        EnsureInitialized();
        try
        {
            auto const factory = GetCoordinateSystemFactory();
            return ::CrsKit::CoordinateSystems::AreEquivalent(
                factory->CreateFromWkt(winrt::to_string(firstWkt)), factory->CreateFromWkt(winrt::to_string(secondWkt)));
        }
        catch (...) { return false; }
    }

    hstring CrsEngine::GetHorizontalWkt(hstring const& wkt)
    {
        EnsureInitialized();
        try
        {
            auto const horizontal = GetUtilities()->ExtractHorizontalVertical(CreateCoordinateSystemFromWkt(wkt)).first;
            return horizontal ? winrt::to_hstring(horizontal->GetWkt()) : hstring{};
        }
        catch (winrt::hresult_error const&) { throw; }
        catch (std::exception const& e) { ThrowWinrtError(e); }
    }

    hstring CrsEngine::GetVerticalWkt(hstring const& wkt)
    {
        EnsureInitialized();
        try
        {
            // ExtractHorizontalVertical returns the WKT of an unknown vertical system when it receives a WKT without a vertical
            // component. For a parsed system without a vertical component, ExtractHorizontalVertical returns null for the
            // vertical component, and GetVerticalWkt converts that null to an empty string.
            auto const vertical = GetUtilities()->ExtractHorizontalVertical(CreateCoordinateSystemFromWkt(wkt)).second;
            return vertical ? winrt::to_hstring(vertical->GetWkt()) : hstring{};
        }
        catch (winrt::hresult_error const&) { throw; }
        catch (std::exception const& e) { ThrowWinrtError(e); }
    }

    bool CrsEngine::IsHorizontalComponentLocal(hstring const& wkt)
    {
        EnsureInitialized();
        try
        {
            auto const horizontal = GetUtilities()->ExtractHorizontalVertical(CreateCoordinateSystemFromWkt(wkt)).first;
            return nullptr != std::dynamic_pointer_cast<LocalCoordinateSystem>(horizontal);
        }
        catch (...) { return false; }
    }

    hstring CrsEngine::GetUnknownLocalWkt()
    {
        EnsureInitialized();
        return winrt::to_hstring(GetUtilities()->GetCompoundLocalWkt());
    }

    int32_t CrsEngine::GetEpsgCode(hstring const& wkt)
    {
        EnsureInitialized();
        try { return GetUtilities()->GetEpsgCodeFromWkt(winrt::to_string(wkt)); }
        catch (...) { return 0; }
    }

    hstring CrsEngine::GetWktFromGeoKeys(uint16_t const modelType, uint16_t const projectedCrsCode,
        uint16_t const geographicCrsCode, uint16_t const projectionCode)
    {
        EnsureInitialized();
        // GeoTIFF reserves 32767 for a user-defined value.
        constexpr uint16_t userDefined = 32767;
        try
        {
            auto const utilities = GetUtilities();
            switch (modelType)
            {
            case 1: // GTModelTypeGeoKey: ModelTypeProjected
                if (userDefined != projectedCrsCode)
                    return winrt::to_hstring(utilities->GetWktFromEpsgCode(projectedCrsCode));
                if (userDefined == projectionCode)
                    return {};
                return winrt::to_hstring(utilities->GetWktFromProjectionCodeForGeographic(projectionCode, geographicCrsCode));
            case 2: // GTModelTypeGeoKey: ModelTypeGeographic, with the longitude first, like X and Y of a LAS file
                return winrt::to_hstring(utilities->GetWktFromGeographicEpsgCode(geographicCrsCode, true));
            default: // geocentric or missing
                return {};
            }
        }
        catch (...) { return {}; }
    }

    winrt::Digi21::CrsKit::Interop::CrsUnit CrsEngine::GetFirstAxisUnit(hstring const& wkt)
    {
        EnsureInitialized();
        try
        {
            // GetUnits(0) of a compound system returns the unit of its horizontal component.
            return std::visit(Overloaded{
                [](LinearUnit const& unit) { return winrt::Digi21::CrsKit::Interop::CrsUnit{ winrt::to_hstring(unit.GetName()), false, unit.GetMetersPerUnit(), 0.0 }; },
                [](AngularUnit const& unit) { return winrt::Digi21::CrsKit::Interop::CrsUnit{ winrt::to_hstring(unit.GetName()), true, 0.0, unit.GetRadiansPerUnit() }; } },
                CreateCoordinateSystemFromWkt(wkt)->GetUnits(0));
        }
        catch (winrt::hresult_error const&) { throw; }
        catch (std::exception const& e) { ThrowWinrtError(e); }
    }

    winrt::Digi21::CrsKit::Interop::Transformation CrsEngine::CreateEastNorthToWgs84LatitudeLongitude(hstring const& sourceWkt)
    {
        EnsureInitialized();
        try
        {
            auto const utilities = GetUtilities();
            auto const horizontalWkt = utilities->ExtractHorizontalVertical(winrt::to_string(sourceWkt)).first;

            // The transformation expects {easting, northing} or {longitude, latitude}, whatever axis order the source CRS declares.
            auto const source = GetCoordinateSystemFactory()->CreateFromWkt(utilities->GetWktWithEastNorthAxes(horizontalWkt));
            // EPSG:4326 declares the latitude first, so Transform returns {latitude, longitude} in degrees.
            auto const target = GetCoordinateSystemAuthorityFactory()->CreateGeographicCoordinateSystem(4326);

            auto const coordinateTransformation = GetCoordinateTransformationFactory()->CreateFromCoordinateSystems(source, target);
            if (!coordinateTransformation)
                throw winrt::hresult_error(E_FAIL, L"No coordinate operation converts this system to WGS 84.");
            return winrt::make<implementation::Transformation>(coordinateTransformation);
        }
        catch (winrt::hresult_error const&) { throw; }
        catch (std::exception const& e) { ThrowWinrtError(e); }
    }
}
