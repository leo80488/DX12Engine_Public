#pragma once

// WidgetRegistry — generation-based weak handles to Widget instances.
//
// Lua (and any external script layer) holds WidgetHandle, NOT raw pointers,
// because widgets can be destroyed when their owning UIRoot/Entity is gone.
// Each handle carries an id + generation; Get(handle) returns nullptr when
// the slot was reused or freed (mirrors design doc §6.3).
//
// Single-threaded: the UI subsystem ticks on the main thread; the registry
// is non-locking on purpose. If we ever cross threads we'll wrap accesses.

#include <cstdint>
#include <vector>

namespace UI
{
    class Widget;

    struct WidgetHandle
    {
        uint32_t id         = 0;
        uint32_t generation = 0;

        bool IsValid() const { return id != 0; }
        bool operator==(const WidgetHandle& o) const
        { return id == o.id && generation == o.generation; }
        bool operator!=(const WidgetHandle& o) const { return !(*this == o); }
    };

    class WidgetRegistry
    {
    public:
        // Register @p w and return a fresh handle. Caller still owns the
        // Widget pointer (typically held inside a unique_ptr); this only
        // tracks a non-owning reference.
        WidgetHandle Acquire(Widget* w);

        // Drop @p h. The slot becomes available for re-use; the generation
        // bumps so any leftover WidgetHandle copies start returning nullptr.
        void Release(WidgetHandle h);

        // Resolve @p h. Returns nullptr if the slot was reused / freed.
        Widget* Get(WidgetHandle h) const;

        // Singleton access — UI is a single-instance subsystem.
        static WidgetRegistry& Get();

    private:
        struct Slot
        {
            Widget*  ptr        = nullptr;
            uint32_t generation = 0;
        };
        std::vector<Slot>     m_slots;
        std::vector<uint32_t> m_freeList;

        // Reserve slot 0 as "invalid" so a default-constructed handle is null.
        WidgetRegistry() { m_slots.emplace_back(); }
    };

} // namespace UI
