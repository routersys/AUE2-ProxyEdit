#pragma once

#include <windows.h>

#include "plugin2.h"

namespace pe {

void RegisterStatusPanel(HOST_APP_TABLE* host);
void DestroyStatusPanel();

}
