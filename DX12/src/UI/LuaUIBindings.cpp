#include "UI/LuaUIBindings.h"
#include "UI/Widget.h"
#include "UI/BasicWidgets.h"
#include "UI/UIComponents.h"
#include "UI/WidgetRegistry.h"
#include "UI/Tween.h"
#include "ECS/ECS.h"
#include "ECS/HierarchyComponents.h"   // LocalTransform / GlobalTransform
#include "ECS/FollowComponents.h"      // FollowEntityComponent
#include "System/Log.h"

#include <DirectXMath.h>

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

#include <unordered_map>
#include <memory>

namespace UI
{
    // Tracks widgets created from Lua that haven't yet been parented or
    // mounted as a root. Owns the unique_ptr until the user transfers
    // ownership via AddChild / MountRoot.
    struct LuaUIState
    {
        World* world = nullptr;
        std::unordered_map<uint32_t, std::unique_ptr<Widget>> orphans;

        // Move @p w into the orphan pool, register it with WidgetRegistry,
        // return the resulting WidgetHandle.
        WidgetHandle Adopt(std::unique_ptr<Widget> w)
        {
            Widget* raw = w.get();
            WidgetHandle h = WidgetRegistry::Get().Acquire(raw);
            orphans.emplace(h.id, std::move(w));
            return h;
        }

        // Detach @p h from the orphan pool (returns null if it was already
        // adopted by a parent / mounted as root).
        std::unique_ptr<Widget> Take(WidgetHandle h)
        {
            auto it = orphans.find(h.id);
            if (it == orphans.end()) return nullptr;
            std::unique_ptr<Widget> w = std::move(it->second);
            orphans.erase(it);
            return w;
        }
    };

    static LuaUIState& State()
    {
        static LuaUIState s;
        return s;
    }

    // ---- Helpers — read common config-table fields ---------------------------

    static Vec2 ReadVec2(const sol::object& o, Vec2 fallback = { 0, 0 })
    {
        if (!o.is<sol::table>()) return fallback;
        sol::table t = o.as<sol::table>();
        return {
            t.get_or(1, fallback.x),
            t.get_or(2, fallback.y),
        };
    }

    static Color32 ReadColor(const sol::object& o, Color32 fallback = Color32::White())
    {
        if (o.is<uint32_t>()) return Color32(o.as<uint32_t>());
        if (o.is<int>())      return Color32(static_cast<uint32_t>(o.as<int>()));
        return fallback;
    }

    static Anchor ReadAnchor(const sol::object& o, Anchor fallback = Anchor::TopLeft)
    {
        if (!o.is<std::string>()) return fallback;
        const std::string s = o.as<std::string>();
        if (s == "top-left")     return Anchor::TopLeft;
        if (s == "top")          return Anchor::Top;
        if (s == "top-right")    return Anchor::TopRight;
        if (s == "left")         return Anchor::Left;
        if (s == "center")       return Anchor::Center;
        if (s == "right")        return Anchor::Right;
        if (s == "bottom-left")  return Anchor::BottomLeft;
        if (s == "bottom")       return Anchor::Bottom;
        if (s == "bottom-right") return Anchor::BottomRight;
        if (s == "stretch")      return Anchor::Stretch;
        return fallback;
    }

    static void ApplyCommonSpec(Widget& w, sol::table& cfg)
    {
        w.spec.anchor = ReadAnchor(cfg["anchor"]);
        if (cfg["offset"].valid()) w.spec.offset = ReadVec2(cfg["offset"]);
        if (cfg["size"].valid())   w.spec.size   = ReadVec2(cfg["size"], { -1, -1 });
        if (cfg["pivot"].valid())  w.spec.pivot  = ReadVec2(cfg["pivot"]);
        if (cfg["minSize"].valid()) w.spec.minSize = ReadVec2(cfg["minSize"]);
        if (cfg["maxSize"].valid()) w.spec.maxSize = ReadVec2(cfg["maxSize"], { 1e6f, 1e6f });
        if (cfg["flexGrow"].valid())   w.spec.flexGrow   = cfg.get_or("flexGrow", 0.f);
        if (cfg["flexShrink"].valid()) w.spec.flexShrink = cfg.get_or("flexShrink", 1.f);
        if (cfg["visible"].valid())    w.visible = cfg.get_or("visible", true);
    }

    // Parent attachment: if cfg.parent is a WidgetHandle and the child is
    // still in the orphan pool, transfer ownership to the parent.
    static void MaybeReparent(WidgetHandle childH, sol::table& cfg)
    {
        sol::object p = cfg["parent"];
        if (!p.valid() || !p.is<WidgetHandle>()) return;
        WidgetHandle parentH = p.as<WidgetHandle>();
        Widget* parent = WidgetRegistry::Get().Get(parentH);
        if (!parent) return;
        std::unique_ptr<Widget> child = State().Take(childH);
        if (child) parent->AddChild(std::move(child));
    }

    // ---- Constructors --------------------------------------------------------

    static WidgetHandle MakeCanvas(sol::table cfg)
    {
        auto w = std::make_unique<CanvasWidget>();
        w->spec.anchor = Anchor::TopLeft;
        if (cfg["size"].valid()) w->spec.size = ReadVec2(cfg["size"], { -1, -1 });
        ApplyCommonSpec(*w, cfg);
        WidgetHandle h = State().Adopt(std::move(w));
        MaybeReparent(h, cfg);
        return h;
    }

    static WidgetHandle MakeFlex(sol::table cfg)
    {
        auto w = std::make_unique<FlexWidget>();
        ApplyCommonSpec(*w, cfg);
        const std::string dir = cfg.get_or("direction", std::string("row"));
        w->direction = (dir == "column") ? FlexDirection::Column : FlexDirection::Row;
        const std::string just = cfg.get_or("justify", std::string("start"));
        if      (just == "center")        w->justify = FlexJustify::Center;
        else if (just == "end")           w->justify = FlexJustify::End;
        else if (just == "space-between") w->justify = FlexJustify::SpaceBetween;
        else if (just == "space-around")  w->justify = FlexJustify::SpaceAround;
        else                              w->justify = FlexJustify::Start;
        const std::string align = cfg.get_or("align", std::string("start"));
        if      (align == "center")  w->align = FlexAlign::Center;
        else if (align == "end")     w->align = FlexAlign::End;
        else if (align == "stretch") w->align = FlexAlign::Stretch;
        else                         w->align = FlexAlign::Start;
        w->spacing = cfg.get_or("spacing", 0.f);
        if (cfg["padding"].valid()) w->padding = ReadVec2(cfg["padding"]);
        WidgetHandle h = State().Adopt(std::move(w));
        MaybeReparent(h, cfg);
        return h;
    }

    static WidgetHandle MakeText(sol::table cfg)
    {
        auto w = std::make_unique<TextWidget>();
        ApplyCommonSpec(*w, cfg);
        w->text  = cfg.get_or("text", std::string());
        w->color = ReadColor(cfg["color"]);
        WidgetHandle h = State().Adopt(std::move(w));
        MaybeReparent(h, cfg);
        return h;
    }

    static WidgetHandle MakeImage(sol::table cfg)
    {
        auto w = std::make_unique<ImageWidget>();
        ApplyCommonSpec(*w, cfg);
        // texture is bound by C++ side via SetTexture method (Lua scripts
        // don't construct GPU SRV handles directly).
        w->tint = ReadColor(cfg["tint"]);
        if (cfg["uv0"].valid()) w->uv0 = ReadVec2(cfg["uv0"]);
        if (cfg["uv1"].valid()) w->uv1 = ReadVec2(cfg["uv1"], { 1, 1 });
        WidgetHandle h = State().Adopt(std::move(w));
        MaybeReparent(h, cfg);
        return h;
    }

    static WidgetHandle MakeButton(sol::table cfg)
    {
        auto w = std::make_unique<ButtonWidget>();
        ApplyCommonSpec(*w, cfg);
        w->text          = cfg.get_or("text", std::string());
        if (cfg["bgColor"].valid())      w->bgColor      = ReadColor(cfg["bgColor"], w->bgColor);
        if (cfg["bgColorHover"].valid()) w->bgColorHover = ReadColor(cfg["bgColorHover"], w->bgColorHover);
        if (cfg["bgColorPress"].valid()) w->bgColorPress = ReadColor(cfg["bgColorPress"], w->bgColorPress);
        if (cfg["borderColor"].valid())  w->borderColor  = ReadColor(cfg["borderColor"], w->borderColor);
        if (cfg["textColor"].valid())    w->textColor    = ReadColor(cfg["textColor"], w->textColor);
        if (cfg["borderThick"].valid())  w->borderThick  = cfg.get_or("borderThick", 1.f);
        if (cfg["cornerRadius"].valid()) w->cornerRadius = cfg.get_or("cornerRadius", 4.f);

        sol::object onClick = cfg["onClick"];
        if (onClick.is<sol::function>())
        {
            sol::function fn = onClick.as<sol::function>();
            // Capture by value so the reference outlives Lua's local 'cfg'.
            w->onClick = [fn]() {
                sol::protected_function pf = fn;
                auto r = pf();
                if (!r.valid())
                {
                    sol::error err = r;
                    LOG_ERROR("Lua UI onClick: %s", err.what());
                }
            };
        }

        WidgetHandle h = State().Adopt(std::move(w));
        MaybeReparent(h, cfg);
        return h;
    }

    static WidgetHandle MakeInputField(sol::table cfg)
    {
        auto w = std::make_unique<InputFieldWidget>();
        ApplyCommonSpec(*w, cfg);
        w->text        = cfg.get_or("text",        std::string());
        w->placeholder = cfg.get_or("placeholder", std::string());
        if (cfg["bgColor"].valid())          w->bgColor          = ReadColor(cfg["bgColor"], w->bgColor);
        if (cfg["bgColorFocused"].valid())   w->bgColorFocused   = ReadColor(cfg["bgColorFocused"], w->bgColorFocused);
        if (cfg["borderColor"].valid())      w->borderColor      = ReadColor(cfg["borderColor"], w->borderColor);
        if (cfg["borderColorFocus"].valid()) w->borderColorFocus = ReadColor(cfg["borderColorFocus"], w->borderColorFocus);
        if (cfg["textColor"].valid())        w->textColor        = ReadColor(cfg["textColor"], w->textColor);
        if (cfg["maxLen"].valid())           w->maxLen           = static_cast<size_t>(cfg.get_or("maxLen", 256.f));

        sol::object onChange = cfg["onChange"];
        if (onChange.is<sol::function>())
        {
            sol::function fn = onChange.as<sol::function>();
            w->onChange = [fn](const std::string& t) {
                sol::protected_function pf = fn;
                auto r = pf(t);
                if (!r.valid()) { sol::error err = r; LOG_ERROR("Lua UI onChange: %s", err.what()); }
            };
        }
        sol::object onSubmit = cfg["onSubmit"];
        if (onSubmit.is<sol::function>())
        {
            sol::function fn = onSubmit.as<sol::function>();
            w->onSubmit = [fn](const std::string& t) {
                sol::protected_function pf = fn;
                auto r = pf(t);
                if (!r.valid()) { sol::error err = r; LOG_ERROR("Lua UI onSubmit: %s", err.what()); }
            };
        }

        WidgetHandle h = State().Adopt(std::move(w));
        MaybeReparent(h, cfg);
        return h;
    }

    static WidgetHandle MakeProgressBar(sol::table cfg)
    {
        auto w = std::make_unique<ProgressBarWidget>();
        ApplyCommonSpec(*w, cfg);
        w->value = cfg.get_or("value", 0.5f);
        if (cfg["fillColor"].valid())       w->fillColor       = ReadColor(cfg["fillColor"], w->fillColor);
        if (cfg["backgroundColor"].valid()) w->backgroundColor = ReadColor(cfg["backgroundColor"], w->backgroundColor);
        if (cfg["borderColor"].valid())     w->borderColor     = ReadColor(cfg["borderColor"], w->borderColor);
        if (cfg["borderThick"].valid())     w->borderThick     = cfg.get_or("borderThick", 1.f);
        WidgetHandle h = State().Adopt(std::move(w));
        MaybeReparent(h, cfg);
        return h;
    }

    // ---- WidgetHandle method dispatch (Lua-visible) -------------------------

    static void Handle_SetVisible(WidgetHandle& h, bool v)
    {
        if (Widget* w = WidgetRegistry::Get().Get(h)) w->visible = v;
    }
    static void Handle_SetPosition(WidgetHandle& h, float x, float y)
    {
        if (Widget* w = WidgetRegistry::Get().Get(h))
        { w->spec.offset = { x, y }; w->MarkLayoutDirty(); }
    }
    static void Handle_SetSize(WidgetHandle& h, float w_, float ht)
    {
        if (Widget* w = WidgetRegistry::Get().Get(h))
        { w->spec.size = { w_, ht }; w->MarkLayoutDirty(); }
    }
    static void Handle_SetText(WidgetHandle& h, const std::string& t)
    {
        Widget* w = WidgetRegistry::Get().Get(h);
        if (!w) return;
        if (auto* tw = dynamic_cast<TextWidget*>(w))       { tw->SetText(t); return; }
        if (auto* bw = dynamic_cast<ButtonWidget*>(w))     { bw->text = t; bw->MarkVisualDirty(); return; }
        if (auto* iw = dynamic_cast<InputFieldWidget*>(w)) { iw->text = t; iw->MarkVisualDirty(); return; }
    }
    static std::string Handle_GetText(WidgetHandle& h)
    {
        Widget* w = WidgetRegistry::Get().Get(h);
        if (!w) return {};
        if (auto* tw = dynamic_cast<TextWidget*>(w))       return tw->text;
        if (auto* bw = dynamic_cast<ButtonWidget*>(w))     return bw->text;
        if (auto* iw = dynamic_cast<InputFieldWidget*>(w)) return iw->text;
        return {};
    }
    static void Handle_SetValue(WidgetHandle& h, float v)
    {
        Widget* w = WidgetRegistry::Get().Get(h);
        if (!w) return;
        if (auto* pb = dynamic_cast<ProgressBarWidget*>(w)) pb->SetValue(v);
    }
    static void Handle_SetTextureSrv(WidgetHandle& h, uint64_t srv)
    {
        Widget* w = WidgetRegistry::Get().Get(h);
        if (!w) return;
        if (auto* iw = dynamic_cast<ImageWidget*>(w))
        { iw->texture.srvGpuHandle = srv; iw->MarkVisualDirty(); }
    }
    static void Handle_AddChild(WidgetHandle& parentH, WidgetHandle childH)
    {
        Widget* parent = WidgetRegistry::Get().Get(parentH);
        if (!parent) return;
        std::unique_ptr<Widget> child = State().Take(childH);
        if (child) parent->AddChild(std::move(child));
    }
    static bool Handle_IsValid(WidgetHandle& h)
    {
        return WidgetRegistry::Get().Get(h) != nullptr;
    }

    // ---- Tween Lua API ------------------------------------------------------
    static Easing ParseEasing(const std::string& s)
    {
        if (s == "linear")        return Easing::Linear;
        if (s == "in-quad")       return Easing::InQuad;
        if (s == "out-quad")      return Easing::OutQuad;
        if (s == "in-out-quad")   return Easing::InOutQuad;
        if (s == "in-cubic")      return Easing::InCubic;
        if (s == "out-cubic")     return Easing::OutCubic;
        if (s == "in-out-cubic")  return Easing::InOutCubic;
        if (s == "out-back")      return Easing::OutBack;
        if (s == "out-elastic")   return Easing::OutElastic;
        if (s == "out-bounce")    return Easing::OutBounce;
        return Easing::OutCubic;
    }

    // ui.TweenAlpha(handle, fromA, toA, duration, easing) — fades a widget's
    // text/fill alpha. Walks the supported widgets and animates their colour
    // alpha channel.
    static void TweenAlpha(WidgetHandle h, float fromA, float toA,
                           float duration, sol::optional<std::string> easingS)
    {
        Widget* w = WidgetRegistry::Get().Get(h);
        if (!w) return;
        const Easing easing = ParseEasing(easingS.value_or("out-cubic"));
        // Pick the most "obvious" colour for each widget kind.
        Color32* dst = nullptr;
        if (auto* tw = dynamic_cast<TextWidget*>(w))            dst = &tw->color;
        else if (auto* iw = dynamic_cast<ImageWidget*>(w))       dst = &iw->tint;
        else if (auto* bw = dynamic_cast<ButtonWidget*>(w))      dst = &bw->bgColor;
        else if (auto* pb = dynamic_cast<ProgressBarWidget*>(w)) dst = &pb->fillColor;
        if (!dst) return;
        const uint8_t r = static_cast<uint8_t>(dst->rgba       & 0xFFu);
        const uint8_t g = static_cast<uint8_t>((dst->rgba >> 8) & 0xFFu);
        const uint8_t b = static_cast<uint8_t>((dst->rgba >> 16) & 0xFFu);
        const Color32 from(r, g, b, static_cast<uint8_t>(std::clamp(fromA, 0.f, 1.f) * 255.f));
        const Color32 to  (r, g, b, static_cast<uint8_t>(std::clamp(toA,   0.f, 1.f) * 255.f));
        TweenSystem::Get().TweenColor(dst, from, to, duration, easing);
    }

    // ui.TweenOffset(handle, fromX, fromY, toX, toY, duration, easing) — slides.
    static void TweenOffset(WidgetHandle h,
                             float fx, float fy, float tx, float ty,
                             float duration, sol::optional<std::string> easingS)
    {
        Widget* w = WidgetRegistry::Get().Get(h);
        if (!w) return;
        const Easing easing = ParseEasing(easingS.value_or("out-cubic"));
        TweenSystem::Get().TweenVec2(&w->spec.offset, { fx, fy }, { tx, ty },
                                      duration, easing);
        w->MarkLayoutDirty();
    }

    // ui.TweenValue(progressBarHandle, fromV, toV, duration, easing).
    static void TweenValue(WidgetHandle h, float fromV, float toV,
                            float duration, sol::optional<std::string> easingS)
    {
        Widget* w = WidgetRegistry::Get().Get(h);
        if (!w) return;
        auto* pb = dynamic_cast<ProgressBarWidget*>(w);
        if (!pb) return;
        const Easing easing = ParseEasing(easingS.value_or("linear"));
        Tween t{};
        t.storage   = &pb->value;
        t.guardHandle = h;
        t.fromValue = fromV; t.toValue = toV;
        t.duration  = duration; t.easing = easing;
        TweenSystem::Get().AddTween(std::move(t));
    }

    // ---- MountRoot — creates a SCREEN-SPACE Entity with UIRootComponent ----

    static uint32_t MountRoot(const std::string& name, WidgetHandle rootH, int sortOrder)
    {
        World* world = State().world;
        if (!world) { LOG_ERROR("ui.MountRoot: world not bound"); return 0; }
        std::unique_ptr<Widget> rootW = State().Take(rootH);
        if (!rootW) { LOG_WARNING("ui.MountRoot: root widget already mounted/parented"); return 0; }

        Entity e = world->CreateEntity();
        UIRootComponent c;
        c.name      = name;
        c.sortOrder = sortOrder;
        c.root      = std::move(rootW);
        world->AddComponent<UIRootComponent>(e, std::move(c));
        world->AddComponent<UIScreenSpaceComponent>(e, {});
        return static_cast<uint32_t>(e);
    }

    // ---- MountWorldRoot — DEPRECATED ----------------------------------------
    // World-space UI is no longer driven by widget trees; it lives in
    // UI/WorldSpaceUI.h as flat ECS components (WorldSpaceUIComponent +
    // WorldUIText/WorldUIBar/WorldUIImage/DamageNumber) and is rendered by
    // WorldUIBillboardPass.  Lua callers should compose those components
    // directly (or wait for dedicated bindings).  This stub keeps the
    // function name compiling — it just mounts the widget tree as a regular
    // screen-space root and logs a deprecation warning.
    static uint32_t MountWorldRoot(const std::string& name,
                                   WidgetHandle rootH,
                                   uint32_t /*targetEntity*/,
                                   float /*ox*/, float /*oy*/, float /*oz*/,
                                   sol::optional<sol::table> /*opts*/)
    {
        LOG_WARNING("ui.MountWorldRoot is deprecated — world-space UI moved "
                    "to flat WorldSpaceUI components. Falling back to "
                    "screen-space MountRoot for '%s'.", name.c_str());
        World* world = State().world;
        if (!world) { LOG_ERROR("ui.MountWorldRoot: world not bound"); return 0; }
        std::unique_ptr<Widget> rootW = State().Take(rootH);
        if (!rootW) { LOG_WARNING("ui.MountWorldRoot: root widget already mounted/parented"); return 0; }

        Entity e = world->CreateEntity();
        UIRootComponent c;
        c.name      = name;
        c.sortOrder = 0;
        c.root      = std::move(rootW);
        world->AddComponent<UIRootComponent>(e, std::move(c));
        world->AddComponent<UIScreenSpaceComponent>(e, {});
        return static_cast<uint32_t>(e);
    }

    // ---- AttachToEntity — DEPRECATED ----------------------------------------
    // World-space wiring no longer goes through widget-tree UI.  Use the
    // flat WorldSpaceUI components (UI/WorldSpaceUI.h) instead.
    static void AttachToEntity(uint32_t uiEntity, uint32_t /*targetEntity*/,
                                float /*ox*/, float /*oy*/, float /*oz*/,
                                sol::optional<sol::table> /*opts*/)
    {
        LOG_WARNING("ui.AttachToEntity is deprecated — world-space UI moved "
                    "to flat WorldSpaceUI components (entity %u left as-is).",
                    uiEntity);
    }

    // ---- Top-level binding ---------------------------------------------------

    void RegisterLuaUIBindings(sol::state& lua, World& world)
    {
        State().world = &world;

        // WidgetHandle usertype — methods exposed via `:` syntax in Lua.
        sol::usertype<WidgetHandle> wh = lua.new_usertype<WidgetHandle>(
            "Widget",
            sol::constructors<WidgetHandle()>());
        wh["IsValid"]       = &Handle_IsValid;
        wh["SetVisible"]    = &Handle_SetVisible;
        wh["SetPosition"]   = &Handle_SetPosition;
        wh["SetSize"]       = &Handle_SetSize;
        wh["SetText"]       = &Handle_SetText;
        wh["GetText"]       = &Handle_GetText;
        wh["SetValue"]      = &Handle_SetValue;
        wh["SetTextureSrv"] = &Handle_SetTextureSrv;
        wh["AddChild"]      = &Handle_AddChild;

        sol::table ui = lua.create_named_table("ui");
        ui["Canvas"]      = &MakeCanvas;
        ui["Flex"]        = &MakeFlex;
        ui["Text"]        = &MakeText;
        ui["Image"]       = &MakeImage;
        ui["Button"]      = &MakeButton;
        ui["InputField"]  = &MakeInputField;
        ui["ProgressBar"] = &MakeProgressBar;
        ui["MountRoot"]      = &MountRoot;
        ui["MountWorldRoot"] = &MountWorldRoot;
        ui["AttachToEntity"] = &AttachToEntity;
        ui["TweenAlpha"]  = &TweenAlpha;
        ui["TweenOffset"] = &TweenOffset;
        ui["TweenValue"]  = &TweenValue;

        LOG_SUCCESS("LuaUIBindings: ui table installed");
    }

} // namespace UI
