#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <atomic>
#include <memory>
#include <type_traits>
#include <utility>

namespace audioalgo::editor {

// Worker threads carry this shared state instead of JUCE SafePointer: JUCE's
// WeakReference is explicitly not thread-safe. Only the message thread reads
// the component pointer, while invalidation is serialized with callback delivery.
template <typename ComponentType>
class ComponentDispatchState final {
public:
    explicit ComponentDispatchState(ComponentType& component) noexcept : component_(&component) {}

    void invalidate() noexcept {
        jassert(juce::MessageManager::getInstance()->isThisTheMessageThread());
        is_live_.store(false, std::memory_order_release);
        component_.store(nullptr, std::memory_order_release);
    }

    [[nodiscard]] bool is_live() const noexcept { return is_live_.load(std::memory_order_acquire); }

    [[nodiscard]] ComponentType* get_on_message_thread() const noexcept {
        jassert(juce::MessageManager::getInstance()->isThisTheMessageThread());
        return component_.load(std::memory_order_acquire);
    }

private:
    std::atomic<bool> is_live_{true};
    std::atomic<ComponentType*> component_;
};

template <typename ComponentType, typename Callback>
[[nodiscard]] bool dispatch_to_live_component(
    std::weak_ptr<ComponentDispatchState<ComponentType>> target,
    Callback&& callback) {
    auto state = target.lock();
    if (!state || !state->is_live())
        return false;

    using StoredCallback = std::decay_t<Callback>;
    return juce::MessageManager::callAsync(
        [state = std::move(state), callback = StoredCallback(std::forward<Callback>(callback))]() mutable {
            if (auto* component = state->get_on_message_thread())
                callback(*component);
        });
}

} // namespace audioalgo::editor
