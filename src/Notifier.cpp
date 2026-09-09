#include "Notifier.h"

#include <objbase.h>
#include <shobjidl.h>

#include <atomic>

#include "HostContext.h"
#include "Log.h"
#include "ProxyBuilder.h"

namespace pe {

namespace {

const UINT_PTR kTimerId = 0x50450001;
const UINT kTimerInterval = 700;

ITaskbarList3* g_taskbar = nullptr;
HWND g_window = nullptr;
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
        Say(L"プロキシの生成が一巡しました");
    }
}

void CALLBACK OnTimer(HWND, UINT, UINT_PTR, DWORD) {
    Update();
}

}

void StartNotifier() {
    g_window = HostWindow();
    if (!g_window) return;
    SetTimer(g_window, kTimerId, kTimerInterval, OnTimer);
}

void StopNotifier() {
    if (g_window) {
        KillTimer(g_window, kTimerId);
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
