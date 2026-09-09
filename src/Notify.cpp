#include "Notify.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

namespace pe {

namespace {

struct Listener {
    HWND window = nullptr;
    UINT message = 0;
    std::atomic<bool> pending{false};
};

std::vector<std::unique_ptr<Listener>> g_listeners;
std::mutex g_lock;

}

void AddStateListener(HWND window, UINT message) {
    if (!window) return;
    std::lock_guard<std::mutex> lock(g_lock);
    for (const auto& listener : g_listeners) {
        if (listener->window == window) return;
    }
    auto listener = std::make_unique<Listener>();
    listener->window = window;
    listener->message = message;
    g_listeners.push_back(std::move(listener));
}

void RemoveStateListener(HWND window) {
    std::lock_guard<std::mutex> lock(g_lock);
    for (size_t index = 0; index < g_listeners.size(); index++) {
        if (g_listeners[index]->window != window) continue;
        g_listeners.erase(g_listeners.begin() + (ptrdiff_t)index);
        return;
    }
}

void AcknowledgeStateChange(HWND window) {
    std::lock_guard<std::mutex> lock(g_lock);
    for (const auto& listener : g_listeners) {
        if (listener->window == window) listener->pending.store(false);
    }
}

void PublishStateChange() {
    std::lock_guard<std::mutex> lock(g_lock);
    for (const auto& listener : g_listeners) {
        if (listener->pending.exchange(true)) continue;
        if (!PostMessageW(listener->window, listener->message, 0, 0)) listener->pending.store(false);
    }
}

}
