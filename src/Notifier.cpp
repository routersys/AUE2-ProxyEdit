#include "Notifier.h"

#include <objbase.h>
#include <shobjidl.h>

#include <atomic>

#include "HostContext.h"
#include "Log.h"
#include "Notify.h"
#include "ProxyBuilder.h"

namespace pe {

namespace {

const wchar_t* kClassName = L"ProxyEditNotifierSink";
const UINT kStateMessage = WM_APP + 1;

ITaskbarList3* g_taskbar = nullptr;
HWND g_window = nullptr;
HWND g_sink = nullptr;
bool g_showing = false;
std::atomic<bool> g_finished_reported{true};

ITaskbarList3* Taskbar() {
    if (!g_taskbar) {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (FAILED(CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&g_taskbar)))) {
            g_taskbar = nullptr;
        } else if (FAILED(g_taskbar->HrInit())) {
            g_taskbar->Release();
            g_taskbar = nullptr;
        }
    }
    return g_taskbar;
}

void Update() {
    HWND host = HostWindow();
    if (!host) return;
    BuilderSummary summary = BuilderState();
    ITaskbarList3* taskbar = Taskbar();
    if (!taskbar) return;

    const bool busy = summary.jobs > 0 && summary.queued_chunks > 0 && !summary.paused;
    if (busy) {
        taskbar->SetProgressState(host, TBPF_NORMAL);
        ULONGLONG value = (ULONGLONG)(summary.overall * 1000.0);
        taskbar->SetProgressValue(host, value, 1000);
        g_showing = true;
        g_finished_reported.store(false);
        return;
    }
    if (summary.paused && summary.jobs > 0 && summary.queued_chunks > 0) {
        taskbar->SetProgressState(host, TBPF_PAUSED);
        taskbar->SetProgressValue(host, (ULONGLONG)(summary.overall * 1000.0), 1000);
        g_showing = true;
        return;
    }
    if (g_showing) {
        taskbar->SetProgressState(host, TBPF_NOPROGRESS);
        g_showing = false;
    }
    if (!g_finished_reported.exchange(true) && summary.jobs > 0) {
        Say(L"プロキシの生成が終わりました");
    }
}

LRESULT CALLBACK SinkProc(HWND window, UINT message, WPARAM first, LPARAM second) {
    if (message == kStateMessage) {
        AcknowledgeStateChange(window);
        Update();
        return 0;
    }
    return DefWindowProcW(window, message, first, second);
}

}

void StartNotifier() {
    g_window = HostWindow();
    if (!g_window) return;
    WNDCLASSEXW description{};
    description.cbSize = sizeof(description);
    description.lpfnWndProc = SinkProc;
    description.hInstance = ModuleInstance();
    description.lpszClassName = kClassName;
    RegisterClassExW(&description);
    g_sink = CreateWindowExW(0, kClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                             ModuleInstance(), nullptr);
    if (g_sink) AddStateListener(g_sink, kStateMessage);
}

void StopNotifier() {
    if (g_sink) {
        RemoveStateListener(g_sink);
        DestroyWindow(g_sink);
        g_sink = nullptr;
    }
    if (g_window) {
        if (g_taskbar && g_showing) g_taskbar->SetProgressState(g_window, TBPF_NOPROGRESS);
        g_window = nullptr;
    }
    if (g_taskbar) {
        g_taskbar->Release();
        g_taskbar = nullptr;
    }
}

void RefreshNotifier() {
    Update();
}

}
