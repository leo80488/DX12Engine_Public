#include "Reflection/ReflectionEditor.h"

#include "imgui/imgui.h"

#include <DirectXMath.h>
#include <cstdio>
#include <cstring>
#include <string>

namespace Reflect
{
namespace
{
    // Shift the byte pointer to the field location.
    inline void* FieldPtr(void* obj, std::size_t off)
    {
        return static_cast<unsigned char*>(obj) + off;
    }

    // Read an integer of `size` bytes (1/2/4) into a widened int. Used for
    // enum fields whose underlying type may be uint8_t / int / uint32_t.
    inline int ReadEnumInt(const void* p, std::size_t size)
    {
        switch (size)
        {
            case 1: return static_cast<int>(*static_cast<const uint8_t*>(p));
            case 2: return static_cast<int>(*static_cast<const uint16_t*>(p));
            case 4: return *static_cast<const int*>(p);
            default: return 0;
        }
    }

    // Narrow `value` and write into the enum-sized field.
    inline void WriteEnumInt(void* p, std::size_t size, int value)
    {
        switch (size)
        {
            case 1: *static_cast<uint8_t*>(p)  = static_cast<uint8_t>(value);  break;
            case 2: *static_cast<uint16_t*>(p) = static_cast<uint16_t>(value); break;
            case 4: *static_cast<int*>(p)      = value;                        break;
            default: break;
        }
    }

    // Apply a tooltip if the previous item is hovered. ImGui >= 1.89 has
    // ImGui::SetItemTooltip, but we hand-roll to stay compatible with the
    // simpler IsItemHovered + BeginTooltip flow used elsewhere in editor code.
    inline void MaybeTooltip(const char* tip)
    {
        if (tip && *tip && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
            ImGui::SetTooltip("%s", tip);
    }

    bool DrawFloat(void* p, const FieldDescriptor& f)
    {
        float* v = static_cast<float*>(p);
        const char* fmt = f.fmt ? f.fmt : "%.3f";
        if (f.widget == Reflect::Widget::Slider)
            return ImGui::SliderFloat(f.label, v, f.minVal, f.maxVal, fmt);
        if (f.widget == Reflect::Widget::Angle)
        {
            float deg = DirectX::XMConvertToDegrees(*v);
            if (ImGui::SliderFloat(f.label, &deg, f.minVal, f.maxVal, "%.1f deg"))
            {
                *v = DirectX::XMConvertToRadians(deg);
                return true;
            }
            return false;
        }
        // Default = Drag.
        return ImGui::DragFloat(f.label, v, f.speed, f.minVal, f.maxVal, fmt);
    }

    bool DrawFloat2(void* p, const FieldDescriptor& f)
    {
        float* v = static_cast<float*>(p);
        const char* fmt = f.fmt ? f.fmt : "%.3f";
        return ImGui::DragFloat2(f.label, v, f.speed, f.minVal, f.maxVal, fmt);
    }

    bool DrawFloat3(void* p, const FieldDescriptor& f)
    {
        float* v = static_cast<float*>(p);
        if (f.widget == Reflect::Widget::Color)
            return ImGui::ColorEdit3(f.label, v);
        const char* fmt = f.fmt ? f.fmt : "%.3f";
        return ImGui::DragFloat3(f.label, v, f.speed, f.minVal, f.maxVal, fmt);
    }

    bool DrawFloat4(void* p, const FieldDescriptor& f)
    {
        float* v = static_cast<float*>(p);
        if (f.widget == Reflect::Widget::Color)
            return ImGui::ColorEdit4(f.label, v);
        const char* fmt = f.fmt ? f.fmt : "%.3f";
        return ImGui::DragFloat4(f.label, v, f.speed, f.minVal, f.maxVal, fmt);
    }

    bool DrawInt(void* p, const FieldDescriptor& f)
    {
        int* v = static_cast<int*>(p);
        const int mn = static_cast<int>(f.minVal);
        const int mx = static_cast<int>(f.maxVal);
        if (f.widget == Reflect::Widget::Slider)
            return ImGui::SliderInt(f.label, v, mn, mx);
        return ImGui::DragInt(f.label, v, f.speed, mn, mx);
    }

    bool DrawUInt(void* p, const FieldDescriptor& f)
    {
        // ImGui has no DragUInt; route via a temporary signed int. Keeps the
        // entire range up to INT_MAX editable, which is plenty for the
        // counters / bitmasks reflected components carry today.
        unsigned* v  = static_cast<unsigned*>(p);
        int       tmp = static_cast<int>(*v);
        const int mn  = static_cast<int>(f.minVal);
        const int mx  = static_cast<int>(f.maxVal);
        bool changed = (f.widget == Reflect::Widget::Slider)
            ? ImGui::SliderInt(f.label, &tmp, mn, mx)
            : ImGui::DragInt(f.label, &tmp, f.speed, mn, mx);
        if (changed) *v = static_cast<unsigned>(tmp < 0 ? 0 : tmp);
        return changed;
    }

    bool DrawBool(void* p, const FieldDescriptor& f)
    {
        bool* v = static_cast<bool*>(p);
        return ImGui::Checkbox(f.label, v);
    }

    bool DrawEnum(void* p, const FieldDescriptor& f)
    {
        if (!f.enumOpts || f.enumCount == 0) return false;

        const int current = ReadEnumInt(p, f.enumSize);
        // Find current option to show its label as the combo preview.
        const char* preview = "<unknown>";
        int currentIdx = -1;
        for (std::size_t i = 0; i < f.enumCount; ++i)
            if (f.enumOpts[i].value == current) { preview = f.enumOpts[i].label; currentIdx = (int)i; break; }

        bool changed = false;
        if (ImGui::BeginCombo(f.label, preview))
        {
            for (std::size_t i = 0; i < f.enumCount; ++i)
            {
                const bool selected = ((int)i == currentIdx);
                if (ImGui::Selectable(f.enumOpts[i].label, selected))
                {
                    WriteEnumInt(p, f.enumSize, f.enumOpts[i].value);
                    changed = true;
                }
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        return changed;
    }

    bool DrawString(void* p, const FieldDescriptor& f)
    {
        // Fixed-size scratch buffer round-trip — no imgui_stdlib in this
        // project. 512 bytes covers every reflected string field today
        // (paths/labels). Bumping the cap is a one-line change if needed.
        constexpr std::size_t kCap = 512;
        auto* str = static_cast<std::string*>(p);
        char buf[kCap];
        const std::size_t n = (str->size() < kCap - 1) ? str->size() : (kCap - 1);
        std::memcpy(buf, str->data(), n);
        buf[n] = '\0';
        bool changed = false;
        if (ImGui::InputText(f.label, buf, kCap))
        {
            *str = buf;
            changed = true;
        }
        // Optional drag-drop receive — replaces the string with the dropped
        // payload data (assumed null-terminated path string).
        if (f.dropPayload && ImGui::BeginDragDropTarget())
        {
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(f.dropPayload))
            {
                *str = static_cast<const char*>(p->Data);
                changed = true;
            }
            ImGui::EndDragDropTarget();
        }
        return changed;
    }

}

// Forward decl — DrawObject and DrawField call each other (vector elements
// are themselves objects).
bool DrawObject(void* obj, const TypeDescriptor& desc);

namespace
{
    bool DrawVectorImpl(void* obj, const FieldDescriptor& f)
    {
        // The vector field lives at obj+f.offset; for std::vector that's the
        // vector itself, for fixed arrays it's the array base.
        void* vecOrArr = static_cast<unsigned char*>(obj) + f.offset;
        const TypeDescriptor* ed = f.elementDesc;
        if (!ed) return false;

        const bool isArray = (f.vecSize == nullptr);
        std::size_t count = 0;
        std::size_t cap   = 0;

        if (isArray)
        {
            void* countPtr = static_cast<unsigned char*>(obj) + f.countOffset;
            count = f.countIsInt
                ? static_cast<std::size_t>(*static_cast<int*>(countPtr))
                : static_cast<std::size_t>(*static_cast<uint32_t*>(countPtr));
            cap = f.arrayCapacity;
        }
        else
        {
            count = f.vecSize(vecOrArr);
        }

        bool changed = false;
        ImGui::Text("%s (%zu)", f.label, count);
        ImGui::SameLine();
        const bool canAdd = isArray ? (count < cap) : true;
        if (canAdd && ImGui::SmallButton("+"))
        {
            if (isArray)
            {
                // Default-construct the next slot. We can't placement-new a
                // generic T here without knowing the type; rely on the array
                // being default-initialised at construction so simply
                // incrementing count + zero-fill is enough for POD/aggregate
                // element types — which all our reflected element structs are.
                void* slot = f.vecData(vecOrArr, count);
                std::memset(slot, 0, ed->size);
                void* countPtr = static_cast<unsigned char*>(obj) + f.countOffset;
                if (f.countIsInt) ++(*static_cast<int*>(countPtr));
                else              ++(*static_cast<uint32_t*>(countPtr));
                ++count;
            }
            else
            {
                f.vecAdd(vecOrArr);
                ++count;
            }
            changed = true;
        }

        int removeIdx = -1;
        for (std::size_t i = 0; i < count; ++i)
        {
            ImGui::PushID(static_cast<int>(i));
            char hdr[32];
            std::snprintf(hdr, sizeof(hdr), "[%zu]", i);
            if (ImGui::TreeNodeEx(hdr, ImGuiTreeNodeFlags_DefaultOpen))
            {
                void* elem = f.vecData(vecOrArr, i);
                changed |= DrawObject(elem, *ed);
                if (ImGui::SmallButton("Remove")) removeIdx = static_cast<int>(i);
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
        if (removeIdx >= 0)
        {
            if (isArray)
            {
                // Shift down + decrement count. Element type is treated as
                // trivially-copyable (consistent with how the existing editor
                // code mutates Sockets/Capsules arrays).
                for (std::size_t j = (std::size_t)removeIdx; j + 1 < count; ++j)
                {
                    void* dst = f.vecData(vecOrArr, j);
                    void* src = f.vecData(vecOrArr, j + 1);
                    std::memcpy(dst, src, ed->size);
                }
                void* countPtr = static_cast<unsigned char*>(obj) + f.countOffset;
                if (f.countIsInt) --(*static_cast<int*>(countPtr));
                else              --(*static_cast<uint32_t*>(countPtr));
            }
            else
            {
                f.vecRemove(vecOrArr, (std::size_t)removeIdx);
            }
            changed = true;
        }
        return changed;
    }
}

bool DrawField(void* obj, const FieldDescriptor& f)
{
    void* fp = static_cast<unsigned char*>(obj) + f.offset;

    bool changed = false;
    switch (f.kind)
    {
        case FieldKind::Header:
            ImGui::Separator();
            if (f.label && *f.label) ImGui::TextUnformatted(f.label);
            return false;

        case FieldKind::InfoText:
            if (f.label && *f.label) ImGui::TextDisabled("%s", f.label);
            return false;

        case FieldKind::InfoTextFn:
        {
            char buf[256] = {};
            if (f.infoFn) f.infoFn(obj, buf, sizeof(buf));
            if (f.label && *f.label) ImGui::TextDisabled("%s: %s", f.label, buf);
            else                     ImGui::TextDisabled("%s", buf);
            return false;
        }

        case FieldKind::Custom:
            if (f.customDraw) changed = f.customDraw(fp, f);
            break;

        case FieldKind::Vector:
            changed = DrawVectorImpl(obj, f);
            break;

        case FieldKind::F32:    changed = DrawFloat (fp, f); break;
        case FieldKind::F32x2:  changed = DrawFloat2(fp, f); break;
        case FieldKind::F32x3:  changed = DrawFloat3(fp, f); break;
        case FieldKind::F32x4:  changed = DrawFloat4(fp, f); break;
        case FieldKind::I32:    changed = DrawInt   (fp, f); break;
        case FieldKind::U32:    changed = DrawUInt  (fp, f); break;
        case FieldKind::Bool:   changed = DrawBool  (fp, f); break;
        case FieldKind::Enum:   changed = DrawEnum  (fp, f); break;
        case FieldKind::String: changed = DrawString(fp, f); break;
        default: break;
    }

    MaybeTooltip(f.tooltip);
    return changed;
}

bool DrawObject(void* obj, const TypeDescriptor& desc)
{
    bool anyChanged = false;
    ImGui::PushID(desc.name);

    // Stack-style state for nested GroupBegin/CollapseBegin.
    // hideDepth > 0 means we're inside a hidden block (matched at GroupEnd /
    // CollapseEnd at the same depth). Tracks both Group + Collapse so they
    // nest cleanly.
    int hideDepth = 0;

    for (const FieldDescriptor& f : desc.fields)
    {
        if (f.kind == FieldKind::GroupBegin)
        {
            const bool show = hideDepth == 0 && (!f.visible || f.visible(obj));
            if (!show) ++hideDepth;
            else if (hideDepth > 0) ++hideDepth;  // keep counting nested begins
            continue;
        }
        if (f.kind == FieldKind::GroupEnd)
        {
            if (hideDepth > 0) --hideDepth;
            continue;
        }
        if (f.kind == FieldKind::CollapseBegin)
        {
            if (hideDepth > 0)
            {
                ++hideDepth;
                ImGui::PushID(f.label);
                continue;
            }
            const ImGuiTreeNodeFlags flags = (f.minVal > 0.f)
                ? ImGuiTreeNodeFlags_DefaultOpen : 0;
            const bool open = ImGui::CollapsingHeader(f.label, flags);
            // Scope subsequent widget IDs by section label so sibling
            // collapses (e.g. "Spring Bones (Root)" / "(Child)") can reuse
            // the same field labels ("Stiffness", "Damping") without
            // colliding on ImGui's internal ID hash.
            ImGui::PushID(f.label);
            if (!open) ++hideDepth;
            continue;
        }
        if (f.kind == FieldKind::CollapseEnd)
        {
            if (hideDepth > 0) --hideDepth;
            ImGui::PopID();
            continue;
        }

        if (hideDepth > 0) continue;

        // Per-field visibility (separate from groups — a single field can be
        // gated without opening a group).
        if (f.visible && !f.visible(obj)) continue;

        anyChanged |= DrawField(obj, f);
    }

    ImGui::PopID();
    return anyChanged;
}

} // namespace Reflect
