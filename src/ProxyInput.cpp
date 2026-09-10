#include "ProxyInput.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <exception>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

#include "AudioDecoder.h"
#include "ColorConvert.h"
#include "HostContext.h"
#include "JpegCodec.h"
#include "Log.h"
#include "MediaDecoder.h"
#include "ProxyBuilder.h"
#include "ProxyFormat.h"
#include "ProxyStore.h"
#include "Settings.h"
#include "input2.h"

namespace pe {

namespace {

struct Frame {
    std::vector<unsigned char> data;
};

using FramePointer = std::shared_ptr<Frame>;

struct Session {
    ProxyReader reader;
    ProxyHeader header{};
    BITMAPINFOHEADER format{};
    size_t bytes = 0;
    int stride = 0;

    MediaDecoder original;
    std::mutex original_lock;
    std::atomic<bool> original_ready{false};
    std::atomic<bool> original_failed{false};

    AudioDecoder audio;
    WAVEFORMATEX audio_format{};
    bool has_audio = false;

    std::mutex cache_lock;
    std::condition_variable wake;
    std::map<int, FramePointer> cache;
    std::set<int> inflight;
    std::deque<int> queue;
    std::vector<std::thread> workers;
    bool stop = false;
    int ahead = 0;
    int last_frame = -100000;
    int sequential = 0;
};

std::atomic<int> g_sessions{0};

bool DecodeProxyFrame(Session* session, int frame, unsigned char* destination) {
    std::vector<unsigned char> encoded;
    if (!session->reader.ReadFrame(frame, encoded)) return false;
    return DecodeJpegToYuy2(encoded.data(), encoded.size(), session->header.proxy_width,
                            session->header.proxy_height, destination, session->header.source_width,
                            session->header.source_height, session->stride);
}

bool DecodeOriginalFrame(Session* session, int frame, unsigned char* destination) {
    if (session->original_failed.load()) return false;
    std::lock_guard<std::mutex> lock(session->original_lock);
    if (!session->original_ready.load()) {
        if (!session->original.Open(session->header.source_path)) {
            session->original_failed.store(true);
            Warn(L"元素材を読み込めませんでした: %s", session->header.source_path);
            return false;
        }
        session->original_ready.store(true);
    }
    return session->original.ReadFrameYuy2(frame, destination, session->stride,
                                           session->header.source_height);
}

bool ProduceFrame(Session* session, int frame, unsigned char* destination) {
    if (Exporting()) {
        if (DecodeOriginalFrame(session, frame, destination)) return true;
        if (DecodeProxyFrame(session, frame, destination)) return true;
        FillBlackYuy2(destination, session->header.source_width, session->header.source_height,
                      session->stride);
        return true;
    }
    if (DecodeProxyFrame(session, frame, destination)) return true;
    if (DecodeOriginalFrame(session, frame, destination)) return true;
    FillBlackYuy2(destination, session->header.source_width, session->header.source_height,
                  session->stride);
    return true;
}

void SessionWorker(Session* session) {
    for (;;) {
        int frame = -1;
        {
            std::unique_lock<std::mutex> lock(session->cache_lock);
            session->wake.wait(lock, [session] { return session->stop || !session->queue.empty(); });
            if (session->stop) return;
            frame = session->queue.front();
            session->queue.pop_front();
            if (session->cache.count(frame) || session->inflight.count(frame)) continue;
            session->inflight.insert(frame);
        }
        FramePointer produced = std::make_shared<Frame>();
        produced->data.resize(session->bytes);
        ProduceFrame(session, frame, produced->data.data());
        {
            std::lock_guard<std::mutex> lock(session->cache_lock);
            session->inflight.erase(frame);
            session->cache[frame] = produced;
        }
    }
}

INPUT_HANDLE OnOpenBody(LPCWSTR file) {
    ProxyReader probe;
    if (!probe.Open(file)) return nullptr;
    Session* session = new Session();
    session->header = probe.Header();
    probe.Close();
    if (!session->reader.Open(file)) {
        delete session;
        return nullptr;
    }
    session->stride = session->header.source_width * 2;
    session->bytes = (size_t)session->stride * session->header.source_height;
    session->ahead = ReadAheadFrames(session->header.source_width, session->header.source_height);
    session->has_audio = session->audio.Open(session->header.source_path);
    int count = std::clamp(EffectiveWorkerCount() / 2, 1, 3);
    for (int index = 0; index < count; index++) {
        session->workers.emplace_back(SessionWorker, session);
    }
    g_sessions.fetch_add(1);
    return (INPUT_HANDLE)session;
}

bool OnCloseBody(INPUT_HANDLE handle) {
    Session* session = (Session*)handle;
    if (!session) return true;
    {
        std::lock_guard<std::mutex> lock(session->cache_lock);
        session->stop = true;
    }
    session->wake.notify_all();
    for (std::thread& worker : session->workers) {
        if (worker.joinable()) worker.join();
    }
    session->reader.Close();
    session->original.Close();
    session->audio.Close();
    delete session;
    g_sessions.fetch_sub(1);
    return true;
}

bool OnInfoGetBody(INPUT_HANDLE handle, INPUT_INFO* info) {
    Session* session = (Session*)handle;
    if (!session || !info) return false;
    session->format.biSize = sizeof(BITMAPINFOHEADER);
    session->format.biWidth = session->header.source_width;
    session->format.biHeight = session->header.source_height;
    session->format.biPlanes = 1;
    session->format.biBitCount = 16;
    session->format.biCompression = MAKEFOURCC('Y', 'U', 'Y', '2');
    session->format.biSizeImage = (DWORD)session->bytes;
    info->flag = INPUT_INFO::FLAG_VIDEO;
    info->rate = session->header.rate;
    info->scale = session->header.scale;
    info->n = session->header.frame_count;
    info->format = &session->format;
    info->format_size = sizeof(BITMAPINFOHEADER);
    info->audio_n = 0;
    info->audio_format = nullptr;
    info->audio_format_size = 0;
    if (session->has_audio) {
        session->audio_format.wFormatTag = WAVE_FORMAT_PCM;
        session->audio_format.nChannels = (WORD)session->audio.Channels();
        session->audio_format.nSamplesPerSec = (DWORD)session->audio.SampleRate();
        session->audio_format.wBitsPerSample = 16;
        session->audio_format.nBlockAlign =
            (WORD)(session->audio_format.nChannels * session->audio_format.wBitsPerSample / 8);
        session->audio_format.nAvgBytesPerSec =
            session->audio_format.nSamplesPerSec * session->audio_format.nBlockAlign;
        session->audio_format.cbSize = 0;
        info->flag |= INPUT_INFO::FLAG_AUDIO;
        info->audio_n = (int)session->audio.SampleCount();
        info->audio_format = &session->audio_format;
        info->audio_format_size = sizeof(WAVEFORMATEX);
    }
    return true;
}

int OnReadVideoBody(INPUT_HANDLE handle, int frame, void* buffer) {
    Session* session = (Session*)handle;
    if (!session || !buffer) return 0;
    if (frame < 0) frame = 0;
    if (frame >= session->header.frame_count) frame = session->header.frame_count - 1;

    FocusSource(session->header.source_path, frame);

    FramePointer hit;
    {
        std::lock_guard<std::mutex> lock(session->cache_lock);
        if (frame == session->last_frame + 1) {
            if (session->sequential < 1000) session->sequential++;
        } else if (frame != session->last_frame) {
            session->sequential = 0;
        }
        session->last_frame = frame;

        auto found = session->cache.find(frame);
        if (found != session->cache.end()) hit = found->second;

        for (auto entry = session->cache.begin(); entry != session->cache.end();) {
            if (entry->first < frame - 1 || entry->first > frame + session->ahead + 1) {
                entry = session->cache.erase(entry);
            } else {
                ++entry;
            }
        }
        session->queue.clear();
        if (session->sequential >= 2 && !Exporting()) {
            for (int step = 1; step <= session->ahead; step++) {
                int next = frame + step;
                if (next >= session->header.frame_count) break;
                if (session->cache.count(next) || session->inflight.count(next)) continue;
                session->queue.push_back(next);
            }
        }
    }
    session->wake.notify_all();

    if (hit) {
        memcpy(buffer, hit->data.data(), session->bytes);
        return (int)session->bytes;
    }
    ProduceFrame(session, frame, (unsigned char*)buffer);
    return (int)session->bytes;
}

int OnReadAudioBody(INPUT_HANDLE handle, int start, int length, void* buffer) {
    Session* session = (Session*)handle;
    if (!session || !buffer || !session->has_audio) return 0;
    return session->audio.Read(start, length, buffer);
}

INPUT_HANDLE OnOpen(LPCWSTR file) {
    try {
        return OnOpenBody(file);
    } catch (const std::exception& error) {
        Warn(L"プロキシを開けませんでした: %S", error.what());
    } catch (...) {
        Warn(L"プロキシを開けませんでした");
    }
    return nullptr;
}

bool OnClose(INPUT_HANDLE handle) {
    try {
        return OnCloseBody(handle);
    } catch (...) {
        Warn(L"プロキシを閉じる途中で失敗しました");
    }
    return true;
}

bool OnInfoGet(INPUT_HANDLE handle, INPUT_INFO* info) {
    try {
        return OnInfoGetBody(handle, info);
    } catch (...) {
        Warn(L"プロキシの情報を取得できませんでした");
    }
    return false;
}

int OnReadVideo(INPUT_HANDLE handle, int frame, void* buffer) {
    try {
        return OnReadVideoBody(handle, frame, buffer);
    } catch (...) {
        Warn(L"プロキシの読み出しに失敗しました");
    }
    return 0;
}

int OnReadAudio(INPUT_HANDLE handle, int start, int length, void* buffer) {
    try {
        return OnReadAudioBody(handle, start, length, buffer);
    } catch (...) {
        Warn(L"プロキシの音声読み出しに失敗しました");
    }
    return 0;
}

INPUT_PLUGIN_TABLE g_table = {
    INPUT_PLUGIN_TABLE::FLAG_VIDEO | INPUT_PLUGIN_TABLE::FLAG_AUDIO |
        INPUT_PLUGIN_TABLE::FLAG_CONCURRENT,
    L"プロキシ編集",
    L"プロキシ編集 (*.pxy)\0*.pxy\0",
    L"プロキシ編集 version 1.0.0",
    OnOpen,
    OnClose,
    OnInfoGet,
    OnReadVideo,
    OnReadAudio,
    nullptr,
    nullptr,
    nullptr,
};

}

INPUT_PLUGIN_TABLE* ProxyInputTable() {
    return &g_table;
}

void ShutdownProxyInput() {
}

}
