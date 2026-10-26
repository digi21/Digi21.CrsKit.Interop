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
    winrt::Digi21::CrsKit::Interop::CoordinateOperationInfo MakeOperationInfo(CoordinateOperation const& op)
    {
        winrt::Windows::Foundation::IReference<double> accuracy{ nullptr };
        if (op.Accuracy.has_value())
            accuracy = winrt::box_value(op.Accuracy.value())
                           .as<winrt::Windows::Foundation::IReference<double>>();

        std::string gridFiles;
        for (auto const& file : op.GridFiles)
        {
            if (!gridFiles.empty()) gridFiles += ", ";
            gridFiles += file;
        }

        return winrt::make<winrt::Digi21::CrsKit::Interop::implementation::CoordinateOperationInfo>(
            op.Code,
            winrt::to_hstring(op.Name),
            winrt::to_hstring(op.Type),
            accuracy,
            winrt::to_hstring(op.Scope.value_or("")),
            winrt::to_hstring(op.Remarks.value_or("")),
            op.MethodCode,
            winrt::to_hstring(op.AreaOfUse),
            winrt::to_hstring(gridFiles));
    }

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
            auto const map = GetCoordinateSystemAuthorityFactory()->EnumerateCoordinateSystems(winrt::to_string(kind));
            auto result = winrt::single_threaded_vector<winrt::Digi21::CrsKit::Interop::CrsInfo>();
            for (auto const& [code, name] : map)
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
            hstring const area = getStringOrEmpty([&] { return coordinateSystemAuthorityFactory->GetDescriptionAreaApplicationCrs(code); });

            int32_t axisCount{};
            try
            {
                axisCount = static_cast<int32_t>(
                    coordinateSystemAuthorityFactory->GetAxisOfCoordinateSystem(coordinateSystemAuthorityFactory->GetCodeOfCoordinateSystemAssociatedWithCrs(code)).size());
            }
            catch (...) {}

            hstring datumName, datumOrigin, meridian, ellipsoid;
            try
            {
                int const datumCode = coordinateSystemAuthorityFactory->GetCodeOfDatumAssociatedWithCrs(code);
                datumName = getStringOrEmpty([&] { return coordinateSystemAuthorityFactory->GetNameOfDatum(datumCode); });
                datumOrigin = getStringOrEmpty([&] { return coordinateSystemAuthorityFactory->GetOriginDescriptionOfDatum(datumCode); });
                ellipsoid = getStringOrEmpty([&] { return coordinateSystemAuthorityFactory->GetNameOfEllipsoid(coordinateSystemAuthorityFactory->GetCodeOfEllipsoidAssociatedWithDatum(datumCode)); });
            }
            catch (...) {}
            meridian = getStringOrEmpty([&] { return coordinateSystemAuthorityFactory->GetNameOfPrimeMeridian(coordinateSystemAuthorityFactory->GetCodeOfPrimeMeridianAssociatedWithCrs(code)); });

            return winrt::make<implementation::CrsDetails>(
                code, name, kind, axisCount, area, datumName, datumOrigin, meridian, ellipsoid);
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

    hstring CrsEngine::GetStandardAxisLabel(int32_t code)
    {
        EnsureInitialized();
        try
        {
            auto const coordinateSystem = MakeCoordinateSystem(code);
            // The horizontal order is defined by the first two axes; use their real
            // catalogue names (EPSG abbreviations) rather than a hard-coded mapping.
            int const dimension = coordinateSystem->GetDimension();
            int const count = dimension < 2 ? dimension : 2;
            std::string label;
            for (int axisIndex = 0; axisIndex < count; ++axisIndex)
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

            auto unitName = [](AnyUnit const& u) -> std::string
            {
                return std::visit([](auto const& x) { return x.GetName(); }, u);
            };

            std::string result;
            int const dimension = coordinateSystem->GetDimension();
            for (int axisIndex = 0; axisIndex < dimension; ++axisIndex)
            {
                std::string axisName, unit;
                try { axisName = coordinateSystem->GetAxis(axisIndex).GetName(); } catch (...) {}
                try { unit = unitName(coordinateSystem->GetUnits(axisIndex)); } catch (...) {}
                if (axisName.empty()) axisName = std::format("Axis {}", axisIndex + 1);

                if (!result.empty()) result += ", ";
                result += axisName;
                if (!unit.empty()) result += std::format(" ({})", unit);
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
            auto const ct = GetCoordinateTransformationFactory()->CreateFromCoordinateSystems(source, target);
            return winrt::make<implementation::Transformation>(ct);
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
            auto const ct = GetCoordinateTransformationFactory()->CreateFromCoordinateSystems(source, target);
            return winrt::make<implementation::Transformation>(ct);
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
                                         std::vector<CoordinateOperation> const& ops) -> int
                {
                    candidateOperations = ops;
                    hasReceivedCandidateOperations = true;
                    return ops.empty() ? 0 : ops.front().Code; // CrsKit builds the discarded transformation with the first operation
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
            for (auto const& op : candidateOperations)
                result.Append(MakeOperationInfo(op));
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

            auto const ct = GetCoordinateTransformationFactory()->CreateFromCoordinateSystems(source, target, options);
            return winrt::make<implementation::Transformation>(ct);
        }
        catch (winrt::hresult_error const&) { throw; }
        catch (std::exception const& e) { ThrowWinrtError(e); }
    }
}
