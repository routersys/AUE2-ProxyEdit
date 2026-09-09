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
    int unsupported = 0;
};

struct Unsupported {
    std::wstring source;
    std::wstring reason;
};

void RegisterScanMenus(HOST_APP_TABLE* host);
void RequestApply();
void RequestRestore();

int RestoreForExport();
void RequestRestoreProxy(const std::wstring& proxy);
int RestoreProxiesNow(const std::vector<std::wstring>& proxies);
std::vector<Unsupported> UnsupportedSources();
void ApplyAfterExport();
void SuspendAutomaticScan(bool suspend);
bool AutomaticScanSuspended();
bool IsProxyPath(const std::wstring& path);
std::wstring SourceOfProxy(const std::wstring& path);
void StartScanController();
void StopScanController();

}
