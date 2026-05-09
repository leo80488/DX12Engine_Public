#include "UI/WidgetRegistry.h"

namespace UI
{
    WidgetRegistry& WidgetRegistry::Get()
    {
        static WidgetRegistry s_inst;
        return s_inst;
    }

    WidgetHandle WidgetRegistry::Acquire(Widget* w)
    {
        if (!w) return {};
        if (!m_freeList.empty())
        {
            const uint32_t id = m_freeList.back();
            m_freeList.pop_back();
            m_slots[id].ptr = w;
            // generation was already bumped in Release
            return WidgetHandle{ id, m_slots[id].generation };
        }
        const uint32_t id = static_cast<uint32_t>(m_slots.size());
        m_slots.push_back({ w, 1u });
        return WidgetHandle{ id, 1u };
    }

    void WidgetRegistry::Release(WidgetHandle h)
    {
        if (!h.IsValid() || h.id >= m_slots.size()) return;
        Slot& s = m_slots[h.id];
        if (s.generation != h.generation) return; // already freed/reused
        s.ptr = nullptr;
        ++s.generation; // invalidate every copy of this handle
        m_freeList.push_back(h.id);
    }

    Widget* WidgetRegistry::Get(WidgetHandle h) const
    {
        if (!h.IsValid() || h.id >= m_slots.size()) return nullptr;
        const Slot& s = m_slots[h.id];
        if (s.generation != h.generation) return nullptr;
        return s.ptr;
    }

} // namespace UI
