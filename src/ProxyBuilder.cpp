#include "ProxyBuilder.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

#include "ColorConvert.h"
#include "JpegCodec.h"
#include "Log.h"
#include "MediaDecoder.h"
#include "Notify.h"
#include "ProxyFormat.h"
#include "ProxyStore.h"
#include "ScanController.h"
#include "Settings.h"

namespace pe {

namespace {

struct Job {
    SourceKey key;
    std::wstring proxy;
    ProxyHeader header{};
    ProxyWriter writer;
    MediaDecoder decoder;
    std::mutex work;
    std::mutex state_lock;
    std::vector<unsigned char> chunks;
    std::atomic<int> focus{0};
    std::atomic<bool> want_all{false};
    std::atomic<bool> failed{false};
    std::atomic<bool> opened{false};
    std::atomic<double> speed{0.0};
    std::wstring message;
};

using JobPointer = std::shared_ptr<Job>;

std::map<std::wstring, JobPointer> g_jobs;
std::mutex g_registry;
std::vector<std::thread> g_workers;
std::condition_variable g_wake;
std::mutex g_wake_lock;
std::atomic<bool> g_stop{false};
std::atomic<bool> g_paused{false};
std::atomic<int> g_active{0};
std::atomic<unsigned long long> g_generation{0};

void WakeWorkers() {
    {
        std::lock_guard<std::mutex> lock(g_wake_lock);
        g_generation.fetch_add(1);
    }
    g_wake.notify_all();
}

std::wstring Normalized(const std::wstring& path) {
    std::wstring lowered = path;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                   [](wchar_t character) { return (wchar_t)towlower(character); });
    return lowered;
}

int TotalChunks(const ProxyHeader& header) {
    if (header.chunk_frames <= 0) return 0;
    return (header.frame_count + header.chunk_frames - 1) / header.chunk_frames;
}

void RefreshChunkState(const JobPointer& job) {
    std::lock_guard<std::mutex> lock(job->state_lock);
    const int total = TotalChunks(job->header);
    job->chunks.assign((size_t)total, kChunkMissing);
    for (int index = 0; index < total; index++) {
        int begin = index * job->header.chunk_frames;
        int end = std::min(begin + job->header.chunk_frames, job->header.frame_count);
        bool ready = true;
        for (int frame = begin; frame < end; frame += 8) {
            if (!job->writer.HasFrame(frame)) {
                ready = false;
                break;
            }
        }
        if (ready && end > begin && !job->writer.HasFrame(end - 1)) ready = false;
        job->chunks[(size_t)index] = ready ? kChunkReady : kChunkMissing;
    }
}

bool PickChunk(const JobPointer& job, int& chunk) {
    std::lock_guard<std::mutex> lock(job->state_lock);
    const int total = (int)job->chunks.size();
    if (total == 0) return false;
    const int frames = job->header.chunk_frames > 0 ? job->header.chunk_frames : 1;
    int center = job->focus.load() / frames;
    if (center < 0) center = 0;
    if (center >= total) center = total - 1;
    const bool everything = job->want_all.load();
    const int reach = everything ? total : 12;
    for (int distance = 0; distance <= reach; distance++) {
        for (int side = 0; side < 2; side++) {
            int index = side == 0 ? center + distance : center - distance;
            if (index < 0 || index >= total) continue;
            if (job->chunks[(size_t)index] != kChunkMissing) continue;
            job->chunks[(size_t)index] = kChunkWorking;
            chunk = index;
            return true;
        }
    }
    return false;
}

void MarkChunk(const JobPointer& job, int chunk, unsigned char state) {
    {
        std::lock_guard<std::mutex> lock(job->state_lock);
        if (chunk >= 0 && chunk < (int)job->chunks.size()) job->chunks[(size_t)chunk] = state;
    }
    PublishStateChange();
}

bool OpenDecoder(const JobPointer& job) {
    if (job->opened.load()) return true;
    if (!job->decoder.Open(job->key.path)) {
        job->failed.store(true);
        {
            std::lock_guard<std::mutex> lock(job->state_lock);
            job->message = L"この形式は Media Foundation で読み込めません";
        }
        Warn(L"元素材を開けませんでした: %s", job->key.path.c_str());
        return false;
    }
    job->opened.store(true);
    return true;
}

void FailJob(const JobPointer& job, const wchar_t* reason) {
    job->failed.store(true);
    {
        std::lock_guard<std::mutex> lock(job->state_lock);
        job->message = reason;
    }
    Warn(L"生成を中止しました: %s (%s)", reason, job->key.path.c_str());
    RequestRestoreProxy(job->proxy);
    PublishStateChange();
}

void ProcessChunk(const JobPointer& job, int chunk) {
    const ProxyHeader& header = job->header;
    const int begin = chunk * header.chunk_frames;
    const int end = std::min(begin + header.chunk_frames, header.frame_count);
    const int stride = header.source_width * 2;
    std::vector<unsigned char> picture((size_t)stride * header.source_height);
    const int chroma_width = header.proxy_width / 2;
    std::vector<unsigned char> luma((size_t)header.proxy_width * header.proxy_height);
    std::vector<unsigned char> blue((size_t)chroma_width * header.proxy_height);
    std::vector<unsigned char> red((size_t)chroma_width * header.proxy_height);
    std::vector<unsigned char> encoded;

    auto started = std::chrono::steady_clock::now();
    int produced = 0;
    for (int frame = begin; frame < end && !g_stop.load(); frame++) {
        if (job->writer.HasFrame(frame)) continue;
        if (!job->decoder.ReadFrameYuy2(frame, picture.data(), stride, header.source_height)) {
            FailJob(job, L"復号できないフレームがありました");
            break;
        }
        Yuy2ToPlanarScaled(picture.data(), header.source_width, header.source_height, stride,
                           luma.data(), blue.data(), red.data(), header.proxy_width,
                           header.proxy_height);
        if (!EncodeJpegPlanar(luma.data(), blue.data(), red.data(), header.proxy_width,
                              header.proxy_height, header.quality, encoded)) {
            FailJob(job, L"プロキシの符号化に失敗しました");
            break;
        }
        if (!job->writer.WriteFrame(frame, encoded.data(), (int)encoded.size())) {
            FailJob(job, L"プロキシを書き込めませんでした");
            break;
        }
        produced++;
    }
    auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    if (produced > 0 && elapsed > 0.0) job->speed.store((double)produced / elapsed);

    bool ready = !job->failed.load();
    if (ready) {
        for (int frame = begin; frame < end; frame++) {
            if (!job->writer.HasFrame(frame)) {
                ready = false;
                break;
            }
        }
    }
    MarkChunk(job, chunk, ready ? kChunkReady : kChunkMissing);
}

bool TakeWork(JobPointer& picked, int& chunk) {
    std::vector<JobPointer> candidates;
    {
        std::lock_guard<std::mutex> lock(g_registry);
        candidates.reserve(g_jobs.size());
        for (const auto& entry : g_jobs) candidates.push_back(entry.second);
    }
    for (const JobPointer& job : candidates) {
        if (job->failed.load()) continue;
        if (!job->work.try_lock()) continue;
        int selected = -1;
        if (PickChunk(job, selected)) {
            picked = job;
            chunk = selected;
            return true;
        }
        job->work.unlock();
    }
    return false;
}

void Worker() {
    while (!g_stop.load()) {
        const unsigned long long seen = g_generation.load();
        if (g_paused.load()) {
            std::unique_lock<std::mutex> lock(g_wake_lock);
            g_wake.wait(lock, [seen] {
                return g_stop.load() || !g_paused.load() || g_generation.load() != seen;
            });
            continue;
        }
        JobPointer job;
        int chunk = -1;
        if (!TakeWork(job, chunk)) {
            std::unique_lock<std::mutex> lock(g_wake_lock);
            g_wake.wait(lock, [seen] { return g_stop.load() || g_generation.load() != seen; });
            continue;
        }
        g_active.fetch_add(1);
        if (OpenDecoder(job)) {
            ReleaseCapacity((long long)job->header.chunk_frames * 128 * 1024, job->proxy);
            ProcessChunk(job, chunk);
        } else {
            MarkChunk(job, chunk, kChunkMissing);
        }
        g_active.fetch_sub(1);
        job->work.unlock();
        WakeWorkers();
        PublishStateChange();
    }
}

}

void StartBuilder() {
    if (!g_workers.empty()) return;
    g_stop.store(false);
    EnsureStoreDirectory();
    int count = EffectiveWorkerCount();
    for (int index = 0; index < count; index++) g_workers.emplace_back(Worker);
    Say(L"プロキシの生成を %d 本の処理で開始しました", count);
}

void StopBuilder() {
    g_stop.store(true);
    WakeWorkers();
    for (std::thread& worker : g_workers) {
        if (worker.joinable()) worker.join();
    }
    g_workers.clear();
    std::map<std::wstring, JobPointer> jobs;
    {
        std::lock_guard<std::mutex> lock(g_registry);
        jobs.swap(g_jobs);
    }
    for (auto& entry : jobs) {
        entry.second->writer.Close();
        entry.second->decoder.Close();
    }
}

bool RegisterSource(const std::wstring& source, std::wstring& proxy_path) {
    SourceKey key;
    if (!QuerySource(source, key)) return false;
    const std::wstring identity = Normalized(source);
    {
        std::lock_guard<std::mutex> lock(g_registry);
        auto found = g_jobs.find(identity);
        if (found != g_jobs.end()) {
            proxy_path = found->second->proxy;
            return !found->second->failed.load();
        }
    }

    JobPointer job = std::make_shared<Job>();
    job->key = key;
    job->proxy = ProxyPathFor(key);

    MediaDecoder probe;
    if (!probe.Open(source)) return false;
    const Settings& settings = CurrentSettings();
    int proxy_width = probe.Width() * settings.scale_percent / 100;
    int proxy_height = probe.Height() * settings.scale_percent / 100;
    proxy_width -= proxy_width % 2;
    proxy_height -= proxy_height % 2;
    if (proxy_width < 16 || proxy_height < 16) return false;

    ProxyHeader header{};
    header.source_width = probe.Width();
    header.source_height = probe.Height();
    header.rate = probe.Rate();
    header.scale = probe.Scale();
    header.frame_count = (int)std::min<long long>(probe.FrameCount(), 2000000);
    header.proxy_width = proxy_width;
    header.proxy_height = proxy_height;
    header.quality = settings.quality;
    header.chunk_frames = settings.chunk_frames;
    header.source_size = key.size;
    header.source_time = key.time;
    wcsncpy_s(header.source_path, key.path.c_str(), _TRUNCATE);
    probe.Close();

    EnsureStoreDirectory();
    bool reused = false;
    if (job->writer.OpenExisting(job->proxy)) {
        const ProxyHeader& existing = job->writer.Header();
        if (SameSource(key, existing.source_size, existing.source_time) &&
            _wcsicmp(existing.source_path, key.path.c_str()) == 0 &&
            existing.proxy_width == header.proxy_width && existing.proxy_height == header.proxy_height &&
            existing.frame_count == header.frame_count && existing.quality == header.quality &&
            existing.chunk_frames == header.chunk_frames) {
            job->header = existing;
            reused = true;
        } else {
            job->writer.Close();
        }
    }
    if (!reused) {
        if (!job->writer.Create(job->proxy, header)) {
            Warn(L"プロキシを作成できませんでした: %s", job->proxy.c_str());
            return false;
        }
        job->header = job->writer.Header();
    }
    RefreshChunkState(job);

    {
        std::lock_guard<std::mutex> lock(g_registry);
        g_jobs[identity] = job;
    }
    proxy_path = job->proxy;
    Say(L"プロキシを登録しました: %s -> %dx%d", source.c_str(), header.proxy_width, header.proxy_height);
    WakeWorkers();
    PublishStateChange();
    return true;
}

void FocusSource(const std::wstring& source, int frame) {
    std::lock_guard<std::mutex> lock(g_registry);
    auto found = g_jobs.find(Normalized(source));
    if (found == g_jobs.end()) return;
    found->second->focus.store(frame);
    WakeWorkers();
}

void RequestWholeSource(const std::wstring& source) {
    std::lock_guard<std::mutex> lock(g_registry);
    auto found = g_jobs.find(Normalized(source));
    if (found == g_jobs.end()) return;
    found->second->want_all.store(true);
    WakeWorkers();
    PublishStateChange();
}

void DiscardSource(const std::wstring& source) {
    JobPointer job;
    {
        std::lock_guard<std::mutex> lock(g_registry);
        auto found = g_jobs.find(Normalized(source));
        if (found == g_jobs.end()) return;
        job = found->second;
        g_jobs.erase(found);
    }
    std::lock_guard<std::mutex> lock(job->work);
    job->writer.Close();
    job->decoder.Close();
    RemoveProxy(job->proxy);
    PublishStateChange();
}

void SetBuilderPaused(bool paused) {
    g_paused.store(paused);
    WakeWorkers();
    PublishStateChange();
}

bool SourceFailed(const std::wstring& source) {
    std::lock_guard<std::mutex> lock(g_registry);
    auto found = g_jobs.find(Normalized(source));
    return found != g_jobs.end() && found->second->failed.load();
}

bool ProxyInUse(const std::wstring& proxy) {
    std::lock_guard<std::mutex> lock(g_registry);
    for (const auto& entry : g_jobs) {
        if (_wcsicmp(entry.second->proxy.c_str(), proxy.c_str()) == 0) return true;
    }
    return false;
}

bool BuilderPaused() {
    return g_paused.load();
}

std::vector<JobProgress> BuilderSnapshot() {
    std::vector<JobPointer> jobs;
    {
        std::lock_guard<std::mutex> lock(g_registry);
        for (const auto& entry : g_jobs) jobs.push_back(entry.second);
    }
    std::vector<JobProgress> result;
    result.reserve(jobs.size());
    for (const JobPointer& job : jobs) {
        JobProgress progress;
        progress.source = job->key.path;
        progress.proxy = job->proxy;
        progress.source_width = job->header.source_width;
        progress.source_height = job->header.source_height;
        progress.proxy_width = job->header.proxy_width;
        progress.proxy_height = job->header.proxy_height;
        progress.frame_count = job->header.frame_count;
        progress.chunk_frames = job->header.chunk_frames;
        progress.bytes = job->writer.FileSize();
        progress.frames_per_second = job->speed.load();
        progress.failed = job->failed.load();
        {
            std::lock_guard<std::mutex> lock(job->state_lock);
            progress.chunks = job->chunks;
            progress.message = job->message;
        }
        progress.total_chunks = (int)progress.chunks.size();
        for (unsigned char state : progress.chunks) {
            if (state == kChunkReady) progress.ready_chunks++;
        }
        progress.complete = progress.total_chunks > 0 && progress.ready_chunks == progress.total_chunks;
        result.push_back(std::move(progress));
    }
    return result;
}

BuilderSummary BuilderState() {
    BuilderSummary summary;
    summary.paused = g_paused.load();
    summary.working = g_active.load() > 0;
    summary.capacity = CurrentSettings().capacity_bytes;
    int ready = 0;
    int total = 0;
    for (const JobProgress& progress : BuilderSnapshot()) {
        summary.jobs++;
        summary.bytes += progress.bytes;
        ready += progress.ready_chunks;
        total += progress.total_chunks;
        summary.queued_chunks += progress.total_chunks - progress.ready_chunks;
    }
    summary.overall = total > 0 ? (double)ready / (double)total : 1.0;
    return summary;
}

}
