#pragma once

#include <windows.h>

namespace pe {

void StartExportGuard();
void StopExportGuard();

void NoticeEditActivity();
void ExportRestoreFinished(bool restored);

}
