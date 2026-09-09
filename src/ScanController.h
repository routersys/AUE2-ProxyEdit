#pragma once

#include <windows.h>

#include <string>
#include <vector>

#include "plugin2.h"

namespace pe {

struct ScanResult {
    int examined = 0;
    int eligible = 0;
    int swapped = 0;
    int restored = 0;
    int rejected = 0;
};

void RegisterScanMenus(HOST_APP_TABLE* host);
ScanResult ApplyProxies();
ScanResult RestoreOriginals();
bool IsProxyPath(const std::wstring& path);
std::wstring SourceOfProxy(const std::wstring& path);
void RequestAutomaticScan();
void StartScanController();
void StopScanController();

}
