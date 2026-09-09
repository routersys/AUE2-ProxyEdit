#pragma once

#include <windows.h>

#include "logger2.h"

namespace pe {

void SetLogHandle(LOG_HANDLE* handle);
void Say(const wchar_t* format, ...);
void Warn(const wchar_t* format, ...);

}
