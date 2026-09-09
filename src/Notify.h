#pragma once

#include <windows.h>

namespace pe {

void AddStateListener(HWND window, UINT message);
void RemoveStateListener(HWND window);
void AcknowledgeStateChange(HWND window);
void PublishStateChange();

}
