#include "Log.h"

#include <stdio.h>
#include <stdarg.h>

namespace pe {

namespace {

LOG_HANDLE* g_log = nullptr;

#ifdef PE_DEBUG_LOG
void WriteFileLog(const wchar_t* text) {
    wchar_t path[MAX_PATH];
    if (GetTempPathW(MAX_PATH, path) == 0) return;
    wcsncat_s(path, L"ProxyEdit.log", _TRUNCATE);
    FILE* file = nullptr;
    if (_wfopen_s(&file, path, L"a+, ccs=UTF-8") != 0 || !file) return;
    SYSTEMTIME now;
    GetLocalTime(&now);
    fwprintf(file, L"%02d:%02d:%02d.%03d [%lu] %s\n", now.wHour, now.wMinute, now.wSecond,
             now.wMilliseconds, GetCurrentThreadId(), text);
    fclose(file);
}
#endif

void Emit(const wchar_t* text, bool warning) {
    if (g_log) {
        if (warning) g_log->warn(g_log, text);
        else g_log->log(g_log, text);
    }
#ifdef PE_DEBUG_LOG
    WriteFileLog(text);
#else
    (void)text;
#endif
}

void Format(const wchar_t* format, va_list args, bool warning) {
    wchar_t buffer[1024];
    _vsnwprintf_s(buffer, _TRUNCATE, format, args);
    Emit(buffer, warning);
}

}

void SetLogHandle(LOG_HANDLE* handle) {
    g_log = handle;
}

void Say(const wchar_t* format, ...) {
    va_list args;
    va_start(args, format);
    Format(format, args, false);
    va_end(args);
}

void Warn(const wchar_t* format, ...) {
    va_list args;
    va_start(args, format);
    Format(format, args, true);
    va_end(args);
}

}
