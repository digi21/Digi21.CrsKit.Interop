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

        auto const joinWithCommas = [](std::vector<std::string> const& names)
        {
            std::string joined;
            for (auto const& name : names)
            {
                if (!joined.empty()) joined += ", ";
                joined += name;
            }
            return joined;
        };

        return winrt::make<winrt::Digi21::CrsKit::Interop::implementation::CoordinateOperationInfo>(
            candidateOperation.Code,
            winrt::to_hstring(candidateOperation.Name),
            winrt::to_hstring(candidateOperation.Type),
            accuracy,
            winrt::to_hstring(candidateOperation.Scope.value_or("")),
            winrt::to_hstring(candidateOperation.Remarks.value_or("")),
            candidateOperation.MethodCode,
            winrt::to_hstring(candidateOperation.AreaOfUse),
            winrt::to_hstring(joinWithCommas(candidateOperation.GridFiles)),
            winrt::to_hstring(joinWithCommas(::CrsKit::GetMissingGridFiles(candidateOperation))));
    }

    // Returns the candidate operations that CrsKit passes to selectOperation, or an empty list when CrsKit builds the transformation
    // without invoking the callback because at most one operation applies.
    std::vector<CoordinateOperation> CollectCandidateOperations(std::shared_ptr<CoordinateSystem> const& source, std::shared_ptr<CoordinateSystem> const& target)
    {
        std::vector<CoordinateOperation> candidateOperations;
        bool hasReceivedCandidateOperations{ false };
        CoordinateTransformationOptions options{};
        options.selectOperation =
            [&candidateOperations, &hasReceivedCandidateOperations](std::string const&, std::string const&,
                                     std::vector<CoordinateOperation> const& availableCandidateOperations) -> int
            {
                candidateOperations = availableCandidateOperations;
                hasReceivedCandidateOperations = true;
                return availableCandidateOperations.empty() ? 0 : availableCandidateOperations.front().Code; // CollectCandidateOperations discards the transformation if CrsKit creates one with the first operation
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
        return candidateOperations;
    }

    // Creates and returns two coordinate systems: the horizontal component of sourceWkt with its axes ordered east, north, and
    // WGS 84 (EPSG:4326) with its axes ordered latitude, longitude.
    std::pair<std::shared_ptr<CoordinateSystem>, std::shared_ptr<CoordinateSystem>> CreateEastNorthSourceAndWgs84Target(winrt::hstring const& sourceWkt)
    {
        auto const utilities = GetUtilities();
        auto const horizontalWkt = utilities->ExtractHorizontalVertical(winrt::to_string(sourceWkt)).first;
        return { GetCoordinateSystemFactory()->CreateFromWkt(utilities->GetWktWithEastNorthAxes(horizontalWkt)),
                 GetCoordinateSystemAuthorityFactory()->CreateGeographicCoordinateSystem(4326) };
    }

    // Returns {latitude, longitude} in degrees, with the longitude measured from Greenwich, in the datum of source, of the point {east, north} of source, whose
    // axes are east and north. The conversion to the base geographic system of a projected source does not change the datum.
    std::pair<double, double> GetLatitudeLongitudeDegrees(std::shared_ptr<CoordinateSystem> const& source, double const east, double const north)
    {
        std::shared_ptr<IGeographicCoordinateSystem> geographic = std::dynamic_pointer_cast<GeographicCoordinateSystem>(source);
        std::vector<double> point{ east, north };
        if (!geographic)
        {
            auto const projected = std::dynamic_pointer_cast<ProjectedCoordinateSystem>(source);
            if (!projected)
                throw winrt::hresult_error(E_INVALIDARG, L"The horizontal component of sourceWkt is neither projected nor geographic.");
            geographic = projected->GetGeographicCoordinateSystem();
            point = GetCoordinateTransformationFactory()->CreateFromCoordinateSystems(projected, std::dynamic_pointer_cast<CoordinateSystem>(geographic))
                        ->GetMathTransform()->Transform(point);
            if (AxisOrientationEnum::North == geographic->GetAxis(0).GetOrientation())
                std::swap(point[0], point[1]);
        }

        constexpr double degreesPerRadian{ 180.0 / std::numbers::pi };
        auto const degreesPerUnit = geographic->GetAngularUnit().GetRadiansPerUnit() * degreesPerRadian;
        auto const primeMeridian = geographic->GetPrimeMeridian();
        auto const primeMeridianLongitudeDegrees = primeMeridian.GetLongitude() * primeMeridian.GetAngularUnit().GetRadiansPerUnit() * degreesPerRadian;
        return { point[1] * degreesPerUnit, point[0] * degreesPerUnit + primeMeridianLongitudeDegrees };
    }

    // Creates the transformation from source to target with the operation whose EPSG code is operationCode. Returns nullptr if CrsKit
    // cannot create the transformation, for example because CrsKit does not implement the method of the operation.
    std::shared_ptr<ICoordinateTransformation> TryCreateTransformationWithOperation(std::shared_ptr<CoordinateSystem> const& source,
                                                                                    std::shared_ptr<CoordinateSystem> const& target, int const operationCode)
    {
        CoordinateTransformationOptions options{};
        options.selectOperation = [operationCode](std::string const&, std::string const&, std::vector<CoordinateOperation> const&) -> int { return operationCode; };
        try
        {
            return GetCoordinateTransformationFactory()->CreateFromCoordinateSystems(source, target, options);
        }
        catch (std::exception const&)
        {
            return nullptr;
        }
    }

    // Returns the candidate operations that have missing grid files or with which CrsKit can create a transformation. Operations with
    // missing grid files are retained without testing whether CrsKit can create their transformations.
    std::vector<CoordinateOperation> SelectCandidateOperationsWithMissingGridsOrCreatableTransformation(std::shared_ptr<CoordinateSystem> const& source, std::shared_ptr<CoordinateSystem> const& target,
                                                                     std::vector<CoordinateOperation> const& candidateOperations)
    {
        std::vector<CoordinateOperation> candidateOperationsWithMissingGridsOrCreatableTransformation;
        for (auto const& candidateOperation : candidateOperations)
            if (!::CrsKit::GetMissingGridFiles(candidateOperation).empty() || TryCreateTransformationWithOperation(source, target, candidateOperation.Code))
                candidateOperationsWithMissingGridsOrCreatableTransformation.push_back(candidateOperation);
        return candidateOperationsWithMissingGridsOrCreatableTransformation;
    }

    // Accuracy in metres used to order candidate operations; an operation without a stated accuracy goes after every operation with one.
    double GetAccuracyForOrdering(CoordinateOperation const& candidateOperation)
    {
        return candidateOperation.Accuracy.value_or(std::numeric_limits<double>::infinity());
    }

    // Considers the candidate operations whose area-of-use bounding box contains the position. Tries the operations that need grid files
    // and have none missing until CrsKit creates a transformation with one of them; if every attempt fails, tries the operations that need
    // no grid file in the same way. Within each group, tries the operations with a stated accuracy in ascending order of accuracy in
    // metres, then the operations without a stated accuracy, and returns the first transformation that CrsKit creates. Returns nullptr
    // when the area-of-use bounding box of no candidate operation contains the position, and throws if CrsKit cannot create a
    // transformation with any candidate operation whose area-of-use bounding box contains the position.
    std::shared_ptr<ICoordinateTransformation> CreateTransformationForPosition(std::shared_ptr<CoordinateSystem> const& source, std::shared_ptr<CoordinateSystem> const& target,
                                                              std::vector<CoordinateOperation> const& candidateOperations,
                                                              double const latitudeDegrees, double const longitudeDegrees)
    {
        std::vector<CoordinateOperation const*> operationsWithInstalledGrids;
        std::vector<CoordinateOperation const*> operationsWithoutGrids;
        bool hasOperationWithMissingGrids{ false };
        for (auto const& candidateOperation : candidateOperations)
        {
            if (!candidateOperation.AreaOfUseBoundingBox || !candidateOperation.AreaOfUseBoundingBox->Contains(latitudeDegrees, longitudeDegrees))
                continue;
            if (candidateOperation.GridFiles.empty())
                operationsWithoutGrids.push_back(&candidateOperation);
            else if (::CrsKit::GetMissingGridFiles(candidateOperation).empty())
                operationsWithInstalledGrids.push_back(&candidateOperation);
            else
                hasOperationWithMissingGrids = true;
        }

        if (operationsWithInstalledGrids.empty() && operationsWithoutGrids.empty() && !hasOperationWithMissingGrids)
            return nullptr;

        auto const isMoreAccurate = [](CoordinateOperation const* first, CoordinateOperation const* second)
        {
            return GetAccuracyForOrdering(*first) < GetAccuracyForOrdering(*second);
        };
        std::ranges::stable_sort(operationsWithInstalledGrids, isMoreAccurate);
        std::ranges::stable_sort(operationsWithoutGrids, isMoreAccurate);
        for (auto const* operations : { &operationsWithInstalledGrids, &operationsWithoutGrids })
            for (auto const* candidateOperation : *operations)
                if (auto transformation = TryCreateTransformationWithOperation(source, target, candidateOperation->Code))
                    return transformation;

        throw std::runtime_error(std::format(
            "CrsKit cannot create a transformation to WGS 84 with any candidate datum shift whose area-of-use bounding box contains latitude {} and longitude {} degrees.",
            latitudeDegrees, longitudeDegrees));
    }

    // Returns true if wkt is one element KEYWORD[...] whose brackets and quotes are balanced and that has nothing after it. The keyword
    // may contain letters, underscores, or both, as in COMPD_CS, VERT_CS and LOCAL_CS.
    // crskit's parser can fail with an access violation on some truncated texts, such as "]" or a PROJCS cut inside AXIS.
    bool IsCompleteWktElement(std::wstring_view const wkt)
    {
        size_t wktCharacterIndex{};
        while (wktCharacterIndex < wkt.size() && (iswalpha(wkt[wktCharacterIndex]) || L'_' == wkt[wktCharacterIndex]))
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
        // ModifyWithAxisEastNorth and ModifyWithAxisNorthEast read the second axis without checking the dimension, so a
        // system with fewer than two axes, such as a vertical system, keeps its catalogue definition.
        if (coordinateSystem->GetDimension() < 2)
            return coordinateSystem;

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

            auto result = winrt::single_threaded_vector<winrt::Digi21::CrsKit::Interop::CoordinateOperationInfo>();
            for (auto const& candidateOperation : CollectCandidateOperations(source, target))
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
            // ExtractHorizontalVertical returns the Well-Known Text (WKT) of an unknown vertical system when it receives a WKT
            // without a vertical component. For a parsed system without a vertical component, ExtractHorizontalVertical returns
            // null for the vertical component, and GetVerticalWkt converts that null to an empty string.
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

    bool CrsEngine::IsVerticalComponentLocal(hstring const& wkt)
    {
        EnsureInitialized();
        try
        {
            auto const vertical = GetUtilities()->ExtractHorizontalVertical(CreateCoordinateSystemFromWkt(wkt)).second;
            return nullptr != std::dynamic_pointer_cast<LocalCoordinateSystem>(vertical);
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

    winrt::Digi21::CrsKit::Interop::AxisOrientation CrsEngine::GetFirstAxisOrientation(hstring const& wkt)
    {
        EnsureInitialized();
        try
        {
            // The values of AxisOrientation are those of crskit's AxisOrientationEnum.
            return static_cast<winrt::Digi21::CrsKit::Interop::AxisOrientation>(CreateCoordinateSystemFromWkt(wkt)->GetAxis(0).GetOrientation());
        }
        catch (winrt::hresult_error const&) { throw; }
        catch (std::exception const& e) { ThrowWinrtError(e); }
    }

    winrt::Digi21::CrsKit::Interop::Transformation CrsEngine::CreateEastNorthToWgs84LatitudeLongitude(hstring const& sourceWkt, double eastInSourceUnits, double northInSourceUnits)
    {
        EnsureInitialized();
        try
        {
            // The transformation expects {easting, northing} or {longitude, latitude}, whatever axis order the source CRS declares.
            // EPSG:4326 declares the latitude first, so Transform returns {latitude, longitude} in degrees.
            auto const [source, target] = CreateEastNorthSourceAndWgs84Target(sourceWkt);
            auto const [latitudeDegrees, longitudeDegrees] = GetLatitudeLongitudeDegrees(source, eastInSourceUnits, northInSourceUnits);

            std::shared_ptr<ICoordinateTransformation> coordinateTransformation;
            auto const candidateOperations = CollectCandidateOperations(source, target);
            if (candidateOperations.empty())
            {
                // CrsKit invokes no selectOperation callback when at most one operation applies.
                coordinateTransformation = GetCoordinateTransformationFactory()->CreateFromCoordinateSystems(source, target);
            }
            else
            {
                coordinateTransformation = CreateTransformationForPosition(source, target, candidateOperations, latitudeDegrees, longitudeDegrees);
                if (!coordinateTransformation)
                    return nullptr;
            }
            if (!coordinateTransformation)
                throw winrt::hresult_error(E_FAIL, L"No coordinate operation converts the horizontal component of sourceWkt to WGS 84.");
            return winrt::make<implementation::Transformation>(coordinateTransformation);
        }
        catch (winrt::hresult_error const&) { throw; }
        catch (std::exception const& e) { ThrowWinrtError(e); }
    }

    winrt::Windows::Foundation::Collections::IVectorView<winrt::Digi21::CrsKit::Interop::CoordinateOperationInfo>
        CrsEngine::GetEastNorthToWgs84CandidateOperations(hstring const& sourceWkt)
    {
        EnsureInitialized();
        try
        {
            auto const [source, target] = CreateEastNorthSourceAndWgs84Target(sourceWkt);
            auto result = winrt::single_threaded_vector<winrt::Digi21::CrsKit::Interop::CoordinateOperationInfo>();
            for (auto const& candidateOperation : SelectCandidateOperationsWithMissingGridsOrCreatableTransformation(source, target, CollectCandidateOperations(source, target)))
                result.Append(MakeOperationInfo(candidateOperation));
            return result.GetView();
        }
        catch (winrt::hresult_error const&) { throw; }
        catch (std::exception const& e) { ThrowWinrtError(e); }
    }

    winrt::Windows::Foundation::Collections::IVectorView<winrt::Digi21::CrsKit::Interop::CoordinateOperationInfo>
        CrsEngine::GetEastNorthToWgs84CandidateOperationsAtPosition(hstring const& sourceWkt, double eastInSourceUnits, double northInSourceUnits)
    {
        EnsureInitialized();
        try
        {
            auto const [source, target] = CreateEastNorthSourceAndWgs84Target(sourceWkt);
            auto const [latitudeDegrees, longitudeDegrees] = GetLatitudeLongitudeDegrees(source, eastInSourceUnits, northInSourceUnits);
            auto result = winrt::single_threaded_vector<winrt::Digi21::CrsKit::Interop::CoordinateOperationInfo>();
            for (auto const& candidateOperation : SelectCandidateOperationsWithMissingGridsOrCreatableTransformation(source, target, CollectCandidateOperations(source, target)))
                if (candidateOperation.AreaOfUseBoundingBox && candidateOperation.AreaOfUseBoundingBox->Contains(latitudeDegrees, longitudeDegrees))
                    result.Append(MakeOperationInfo(candidateOperation));
            return result.GetView();
        }
        catch (winrt::hresult_error const&) { throw; }
        catch (std::exception const& e) { ThrowWinrtError(e); }
    }

    winrt::Digi21::CrsKit::Interop::Transformation CrsEngine::CreateEastNorthToWgs84LatitudeLongitudeWithOperation(hstring const& sourceWkt, int32_t operationCode)
    {
        EnsureInitialized();
        try
        {
            auto const [source, target] = CreateEastNorthSourceAndWgs84Target(sourceWkt);
            CoordinateTransformationOptions options{};
            options.selectOperation =
                [operationCode](std::string const&, std::string const&, std::vector<CoordinateOperation> const&) -> int { return operationCode; };

            auto const coordinateTransformation = GetCoordinateTransformationFactory()->CreateFromCoordinateSystems(source, target, options);
            if (!coordinateTransformation)
                throw winrt::hresult_error(E_FAIL, L"No coordinate operation converts the horizontal component of sourceWkt to WGS 84.");
            return winrt::make<implementation::Transformation>(coordinateTransformation);
        }
        catch (winrt::hresult_error const&) { throw; }
        catch (std::exception const& e) { ThrowWinrtError(e); }
    }
}
