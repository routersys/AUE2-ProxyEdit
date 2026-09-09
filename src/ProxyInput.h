#pragma once

#include <windows.h>

#include "plugin2.h"

namespace pe {

INPUT_PLUGIN_TABLE* ProxyInputTable();
void ShutdownProxyInput();

}
