#pragma once

// EventBus — strongly-typed queued pub/sub.
//
// Design (ECS_Architecture_Design.md §"Event Bus 的設計要點"):
//   - One channel per event type, keyed by std::type_index — same pattern as
//     ECS ComponentPool storage, so lookup cost is one hash map probe per
//     Publish / Subscribe / Dispatch.
//   - Publish is QUEUED: events go into a per-type vector and are delivered
//     in DispatchAll() / DispatchOne<E>(). Subscribers never run inside
//     Publish, eliminating reentrancy.
//   - Events published from inside a subscriber callback are deferred to the
//     next dispatch (we swap `pending` out before iterating so mid-callback
//     Publishes land in the now-empty `pending`).
//   - Unsubscribe during dispatch is safe: subs are tombstoned (active=false)
//     and compacted opportunistically.
//
// Not supported (yet): sync publish, cross-thread publish. All calls must
// come from the main thread. Add a per-channel mutex if worker threads need
// to publish (see TaskSystem workers — currently none do).
//
// Intentionally header-only: the singleton's state lives in a function-local
// static so linking is trivial and there is no .cpp to maintain.

#include <cstddef>
#include <functional>
#include <memory>
#include <typeindex>
#include <unordered_map>
#include <utility>
#include <vector>

class EventBus
{
public:
    using SubscriptionId = std::size_t;
    static constexpr SubscriptionId kInvalidSub = 0;

    static EventBus& Get()
    {
        static EventBus instance;
        return instance;
    }

    // Enqueue an event. O(1) amortized.
    template <typename E>
    void Publish(E event)
    {
        channel<E>().pending.push_back(std::move(event));
    }

    // Register a subscriber. Returns a non-zero id; pass to Unsubscribe<E>.
    template <typename E>
    SubscriptionId Subscribe(std::function<void(const E&)> callback)
    {
        auto& c = channel<E>();
        const SubscriptionId id = ++c.nextSubId;
        c.subs.push_back({ id, std::move(callback), true });
        return id;
    }

    // Marks the subscription inactive; safe to call from inside a callback.
    // Returns true if the id was found.
    template <typename E>
    bool Unsubscribe(SubscriptionId id)
    {
        if (id == kInvalidSub) return false;
        auto* ch = findChannel<E>();
        if (!ch) return false;
        for (auto& s : ch->subs)
            if (s.id == id && s.active) { s.active = false; return true; }
        return false;
    }

    // Drain every channel once. Callback-emitted events land in the next pass.
    void DispatchAll()
    {
        for (auto& [idx, ch] : m_channels)
            ch->Dispatch();
    }

    // Drain one channel — useful if a caller needs tighter control over when
    // a specific event type is delivered (e.g. flush script-facing events
    // at a different point than engine-internal ones).
    template <typename E>
    void DispatchOne()
    {
        if (auto* ch = findChannel<E>())
            ch->Dispatch();
    }

    // Wipe all queues + subscribers. Intended for shutdown / test harnesses.
    void Clear()
    {
        m_channels.clear();
    }

private:
    struct IChannel
    {
        virtual ~IChannel() = default;
        virtual void Dispatch() = 0;
    };

    template <typename E>
    struct Channel : IChannel
    {
        struct Sub
        {
            SubscriptionId                 id;
            std::function<void(const E&)>  callback;
            bool                           active;
        };

        std::vector<E>   pending;         // enqueued by Publish, drained in Dispatch
        std::vector<Sub> subs;
        SubscriptionId   nextSubId = 0;

        void Dispatch() override
        {
            // Swap pending out first so subscribers that Publish<E> land in
            // the next pass, not this one. This guarantees forward progress
            // even if a subscriber always republishes.
            std::vector<E> processing;
            std::swap(processing, pending);

            for (const E& event : processing)
                for (const Sub& s : subs)
                    if (s.active) s.callback(event);

            // Compact tombstones when they pass a threshold — keeps the
            // inner loop tight without paying for compaction every frame.
            std::size_t inactiveCount = 0;
            for (const Sub& s : subs) if (!s.active) ++inactiveCount;
            if (inactiveCount * 2 > subs.size() && !subs.empty())
            {
                std::vector<Sub> live;
                live.reserve(subs.size() - inactiveCount);
                for (Sub& s : subs) if (s.active) live.push_back(std::move(s));
                subs = std::move(live);
            }
        }
    };

    template <typename E>
    Channel<E>& channel()
    {
        const std::type_index ti{ typeid(E) };
        auto it = m_channels.find(ti);
        if (it != m_channels.end())
            return *static_cast<Channel<E>*>(it->second.get());

        auto owned = std::make_unique<Channel<E>>();
        Channel<E>* raw = owned.get();
        m_channels.emplace(ti, std::move(owned));
        return *raw;
    }

    template <typename E>
    Channel<E>* findChannel()
    {
        const std::type_index ti{ typeid(E) };
        auto it = m_channels.find(ti);
        return it == m_channels.end()
            ? nullptr
            : static_cast<Channel<E>*>(it->second.get());
    }

    std::unordered_map<std::type_index, std::unique_ptr<IChannel>> m_channels;
};
