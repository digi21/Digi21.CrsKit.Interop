#pragma once
#include <unknwn.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>

#include <atomic>
#include <cwctype>
#include <format>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>

// CrsKit provides native coordinate transformations through the vcpkg port 'crskit' and uses C++ standard library types.
// This component converts CrsKit data to WinRT types before returning the data to callers.
#include <CrsKit.h>
#include <SqliteProvider.h>
