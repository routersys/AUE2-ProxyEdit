#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace pe {

enum ChunkState : unsigned char {
    kChunkMissing = 0,
    kChunkWorking = 1,
    kChunkReady = 2,
};

struct JobProgress {
    std::wstring source;
    std::wstring proxy;
    int source_width = 0;
    int source_height = 0;
    int proxy_width = 0;
    int proxy_height = 0;
    int frame_count = 0;
    int chunk_frames = 0;
    int ready_chunks = 0;
    int total_chunks = 0;
    long long bytes = 0;
    double frames_per_second = 0.0;
    bool failed = false;
    bool complete = false;
    std::wstring message;
    std::vector<unsigned char> chunks;
};

struct BuilderSummary {
    int jobs = 0;
    int queued_chunks = 0;
    long long bytes = 0;
    long long capacity = 0;
    bool paused = false;
    bool working = false;
    double overall = 0.0;
};

void StartBuilder();
void StopBuilder();

bool RegisterSource(const std::wstring& source, std::wstring& proxy_path);
void FocusSource(const std::wstring& source, int frame);
void RequestWholeSource(const std::wstring& source);
void DiscardSource(const std::wstring& source);
void SetBuilderPaused(bool paused);
bool BuilderPaused();
bool SourceFailed(const std::wstring& source);

std::vector<JobProgress> BuilderSnapshot();
BuilderSummary BuilderState();

}
