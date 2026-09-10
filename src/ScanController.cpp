#include "ScanController.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <map>
#include <set>
#include <mutex>
#include <string>
#include <thread>

#include "HostContext.h"
#include "Log.h"
#include "ExportGuard.h"
#include "ProxyStore.h"
#include "Notify.h"
#include "ProxyBuilder.h"
#include "ProxyFormat.h"
#include "ProxyStore.h"
#include "Settings.h"

namespace pe {

namespace {

const wchar_t* kEffect = L"動画ファイル";
const wchar_t* kItem = L"ファイル";
std::atomic<bool> g_started{false};
std::atomic<bool> g_suspended{false};
std::atomic<int> g_pending{0};
std::atomic<bool> g_stop{false};
std::thread g_worker;
std::mutex g_wake_lock;
std::condition_variable g_wake;

enum RequestKind {
    kRequestAutomatic = 1,
    kRequestApply = 2,
    kRequestRestore = 4,
    kRequestRestoreFailed = 8,
    kRequestExportRestore = 16,
    kRequestDelete = 32,
};

std::mutex g_report_lock;
std::vector<std::wstring> g_failed_proxies;
std::vector<std::wstring> g_delete_list;
std::vector<Unsupported> g_unsupported;

void Post(int kind) {
    {
        std::lock_guard<std::mutex> lock(g_wake_lock);
        g_pending.fetch_or(kind);
    }
    g_wake.notify_all();
}

struct Decision {
    std::wstring proxy;
    long long size = 0;
    long long time = 0;
};
std::map<std::wstring, Decision> g_decisions;

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
    size_t recoveries = 0;
    int applied = 0;
    int recovered = 0;
};

void ApplySwap(void* param, EDIT_SECTION* edit) {
    SwapRequest* request = (SwapRequest*)param;
    for (size_t index = 0; index < request->objects.size(); index++) {
        std::string encoded = ToUtf8(request->files[index]);
        if (!edit->set_object_item_value(request->objects[index], kEffect, kItem, encoded.c_str())) {
            continue;
        }
        if (index < request->recoveries) request->recovered++;
        else request->applied++;
    }
    if (request->applied > 0 || request->recovered > 0) edit->set_edited_state();
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

struct ProxyState {
    std::wstring source;
    bool present = false;
};

bool ProxyExists(const std::wstring& path) {
    if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) return true;
    const DWORD reason = GetLastError();
    return reason != ERROR_FILE_NOT_FOUND && reason != ERROR_PATH_NOT_FOUND;
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

ScanResult ApplyProxies() {
    ScanResult result;
    Collected collected;
    collected.layers = std::max(EditInfo().layer_max + 1, 1);
    CallReadSection(&collected, CollectObjects);

    SwapRequest request;
    SwapRequest recovery;
    std::map<std::wstring, std::wstring> resolved;
    std::map<std::wstring, ProxyState> proxies;
    std::vector<Unsupported> unsupported;
    for (size_t index = 0; index < collected.objects.size(); index++) {
        const std::wstring& path = collected.files[index];
        result.examined++;
        if (IsProxyPath(path)) {
            auto known = proxies.find(path);
            if (known == proxies.end()) {
                ProxyState state;
                state.present = ProxyExists(path);
                state.source = SourceOfProxy(path);
                if (state.present && !state.source.empty()) {
                    std::wstring proxy;
                    RegisterSource(state.source, proxy);
                }
                known = proxies.emplace(path, state).first;
            }
            if (!known->second.present) {
                if (known->second.source.empty()) {
                    result.rejected++;
                    continue;
                }
                recovery.objects.push_back(collected.objects[index]);
                recovery.files.push_back(known->second.source);
                continue;
            }
            result.eligible++;
            continue;
        }

        auto cached = resolved.find(path);
        if (cached == resolved.end()) {
            SourceKey key;
            const bool present = QuerySource(path, key);
            auto remembered = g_decisions.find(path);
            if (present && remembered != g_decisions.end() &&
                remembered->second.size == key.size && remembered->second.time == key.time) {
                std::wstring proxy = remembered->second.proxy;
                if (!proxy.empty() && !RegisterSource(path, proxy)) {
                    proxy.clear();
                    if (!SourceFailed(path)) {
                        unsupported.push_back({path, L"プロキシの生成に失敗しました"});
                    }
                }
                resolved[path] = proxy;
                cached = resolved.find(path);
            } else {
                MediaQuery query;
                query.path = path;
                CallReadSection(&query, QueryMedia);
                std::wstring proxy;
                if (!query.ok || !Eligible(path, query.info)) {
                    proxy.clear();
                } else if (!RegisterSource(path, proxy)) {
                    proxy.clear();
                    unsupported.push_back({path, L"この形式は読み込めないためプロキシを作れません"});
                }
                resolved[path] = proxy;
                Decision decision;
                decision.proxy = proxy;
                decision.size = key.size;
                decision.time = key.time;
                if (present) g_decisions[path] = decision;
                cached = resolved.find(path);
            }
            if (cached->second.empty()) {
                result.rejected++;
                continue;
            }

        }
        if (cached->second.empty()) continue;
        result.eligible++;
        request.objects.push_back(collected.objects[index]);
        request.files.push_back(cached->second);
    }

    if (!recovery.objects.empty()) {
        request.recoveries = recovery.objects.size();
        request.objects.insert(request.objects.begin(), recovery.objects.begin(),
                               recovery.objects.end());
        request.files.insert(request.files.begin(), recovery.files.begin(), recovery.files.end());
    }
    if (!request.objects.empty()) {
        CallEditSection(&request, ApplySwap);
        result.swapped = request.applied;
        result.restored = request.recovered;
    }
    result.unsupported = (int)unsupported.size();
    {
        std::lock_guard<std::mutex> lock(g_report_lock);
        g_unsupported = unsupported;
    }
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

ScanResult RestoreFailed(const std::vector<std::wstring>& proxies) {
    ScanResult result;
    if (proxies.empty()) return result;
    Collected collected;
    collected.layers = std::max(EditInfo().layer_max + 1, 1);
    CallReadSection(&collected, CollectObjects);

    SwapRequest request;
    std::map<std::wstring, std::wstring> resolved;
    for (size_t index = 0; index < collected.objects.size(); index++) {
        const std::wstring& path = collected.files[index];
        if (!IsProxyPath(path)) continue;
        bool wanted = false;
        for (const std::wstring& proxy : proxies) {
            if (_wcsicmp(proxy.c_str(), path.c_str()) == 0) {
                wanted = true;
                break;
            }
        }
        if (!wanted) continue;
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

void ScanWorker() {
    while (true) {
        int taken = 0;
        {
            std::unique_lock<std::mutex> lock(g_wake_lock);
            g_wake.wait(lock, [] {
                return g_stop.load() || (g_started.load() && g_pending.load() != 0);
            });
            if (g_stop.load()) return;
            taken = g_pending.exchange(0);
        }
        if (taken & kRequestExportRestore) {
            ScanResult result = RestoreOriginals();
            if (result.restored > 0) {
                Say(L"一時的に元素材へ戻しました: %d 件", result.restored);
            }
            PublishStateChange();
            ExportRestoreFinished(result.restored > 0);
        }
        if (taken & kRequestDelete) {
            std::vector<std::wstring> proxies;
            {
                std::lock_guard<std::mutex> lock(g_report_lock);
                proxies.swap(g_delete_list);
            }
            RestoreFailed(proxies);
            for (const std::wstring& proxy : proxies) {
                std::wstring source = SourceOfProxy(proxy);
                if (!source.empty()) DiscardSource(source);
                RemoveProxy(proxy);
            }
            if (!proxies.empty()) Say(L"プロキシを削除しました: %d 件", (int)proxies.size());
            PublishStateChange();
        }
        if (taken & kRequestRestoreFailed) {
            std::vector<std::wstring> proxies;
            {
                std::lock_guard<std::mutex> lock(g_report_lock);
                proxies.swap(g_failed_proxies);
            }
            ScanResult result = RestoreFailed(proxies);
            if (result.restored > 0) {
                Say(L"生成に失敗したので元素材へ戻しました: %d 件", result.restored);
            }
            PublishStateChange();
        }
        if (taken & kRequestRestore) {
            ScanResult result = RestoreOriginals();
            Say(L"元素材へ戻しました: %d 件", result.restored);
            PublishStateChange();
        }
        if (taken & kRequestApply) {
            ScanResult result = ApplyProxies();
            Say(L"プロキシへ差し替えました: 対象 %d 件、差し替え %d 件、対象外 %d 件", result.eligible,
                result.swapped, result.rejected);
            if (result.restored > 0) {
                Warn(L"プロキシが見つからないので元素材へ戻しました: %d 件", result.restored);
            }
            if (result.unsupported > 0) {
                Warn(L"プロキシを作れない素材が %d 件あります。本体は読めますがこのプラグインの復号が対応していない形式です",
                     result.unsupported);
            }
            PublishStateChange();
        } else if ((taken & kRequestAutomatic) && CurrentSettings().enabled &&
                   !g_suspended.load()) {
            ScanResult result = ApplyProxies();
            if (result.swapped > 0) {
                Say(L"自動でプロキシへ差し替えました: %d 件", result.swapped);
            }
            if (result.restored > 0) {
                Warn(L"プロキシが見つからないので元素材へ戻しました: %d 件", result.restored);
            }
            PublishStateChange();
            (void)result;
        }
    }
}

void OnHostEvent(void*) {
    BeginScanning();
    NoticeEditActivity();
    Post(kRequestAutomatic);
}

void OnProjectLoad(PROJECT_FILE*) {
    BeginScanning();
    Post(kRequestAutomatic);
}

void OnEditMenuApply(EDIT_SECTION*) {
    Post(kRequestApply);
}

void OnEditMenuRestore(EDIT_SECTION*) {
    Post(kRequestRestore);
}

}

bool IsProxyPath(const std::wstring& path) {
    if (path.size() < 4) return false;
    return _wcsicmp(path.c_str() + path.size() - 4, L".pxy") == 0;
}

std::wstring SourceOfProxy(const std::wstring& path) {
    ProxyReader reader;
    if (!reader.Open(path)) return RecallSource(path);
    std::wstring source = reader.Header().source_path;
    reader.Close();
    return source;
}

void RequestApply() {
    Post(kRequestApply);
}

void RequestExportRestore() {
    Post(kRequestExportRestore);
}

void RequestDeleteProxies(const std::vector<std::wstring>& proxies) {
    if (proxies.empty()) return;
    {
        std::lock_guard<std::mutex> lock(g_report_lock);
        for (const std::wstring& proxy : proxies) g_delete_list.push_back(proxy);
    }
    Post(kRequestDelete);
}

void BeginScanning() {
    g_started.store(true);
}

void RequestRestoreProxy(const std::wstring& proxy) {
    {
        std::lock_guard<std::mutex> lock(g_report_lock);
        for (const std::wstring& known : g_failed_proxies) {
            if (_wcsicmp(known.c_str(), proxy.c_str()) == 0) return;
        }
        g_failed_proxies.push_back(proxy);
    }
    Post(kRequestRestoreFailed);
}

std::vector<Unsupported> UnsupportedSources() {
    std::lock_guard<std::mutex> lock(g_report_lock);
    return g_unsupported;
}

void SuspendAutomaticScan(bool suspend) {
    g_suspended.store(suspend);
}

bool AutomaticScanSuspended() {
    return g_suspended.load();
}

void RequestRestore() {
    Post(kRequestRestore);
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
    host->register_event_listener(EVENT_TYPE::CHANGE_EDIT_FRAME, nullptr, OnHostEvent);
    host->register_event_listener(EVENT_TYPE::CHANGE_FOCUS_OBJECT, nullptr, OnHostEvent);
    host->register_project_load_handler(OnProjectLoad);
}

}
