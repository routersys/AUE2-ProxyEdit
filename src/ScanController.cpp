#include "ScanController.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <map>
#include <mutex>
#include <chrono>
#include <string>
#include <thread>

#include "HostContext.h"
#include "Log.h"
#include "ProxyBuilder.h"
#include "ProxyFormat.h"
#include "ProxyStore.h"
#include "Settings.h"

namespace pe {

namespace {

const wchar_t* kEffect = L"動画ファイル";
const wchar_t* kItem = L"ファイル";
std::atomic<bool> g_scanning{false};
std::atomic<unsigned long long> g_last_scan{0};
std::atomic<bool> g_requested{false};
std::atomic<bool> g_stop{false};
std::thread g_worker;
std::mutex g_wake_lock;
std::condition_variable g_wake;

std::string ToUtf8(const std::wstring& text) {
    if (text.empty()) return std::string();
    int length = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), nullptr, 0, nullptr,
                                     nullptr);
    std::string result((size_t)length, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), result.data(), length, nullptr,
                        nullptr);
    return result;
}

std::wstring FromUtf8(const char* text) {
    if (!text) return std::wstring();
    int length = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
    if (length <= 1) return std::wstring();
    std::wstring result((size_t)(length - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text, -1, result.data(), length);
    return result;
}

struct Collected {
    int layers = 1;
    std::vector<OBJECT_HANDLE> objects;
    std::vector<std::wstring> files;
};

void CollectObjects(void* param, EDIT_SECTION* edit) {
    Collected* collected = (Collected*)param;
    for (int layer = 0; layer < collected->layers; layer++) {
        int frame = 0;
        for (int guard = 0; guard < 20000; guard++) {
            OBJECT_HANDLE object = edit->find_object(layer, frame);
            if (!object) break;
            OBJECT_LAYER_FRAME position = edit->get_object_layer_frame(object);
            if (edit->count_object_effect(object, kEffect) > 0) {
                LPCSTR value = edit->get_object_item_value(object, kEffect, kItem);
                std::wstring path = FromUtf8(value);
                if (!path.empty()) {
                    collected->objects.push_back(object);
                    collected->files.push_back(path);
                }
            }
            if (position.end < frame) break;
            frame = position.end + 1;
        }
    }
}

struct SwapRequest {
    std::vector<OBJECT_HANDLE> objects;
    std::vector<std::wstring> files;
    int applied = 0;
};

void ApplySwap(void* param, EDIT_SECTION* edit) {
    SwapRequest* request = (SwapRequest*)param;
    for (size_t index = 0; index < request->objects.size(); index++) {
        std::string encoded = ToUtf8(request->files[index]);
        if (edit->set_object_item_value(request->objects[index], kEffect, kItem, encoded.c_str())) {
            request->applied++;
        }
    }
    if (request->applied > 0) edit->set_edited_state();
}

struct MediaQuery {
    std::wstring path;
    MEDIA_INFO info{};
    bool ok = false;
};

void QueryMedia(void* param, EDIT_SECTION* edit) {
    MediaQuery* query = (MediaQuery*)param;
    query->ok = edit->get_media_info(query->path.c_str(), &query->info, sizeof(query->info));
}

bool Eligible(const std::wstring& path, const MEDIA_INFO& info) {
    const Settings& settings = CurrentSettings();
    if (info.video_track_num <= 0) return false;
    if (info.width >= settings.target_min_width) return true;
    if (settings.target_min_mbps > 0 && info.total_time > 0.5) {
        SourceKey key;
        if (QuerySource(path, key)) {
            double mbps = (double)key.size * 8.0 / info.total_time / 1000000.0;
            if (mbps >= settings.target_min_mbps) return true;
        }
    }
    return false;
}

void ScanWorker() {
    while (!g_stop.load()) {
        {
            std::unique_lock<std::mutex> lock(g_wake_lock);
            g_wake.wait_for(lock, std::chrono::milliseconds(400));
        }
        if (g_stop.load()) return;
        if (!g_requested.exchange(false)) continue;
        if (!CurrentSettings().enabled) continue;
        ScanResult result = ApplyProxies();
        g_last_scan.store(GetTickCount64());
        if (result.swapped > 0) {
            Say(L"自動でプロキシへ差し替えました: %d 件", result.swapped);
        }
    }
}

void OnHostEvent(void*) {
    RequestAutomaticScan();
}

void OnEditMenuApply(EDIT_SECTION*) {
    ScanResult result = ApplyProxies();
    Say(L"プロキシへ差し替えました: 対象 %d 件、差し替え %d 件", result.eligible, result.swapped);
}

void OnEditMenuRestore(EDIT_SECTION*) {
    ScanResult result = RestoreOriginals();
    Say(L"元素材へ戻しました: %d 件", result.restored);
}

}

bool IsProxyPath(const std::wstring& path) {
    if (path.size() < 4) return false;
    return _wcsicmp(path.c_str() + path.size() - 4, L".pxy") == 0;
}

std::wstring SourceOfProxy(const std::wstring& path) {
    ProxyReader reader;
    if (!reader.Open(path)) return std::wstring();
    std::wstring source = reader.Header().source_path;
    reader.Close();
    return source;
}

ScanResult ApplyProxies() {
    ScanResult result;
    if (g_scanning.exchange(true)) return result;
    Collected collected;
    collected.layers = std::max(EditInfo().layer_max + 1, 1);
    CallReadSection(&collected, CollectObjects);

    SwapRequest request;
    std::map<std::wstring, std::wstring> resolved;
    for (size_t index = 0; index < collected.objects.size(); index++) {
        const std::wstring& path = collected.files[index];
        result.examined++;
        if (IsProxyPath(path)) continue;

        auto cached = resolved.find(path);
        if (cached == resolved.end()) {
            MediaQuery query;
            query.path = path;
            CallReadSection(&query, QueryMedia);
            if (!query.ok || !Eligible(path, query.info)) {
                resolved[path] = std::wstring();
                result.rejected++;
                continue;
            }
            std::wstring proxy;
            if (!RegisterSource(path, proxy)) {
                resolved[path] = std::wstring();
                result.rejected++;
                continue;
            }
            resolved[path] = proxy;
            cached = resolved.find(path);
        }
        if (cached->second.empty()) continue;
        result.eligible++;
        request.objects.push_back(collected.objects[index]);
        request.files.push_back(cached->second);
    }

    if (!request.objects.empty()) {
        CallEditSection(&request, ApplySwap);
        result.swapped = request.applied;
    }
    g_scanning.store(false);
    return result;
}

ScanResult RestoreOriginals() {
    ScanResult result;
    Collected collected;
    collected.layers = std::max(EditInfo().layer_max + 1, 1);
    CallReadSection(&collected, CollectObjects);

    SwapRequest request;
    std::map<std::wstring, std::wstring> resolved;
    for (size_t index = 0; index < collected.objects.size(); index++) {
        const std::wstring& path = collected.files[index];
        if (!IsProxyPath(path)) continue;
        auto cached = resolved.find(path);
        if (cached == resolved.end()) {
            resolved[path] = SourceOfProxy(path);
            cached = resolved.find(path);
        }
        if (cached->second.empty()) continue;
        request.objects.push_back(collected.objects[index]);
        request.files.push_back(cached->second);
    }
    if (!request.objects.empty()) {
        CallEditSection(&request, ApplySwap);
        result.restored = request.applied;
    }
    return result;
}

void RequestAutomaticScan() {
    if (GetTickCount64() - g_last_scan.load() < 1500) return;
    g_requested.store(true);
    g_wake.notify_all();
}

void StartScanController() {
    if (g_worker.joinable()) return;
    g_stop.store(false);
    g_worker = std::thread(ScanWorker);
}

void StopScanController() {
    g_stop.store(true);
    g_wake.notify_all();
    if (g_worker.joinable()) g_worker.join();
}

void RegisterScanMenus(HOST_APP_TABLE* host) {
    host->register_edit_menu(L"プロキシへ差し替え", OnEditMenuApply);
    host->register_edit_menu(L"プロキシから元素材へ戻す", OnEditMenuRestore);
    host->register_event_listener(EVENT_TYPE::UPDATE_OBJECT, nullptr, OnHostEvent);
    host->register_event_listener(EVENT_TYPE::CHANGE_EDIT_SCENE, nullptr, OnHostEvent);
}

}
