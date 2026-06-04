#include "Editor/EditorLayer.h"
#include "Editor/EntityRefPicker.h"
#include "Editor/FontAwesomeIcons.h"
#include "Reflection/ComponentReflection.h" // Reflect::Descriptor<T> specializations for RegisterReflectedComponent
#include "ECS/GuidComponent.h"
#include "ECS/GuidRegistry.h"
#include "AI/AIComponents.h"
#include "AI/AISystem.h"   // m_aiSys->AcquireTree from the AIComponent inspector
#include "AI/BTAsset.h"
#include "AI/BTNode.h"
#include "ECS/Components.h"           // Transform, MaterialComponent
#include "ECS/FollowComponents.h"
#include "UI/UIComponents.h"
#include "UI/WorldSpaceUI.h"
#include "UI/BasicWidgets.h"
#include "ECS/TerrainComponent.h"     // TerrainComponent + TerrainLayer
#include "ECS/BillboardComponent.h"   // BillboardComponent
#include "ECS/ParticleComponent.h"    // ParticleEmitterComponent
#include "ECS/VideoComponent.h"       // VideoComponent (inspector postDraw)
#include "ECS/VideoHelpers.h"         // Video::Play/Pause/Stop/Restart/Seek
#include "ECS/TrailComponent.h"       // TrailComponent
#include "ECS/BeamComponent.h"        // BeamComponent (procedural-tube heavy beam)
#include "Graphics/TracerSystem.h"    // Renderer::GetTracerSystem() for Debug menu test spawn
#include "ECS/ReflectionProbeComponent.h"
#include "ECS/TagComponent.h"
#include "Scene/MeshSpawner.h"
#include "Resource/SceneSerializer.h"  // SaveScene / LoadScene
#include "Resource/PostProcessConfig.h" // Save/Load/Apply post-process config (.ippc)
#include "ECS/HierarchyComponents.h"  // LocalTransform, GlobalTransform, Parent, Children, etc.
#include "ECS/PhysicsComponents.h"    // RigidBodyComponent, ColliderComponent
#include "Tools/CollisionMeshBaker.h" // Bake render meshes into combined .meshlib
#include "Resource/MeshLibrary.h"     // Load baked meshlib for in-world preview swap
#include "System/TaskSystem.h"        // Worker count for the bake popup
#include "Physics/PhysicsSystem.h"    // InvalidateMeshShapeCache after re-baking colliders
#include "Nav/NavMeshSystem.h"        // Build NavMesh from world colliders
#include "Nav/NavComponents.h"        // NavAgentComponent (reflection-registered)
#include "ECS/AIIntentComponent.h"    // AIIntentComponent (reflection-registered)
#include "ECS/PerceptionComponent.h"  // PerceptionComponent (reflection-registered)
#include "Scene/Ray.h"
#include "Scene/SceneInstanceLoader.h"
#include "Graphics/Renderer.h"
#include "Graphics/ShaderReflection.h"
#include "ECS/MaterialReflectionSync.h"
#include "ECS/MaterialSchema.h"
#include "Graphics/GraphicsDX12.h"
#include "RenderGraph/RenderPass/ToneMapPass.h"
#include "RenderGraph/RenderPass/SkyIBLPass.h"
#include "RenderGraph/RenderPass/VolumetricFogPass.h"
#include "RenderGraph/RenderPass/AutoExposurePass.h"
#include "RenderGraph/RenderPass/TAAPass.h"
#include "RenderGraph/RenderPass/FXAAPass.h"
#include "RenderGraph/RenderPass/OutlinePass.h"
#include "RenderGraph/RenderPass/XeGTAOPass.h"
#include "RenderGraph/RenderPass/SSRPass.h"
#include "RenderGraph/RenderPass/CASPass.h"
#include "PostProcess/PostProcessStack.h"
#include "PostProcess/ParameterStore.h"
#include "PostProcess/VolumeSystem.h"
#include "PostProcess/ScriptedOverride.h"
#include "ECS/VolumeComponent.h"
#include "ECS/IKSystem.h"
#include "RenderGraph/RenderPass/DebugWirePass.h"
#include "RenderGraph/RenderPass/DDGIProbeDebugPass.h"
#include "RenderGraph/RenderPass/DecalPass.h"
#include "RenderGraph/RenderPass/SpotShadowPass.h"
#include "System/Log.h"
#include "Resource/TextureSystem.h"
#include "Resource/ResourceManager.h"
#include "Resource/AssetManager.h"
#include "Resource/MeshImporter.h"
#include "Resource/MaterialImporter.h"
#include "Resource/MaterialSerializer.h"
#include "Resource/DecalMaterialSerializer.h"
#include "Resource/PrefabSerializer.h"
#include "Resource/ComponentSerializers.h"
#include "Resource/TextureImporter.h"
#include "Resource/ShaderImporter.h"
#include "Resource/SceneImporter.h"
#include "Resource/PmxImporter.h"
#include "Resource/VrmImporter.h"
#include "Resource/AnimationImporter.h"
#include "Resource/AnimationClipSystem.h"
#include "Resource/AnimationResource.h"
#include "Resource/AnimationSerializer.h"
#include "Resource/VmdImporter.h"
#include "Audio/AudioImporter.h"
#include "ECS/AnimationComponents.h"
#include "ECS/NotifyTypes.h"
#include "Editor/NotifyEditorCategoryDrawers.h"
#include "ECS/FollowComponents.h"
#include "ECS/SkyboxComponent.h"
#include "ECS/ReflectionProbeComponent.h"
#include "ECS/DDGIComponents.h"
#include "Physics/ChainPhysicsSystem.h"
#include "Scripting/ScriptComponent.h"
#include "Editor/DebugDrawSystem.h"
#include "Scripting/ScriptSystem.h"     // exposed-var schema + live push
#include "Math/MathUtils.h"
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/GraphicsDX12.h"
#include "Graphics/GPUProfiler.h"
#include "System/Window.h"
#include "imgui/imgui_impl_win32.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>   // ShellExecuteA — used by Build menu to spawn packager
#pragma comment(lib, "shell32.lib")

// Forward-decl for ImGui_ImplWin32_WndProcHandler (used below as Window::WndMsgHook).
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"
#include "imgui/ImGuizmo/ImGuizmo.h"
#include <cassert>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <map>          // sorted category buckets in Add Component popup
#include <windows.h>
#include <commdlg.h>
#pragma comment(lib, "comdlg32.lib")

using namespace DirectX;

namespace
{
    const char* kWindowHierarchy = "Hierarchy";
    const char* kWindowViewport  = "Viewport";
    const char* kWindowInspector = "Inspector";
    const char* kWindowResource  = "Resource";

    // Set by Tools > Bake Collision Meshes... menu item; consumed by
    // OnUIRender after EndMenuBar to open the modal popup at the correct
    // ID stack level (modals can't be opened from inside BeginMenuBar).
    bool g_bakeCollisionOpenRequest = false;

    // Same pattern for Tools > Bake NavMesh...
    bool g_bakeNavMeshOpenRequest = false;

    // Fwd-decl: definition lives in the entity-picker namespace below; Material inspector calls it before that.
    bool DrawTextureSlotWidget(const char* label, MaterialComponent::TextureMap& slot);

    // Try common locations for Font Awesome (fa-solid-900.ttf)
    const char* kFontAwesomePaths[] =
    {
        "asset/font/fontawesome/Font Awesome 7 Free-Solid-900.otf",
    };

    // Open a Win32 multi-select file dialog; returns all chosen paths (empty on cancel).
    std::vector<std::string> OpenFileDialogMulti(const char* filter, const char* initDir)
    {
        // 32 KB buffer supports ~200+ typical file paths.
        static char buf[32768];
        buf[0] = '\0';
        OPENFILENAMEA ofn{};
        ofn.lStructSize     = sizeof(ofn);
        ofn.lpstrFilter     = filter;
        ofn.lpstrFile       = buf;
        ofn.nMaxFile        = sizeof(buf);
        ofn.lpstrInitialDir = initDir;
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR
                  | OFN_ALLOWMULTISELECT | OFN_EXPLORER;
        if (!GetOpenFileNameA(&ofn))
            return {};

        // Parse: single file = "full\path\0", multi = "dir\0file1\0file2\0\0".
        std::vector<std::string> paths;
        const char* p = buf;
        std::string dir = p;
        p += dir.size() + 1;
        if (*p == '\0')
        {
            paths.push_back(dir);
        }
        else
        {
            while (*p)
            {
                paths.push_back(dir + "\\" + p);
                p += strlen(p) + 1;
            }
        }
        return paths;
    }

    // Single-file save dialog; returns empty string on cancel.
    std::string SaveFileDialog(const char* filter, const char* defaultExt, const char* initDir)
    {
        static char buf[MAX_PATH];
        buf[0] = '\0';
        OPENFILENAMEA ofn{};
        ofn.lStructSize     = sizeof(ofn);
        ofn.lpstrFilter     = filter;
        ofn.lpstrFile       = buf;
        ofn.nMaxFile        = sizeof(buf);
        ofn.lpstrInitialDir = initDir;
        ofn.lpstrDefExt     = defaultExt;
        ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        return GetSaveFileNameA(&ofn) ? std::string(buf) : std::string{};
    }

    // Single-file open dialog; returns empty string on cancel.
    std::string OpenFileDialog(const char* filter, const char* initDir)
    {
        static char buf[MAX_PATH];
        buf[0] = '\0';
        OPENFILENAMEA ofn{};
        ofn.lStructSize     = sizeof(ofn);
        ofn.lpstrFilter     = filter;
        ofn.lpstrFile       = buf;
        ofn.nMaxFile        = sizeof(buf);
        ofn.lpstrInitialDir = initDir;
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        return GetOpenFileNameA(&ofn) ? std::string(buf) : std::string{};
    }

    // Read a file from disk into a byte vector.
    std::vector<uint8_t> ReadFileFully(const std::string& path)
    {
        std::ifstream f(path, std::ios::binary | std::ios::ate);
        if (!f) return {};
        const auto sz = f.tellg();
        if (sz <= 0) return {};
        f.seekg(0);
        std::vector<uint8_t> buf(static_cast<size_t>(sz));
        f.read(reinterpret_cast<char*>(buf.data()), sz);
        return buf;
    }

    // Write a byte vector to disk, creating directories as needed.
    bool WriteFileFully(const std::string& path, const std::vector<uint8_t>& data)
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        fs::create_directories(fs::path(path).parent_path(), ec);
        std::ofstream f(path, std::ios::binary);
        if (!f) return false;
        f.write(reinterpret_cast<const char*>(data.data()),
                static_cast<std::streamsize>(data.size()));
        return f.good();
    }

    // ---- Hierarchy panel state (set by context-menu / rename UI, consumed next frame) ----
    Entity s_renamingEntity      = NullEntity;
    char   s_renameBuffer[256]   = {};
    Entity s_pendingDelete       = NullEntity;
    Entity s_pendingCreateChild  = NullEntity;
    Entity s_pendingDuplicate    = NullEntity;
    Entity s_pendingPrefabSave   = NullEntity;
    Entity s_pendingWrapVisual   = NullEntity;       // context-menu → wrap visual into child
    // Drag-drop reparent. child == NullEntity means "no pending"; newParent
    // == NullEntity means "drop into root (unparent)".
    struct PendingReparent { Entity child = NullEntity; Entity newParent = NullEntity; };
    PendingReparent s_pendingReparent;

    // ---- Dynamic hierarchy view (DesignMd/ecs_flat_hierarchical_editor_design.md §3) ----
    //
    // Runtime ECS is flat — relations are expressed via Parent/Children AND
    // FollowSocketComponent / FollowEntityComponent. The hierarchy panel
    // dynamically builds a tree each frame from these references so that a
    // satellite entity (HPBar, NameTag, weapon) following its target appears
    // visually under that target, even though no Parent component links them.
    //
    // Priority when an entity has multiple connectors (§3.3):
    //   Parent  >  FollowSocket  >  FollowEntity
    // First non-null wins; the others are treated as siblings of the target.
    struct HierarchyMap
    {
        std::unordered_map<Entity, Entity>              parent;    // post cycle-sever
        std::unordered_map<Entity, std::vector<Entity>> children;  // logical parent -> sorted kids
        std::unordered_set<Entity>                      cyclic;    // members of a cycle (link severed)
    };

    Entity ResolveFollowTarget(World* world, EntityHandle h)
    {
        if (!world->IsHandleValid(h)) return NullEntity;
        return h.entity;
    }

    Entity LogicalParentOf(World* world, Entity e)
    {
        if (auto* pc = world->GetComponent<Parent>(e); pc && pc->entity != NullEntity)
            if (world->IsAlive(pc->entity)) return pc->entity;

        if (auto* fs = world->GetComponent<FollowSocketComponent>(e))
        {
            const Entity t = ResolveFollowTarget(world, fs->target);
            if (t != NullEntity && t != e && world->IsAlive(t)) return t;
        }

        if (auto* fe = world->GetComponent<FollowEntityComponent>(e))
        {
            const Entity t = ResolveFollowTarget(world, fe->target);
            if (t != NullEntity && t != e && world->IsAlive(t)) return t;
        }

        return NullEntity;
    }

    // Build per-frame hierarchy map. O(N) average — each entity walks up to a
    // root once for cycle detection, but the cycle-walk early-outs at the first
    // revisit so worst case is bounded by the longest follow chain.
    HierarchyMap BuildHierarchyMap(World* world)
    {
        HierarchyMap m;

        for (Entity e : world->GetEntities())
        {
            if (!world->IsAlive(e)) continue;
            m.parent[e] = LogicalParentOf(world, e);
        }

        for (auto& [e, _] : m.parent)
        {
            std::unordered_set<Entity> seen;
            Entity cur = e;
            while (cur != NullEntity)
            {
                if (!seen.insert(cur).second)
                {
                    m.cyclic.insert(e);
                    m.parent[e] = NullEntity;  // sever — cyclic entity becomes a root
                    break;
                }
                auto it = m.parent.find(cur);
                cur = (it != m.parent.end()) ? it->second : NullEntity;
            }
        }

        for (auto& [e, p] : m.parent)
            if (p != NullEntity) m.children[p].push_back(e);
        for (auto& [_, kids] : m.children)
            std::sort(kids.begin(), kids.end());

        return m;
    }

    // Recursively destroy entity + descendants and unlink from parent's Children.
    void DestroyEntityRecursive(World* world, Entity e)
    {
        std::vector<Entity> children;
        if (const Children* ch = world->GetComponent<Children>(e))
            children = ch->entities;

        for (Entity child : children)
            DestroyEntityRecursive(world, child);

        if (const Parent* p = world->GetComponent<Parent>(e); p && p->entity != NullEntity)
        {
            if (Children* pc = world->GetComponent<Children>(p->entity))
            {
                auto& vec = pc->entities;
                vec.erase(std::remove(vec.begin(), vec.end(), e), vec.end());
            }
        }

        world->DestroyEntity(e);
    }

    // Duplicate entity (and descendants) via the ComponentSerializerRegistry round-trip.
    // Hierarchy is rebuilt by the tree walk so serialized child IDs (which point at src) are ignored.
    Entity CloneEntityRecursive(World* world,
                                Resource::AssetManager* assetMgr,
                                Entity src,
                                Entity newParent)
    {
        Entity dst = world->CreateEntity();

        // Top-level clone gets a "(Copy)" suffix; descendants keep their names.
        std::string newName = world->GetName(src);
        if (newParent == NullEntity) newName += " (Copy)";
        world->SetName(dst, newName);

        // Manually copy transform components (not in registry).
        if (const LocalTransform* lt = world->GetComponent<LocalTransform>(src))
            world->AddComponent<LocalTransform>(dst, *lt);
        if (world->HasComponent<GlobalTransform>(src))
            world->AddComponent<GlobalTransform>(dst, GlobalTransform{}); // recomputed by TransformSystem

        // Round-trip every registered component.
        auto& reg = GetComponentRegistry();
        for (const auto& [type, ser] : reg.All())
        {
            if (!ser.has || !ser.has(*world, src)) continue;

            std::ostringstream ss;
            ser.serialize(*world, src, ss);
            const std::string line = ss.str();

            // Format: "  Tag: key=val key=val\n" (mirrors SceneSerializer parser)
            const auto colonPos = line.find(':');
            if (colonPos == std::string::npos) continue;
            size_t start = colonPos + 1;
            if (start < line.size() && line[start] == ' ') ++start;

            KVMap kv;
            std::istringstream tok(line.substr(start));
            std::string t;
            while (tok >> t)
            {
                const auto eq = t.find('=');
                if (eq == std::string::npos) continue;
                kv[t.substr(0, eq)] = t.substr(eq + 1);
            }
            ser.deserialize(*world, dst, kv, assetMgr);
        }

        // Hook into the new parent's Children list (or stay a root if none).
        if (newParent != NullEntity)
        {
            Parent p; p.entity = newParent;
            world->AddComponent<Parent>(dst, p);
            if (Children* pc = world->GetComponent<Children>(newParent))
                pc->entities.push_back(dst);
            else
            {
                Children newCh;
                newCh.entities.push_back(dst);
                world->AddComponent<Children>(newParent, std::move(newCh));
            }
        }

        // Snapshot child list before recursing — world mutates during the loop.
        if (const Children* ch = world->GetComponent<Children>(src))
        {
            const std::vector<Entity> kids = ch->entities;
            for (Entity child : kids)
                CloneEntityRecursive(world, assetMgr, child, dst);
        }
        return dst;
    }

    // Cycle check for drag-drop: dropping `child` onto `newParent` would form
    // a cycle if `child` is itself an ancestor of `newParent`. Walks up the
    // Parent chain with a hard depth cap so a degenerate ECS state doesn't
    // hang the editor.
    bool WouldCreateCycle(World* world, Entity child, Entity newParent)
    {
        Entity cur = newParent;
        for (int hops = 0; hops < 4096 && cur != NullEntity; ++hops)
        {
            if (cur == child) return true;
            const Parent* p = world->GetComponent<Parent>(cur);
            cur = p ? p->entity : NullEntity;
        }
        return false;
    }

    // Move a component value from src → dst. Captures by value first so we
    // never dereference a pointer that AddComponent could relocate via pool
    // growth. Returns true when a move happened.
    template <typename T>
    bool MoveComponentBetween(World* world, Entity src, Entity dst)
    {
        T* p = world->GetComponent<T>(src);
        if (!p) return false;
        T value = *p;
        world->RemoveComponent<T>(src);
        world->AddComponent<T>(dst, std::move(value));
        return true;
    }

    // Reparent `child` under `newParent` (or detach when newParent ==
    // NullEntity) while preserving the child's world transform. The
    // LocalTransform is recomputed so child.GlobalTransform on the next
    // Propagate matches its current world pose: newLT = childGT * inv(newParentGT).
    void ReparentEntity(World* world, Entity child, Entity newParent)
    {
        using namespace DirectX;

        // 1. Recompute LocalTransform to preserve world pose.
        const GlobalTransform* childGT = world->GetComponent<GlobalTransform>(child);
        LocalTransform*        lt      = world->GetComponent<LocalTransform>(child);
        if (childGT && lt)
        {
            XMMATRIX worldM = XMLoadFloat4x4(&childGT->matrix);
            XMMATRIX newLocalM;
            if (newParent == NullEntity)
            {
                newLocalM = worldM; // root — local == world
            }
            else if (const GlobalTransform* pGT =
                         world->GetComponent<GlobalTransform>(newParent))
            {
                XMVECTOR det;
                XMMATRIX pInv = XMMatrixInverse(&det, XMLoadFloat4x4(&pGT->matrix));
                // Degenerate parent matrix (zero-scale, etc.) — fall back to
                // keeping the current LT, which is at least valid even if it
                // visually snaps the child.
                if (XMVectorGetX(det) == 0.0f) newLocalM = XMLoadFloat4x4(&childGT->matrix);
                else                           newLocalM = XMMatrixMultiply(worldM, pInv);
            }
            else
            {
                newLocalM = worldM;
            }
            XMVECTOR t, r, s;
            if (XMMatrixDecompose(&s, &r, &t, newLocalM))
            {
                XMStoreFloat3(&lt->translation, t);
                XMStoreFloat4(&lt->rotation,    r);
                XMStoreFloat3(&lt->scale,       s);
            }
        }

        // 2. Detach from old parent's Children list.
        Parent* oldP = world->GetComponent<Parent>(child);
        const Entity oldParent = oldP ? oldP->entity : NullEntity;
        if (oldParent != NullEntity)
        {
            if (Children* oldCh = world->GetComponent<Children>(oldParent))
            {
                auto& v = oldCh->entities;
                v.erase(std::remove(v.begin(), v.end(), child), v.end());
            }
        }

        // 3. Write new Parent or remove if reparenting to root.
        if (newParent == NullEntity)
        {
            if (oldP) world->RemoveComponent<Parent>(child);
        }
        else
        {
            if (oldP) oldP->entity = newParent;
            else      world->AddComponent<Parent>(child, Parent{ newParent });

            // 4. Insert into new parent's Children list.
            if (Children* newCh = world->GetComponent<Children>(newParent))
            {
                newCh->entities.push_back(child);
            }
            else
            {
                Children c; c.entities.push_back(child);
                world->AddComponent<Children>(newParent, std::move(c));
            }
        }
    }

    // Recursive tree node draw helper for the Hierarchy panel. Walks the
    // logical-children map (Parent/Children + Follow* edges) rather than
    // raw Children component, so entities that follow a target appear under
    // that target visually.
    //
    // Two-click selection: hierarchyHighlight tracks the row that draws the
    // ImGuiTreeNodeFlags_Selected bar. Clicking a row that ISN'T currently
    // highlighted just moves the highlight (Inspector content stays put —
    // the user can now drag this entity onto an Inspector field). Clicking
    // the already-highlighted row promotes it to selectedEntity (which the
    // Inspector reads). See EditorLayer.h for the rationale.
    void DrawEntityTree(World* world, Entity e,
                        Entity& selectedEntity,
                        Entity& hierarchyHighlight,
                        const HierarchyMap& hmap)
    {
        if (!world->IsAlive(e)) return;

        auto kidsIt = hmap.children.find(e);
        const std::vector<Entity>* kids =
            (kidsIt != hmap.children.end()) ? &kidsIt->second : nullptr;
        const bool hasChildren  = kids && !kids->empty();
        const std::string& name = world->GetName(e);

        // Visual tint priority: cycle warning > mesh tint.
        const bool isMesh   = world->HasComponent<MeshLibRef>(e);
        const bool isCyclic = hmap.cyclic.count(e) > 0;
        if (isCyclic)
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.55f, 0.25f, 1.f));
        else if (isMesh)
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.75f, 0.88f, 0.75f, 1.f));

        ImGuiTreeNodeFlags flags =
            ImGuiTreeNodeFlags_OpenOnArrow  |
            ImGuiTreeNodeFlags_SpanAvailWidth;
        if (!hasChildren)  flags |= ImGuiTreeNodeFlags_Leaf;
        // ImGui's Selected bar follows the Hierarchy highlight (the first-
        // click selection). The Inspector content still keys off
        // selectedEntity, which only changes on second-click promote.
        if (hierarchyHighlight == e) flags |= ImGuiTreeNodeFlags_Selected;

        ImGui::PushID(static_cast<int>(e));

        bool open = false;
        if (s_renamingEntity == e)
        {
            // Collapsed/leaf tree node as placeholder, then overlay InputText.
            ImGuiTreeNodeFlags renameFlags = flags | ImGuiTreeNodeFlags_AllowOverlap;
            open = ImGui::TreeNodeEx("##node", renameFlags, "");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
            if (ImGui::InputText("##rename", s_renameBuffer, sizeof(s_renameBuffer),
                                 ImGuiInputTextFlags_EnterReturnsTrue |
                                 ImGuiInputTextFlags_AutoSelectAll))
            {
                if (s_renameBuffer[0] != '\0')
                    world->SetName(e, s_renameBuffer);
                s_renamingEntity = NullEntity;
            }
            else if (!ImGui::IsItemActive() && ImGui::IsItemDeactivated())
            {
                if (s_renameBuffer[0] != '\0')
                    world->SetName(e, s_renameBuffer);
                s_renamingEntity = NullEntity;
            }

            else if (ImGui::IsKeyPressed(ImGuiKey_Escape))
            {
                s_renamingEntity = NullEntity;
            }
        }
        else
        {
            open = ImGui::TreeNodeEx("##node", flags, "%s", name.c_str());
            if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
            {
                if (hierarchyHighlight == e)
                {
                    // Second click on the already-highlighted row —
                    // promote to Inspector. The OnUIRender frame-start
                    // sync will mirror this back into m_lastSeen on the
                    // next frame, so no further state needed here.
                    selectedEntity = e;
                }
                hierarchyHighlight = e;
            }

            // Drag-drop source: the tree node ships its Entity id under the
            // generic "ENTITY" payload type. Two distinct targets currently
            // consume it: another tree node / the root drop zone (treats it
            // as a reparent), and AttachmentRef fields in the Inspector
            // (treats it as a ref bind via Editor::DrawAttachmentRef).
            // Reparent is deferred via s_pendingReparent — mutating
            // Parent/Children during the traversal would invalidate the
            // iteration.
            if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None))
            {
                ImGui::SetDragDropPayload("ENTITY", &e, sizeof(Entity));
                ImGui::Text("Entity \"%s\"", name.c_str());
                ImGui::EndDragDropSource();
            }
            // Drag-drop target: accept onto this entity to make it the new
            // parent. Cycle check prevents dropping a parent into its own
            // descendant.
            if (ImGui::BeginDragDropTarget())
            {
                if (const ImGuiPayload* payload =
                        ImGui::AcceptDragDropPayload("ENTITY"))
                {
                    const Entity dragged = *static_cast<const Entity*>(payload->Data);
                    if (dragged != e && !WouldCreateCycle(world, dragged, e))
                    {
                        s_pendingReparent.child     = dragged;
                        s_pendingReparent.newParent = e;
                    }
                }
                ImGui::EndDragDropTarget();
            }

            // Rename is right-click → context menu → Rename only. Double-
            // click is intentionally NOT bound: it collided with the
            // two-click "first-click select, second-click promote to
            // Inspector" flow (and made it impossible to deliberately
            // re-click an entity to open it in Inspector without entering
            // rename mode by accident).

            // Right-click context menu
            if (ImGui::BeginPopupContextItem("##entity_ctx"))
            {
                selectedEntity = e;
                if (ImGui::MenuItem("Rename"))
                {
                    s_renamingEntity = e;
                    strncpy_s(s_renameBuffer, sizeof(s_renameBuffer), name.c_str(), _TRUNCATE);
                    s_renameBuffer[sizeof(s_renameBuffer) - 1] = '\0';
                }
                if (ImGui::MenuItem("Create Child"))
                    s_pendingCreateChild = e;
                if (ImGui::MenuItem("Wrap Visual as Child",
                                    nullptr, false,
                                    world->HasComponent<MeshHandle>(e)
                                 || world->HasComponent<MeshLibRef>(e)))
                {
                    s_pendingWrapVisual = e;
                }
                if (ImGui::MenuItem("Unparent (drop to root)",
                                    nullptr, false,
                                    world->HasComponent<Parent>(e)))
                {
                    s_pendingReparent.child     = e;
                    s_pendingReparent.newParent = NullEntity;
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Duplicate"))
                    s_pendingDuplicate = e;
                if (ImGui::MenuItem("Save as Prefab"))
                    s_pendingPrefabSave = e;
                ImGui::Separator();
                if (ImGui::MenuItem("Delete"))
                    s_pendingDelete = e;
                ImGui::EndPopup();
            }
        }
        ImGui::PopID();

        if (isCyclic || isMesh) ImGui::PopStyleColor();

        if (open)
        {
            if (kids)
                for (Entity child : *kids)
                    DrawEntityTree(world, child, selectedEntity, hierarchyHighlight, hmap);
            ImGui::TreePop();
        }
    }

    // Strip MSVC "struct " / "class " / "enum " prefix from typeid().name().
    const char* StripTypePrefix(const char* raw)
    {
        if (!raw) return "(unknown)";
        if (std::strncmp(raw, "struct ", 7) == 0) return raw + 7;
        if (std::strncmp(raw, "class ",  6) == 0) return raw + 6;
        if (std::strncmp(raw, "enum ",   5) == 0) return raw + 5;
        return raw;
    }

    // Type-dispatched ImGui widgets used by the MATERIAL_PROPS X-Macro.
    bool MatPropWidget(const char* label, float* v, float mn, float mx)
    {
        return ImGui::SliderFloat(label, v, mn, mx);
    }
    bool MatPropWidget(const char* label, XMFLOAT4* v, float, float)
    {
        return ImGui::ColorEdit4(label, &v->x);
    }

    ImVec4 LogLevelToColor(LogLevel level)
    {
        switch (level)
        {
        case LogLevel::Success: return ImVec4(0.2f, 0.8f, 0.2f, 1.0f);
        case LogLevel::Info:    return ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
        case LogLevel::Warning: return ImVec4(1.0f, 0.9f, 0.2f, 1.0f);
        case LogLevel::Error:   return ImVec4(1.0f, 0.3f, 0.2f, 1.0f);
        default:                return ImVec4(0.8f, 0.8f, 0.8f, 1.0f);
        }
    }
}

// ImGui lifecycle (editor-owned). Engine is ImGui-free; backends initialised in InitImGuiBackends.
EditorLayer::EditorLayer() = default;

EditorLayer::~EditorLayer()
{
    // Detach from Window first so in-flight messages don't re-enter half-destroyed ImGui state.
    Window::SetMessageHook       (nullptr);
    Window::SetWantCaptureMouse  (nullptr);
    Window::SetWantCaptureKeyboard(nullptr);

    if (m_imguiBackendsReady)
    {
        m_imguiMgr.Shutdown();
        m_imguiBackendsReady = false;
    }
    if (ImGui::GetCurrentContext())
        ImGui::DestroyContext();
}

void EditorLayer::InitImGuiBackends()
{
    if (m_imguiBackendsReady || !m_gfx) return;

    auto& dx12 = static_cast<GraphicsDX12&>(*m_gfx);
    m_imguiMgr.Initialize(
        dx12.GetHwnd(),
        dx12.GetDevice(),
        dx12.GetGraphicsQueue(),
        dx12.GetCbvSrvUavAllocator(),
        GraphicsDX12::GetFrameCount());

    // Route Win32 messages into ImGui; let Window skip input when ImGui captures it.
    Window::SetMessageHook(&ImGui_ImplWin32_WndProcHandler);
    Window::SetWantCaptureMouse   ([]() -> bool { return ImGui::GetIO().WantCaptureMouse;    });
    Window::SetWantCaptureKeyboard([]() -> bool { return ImGui::GetIO().WantCaptureKeyboard; });

    m_imguiBackendsReady = true;
}

void EditorLayer::BeginImGuiFrame()
{
    if (m_imguiBackendsReady) m_imguiMgr.BeginFrame();
}

void EditorLayer::ProcessPendingActions()
{
    if (m_pendingLoadScenePath.empty()) return;

    const std::string loadPath = std::move(m_pendingLoadScenePath);
    m_pendingLoadScenePath.clear();

    if (!m_world || !m_assetMgr) return;

    m_selectedEntity = NullEntity;

    // Hard-sync ALL queues (graphics + compute + copy — DDGI's compute pipeline
    // outlives FlushAndWait's graphics-only sync) and drain every deferred-
    // release slot before touching renderer caches. WITHOUT this, MeshDescriptor-
    // Heap reset could race in-flight compute work referencing the old bindless
    // VB/IB.
    //
    // We deliberately do NOT call WaitIdleAndReleaseDeferred a second time
    // AFTER OnWorldClear, even though it would free the prior world's GPU
    // memory before LoadScene and reduce peak VRAM. Empirically that triggered
    // a delayed GPU TDR ~5 s after reload (bindless SRV slots referencing the
    // just-freed memory seem to get reached even after a full sync). Renderer
    // is responsible for safe deferral instead (texture release chains through
    // TextureSystem::Tick; MeshLibrary uses a one-world-lag two-stage release).
    // Peak VRAM = old + new briefly during reload.
    if (m_gfx)
        m_gfx->WaitIdleAndReleaseDeferred();

    if (m_renderer)
        m_renderer->OnWorldClear();

    std::string sceneName;
    std::string ppcPath;
    std::string navPath;
    Resource::LoadScene(loadPath, *m_world, *m_assetMgr,
                       m_renderer, m_animClipSys, &sceneName, &ppcPath, &navPath);
    m_postProcessConfigPath = ppcPath;

    // .iscene stored a .inav alongside — load it so runtime FindPath /
    // NavAgent path-following are live without a separate "Load NavMesh" click.
    if (!navPath.empty() && m_navSys)
        m_navSys->Load(navPath);
}

void EditorLayer::EndImGuiFrame(RHI::CommandList cmd)
{
    if (!m_imguiBackendsReady || !m_gfx) return;
    auto& dx12 = static_cast<GraphicsDX12&>(*m_gfx);
    m_imguiMgr.Render(dx12.GetNativeCommandList(cmd), dx12.GetCbvSrvUavAllocator());
}

void EditorLayer::OnAttach()
{
    // Create ImGui context before any GetIO() call. Safe here — Window doesn't make one.
    if (!ImGui::GetCurrentContext())
    {
        ImGui::CreateContext();
        ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    }

    // Wire concrete per-NotifyCategory property panels into the registry
    // the NotifyTrackEditor consults. Idempotent — re-registering a
    // category just overwrites the previous drawer, so re-attaching the
    // editor layer is safe.
    RegisterDefaultNotifyEditors();

    ImGuiIO& io = ImGui::GetIO();

    // Custom dark theme — rounded corners + blue accent (geometry then colors).
    {
        ImGuiStyle& s = ImGui::GetStyle();
        s.WindowRounding    = 6.0f;
        s.ChildRounding     = 4.0f;
        s.FrameRounding     = 4.0f;
        s.PopupRounding     = 4.0f;
        s.ScrollbarRounding = 6.0f;
        s.GrabRounding      = 3.0f;
        s.TabRounding       = 4.0f;
        s.WindowPadding     = ImVec2(10, 10);
        s.FramePadding      = ImVec2(6, 4);
        s.ItemSpacing       = ImVec2(8, 5);
        s.ItemInnerSpacing  = ImVec2(6, 4);
        s.IndentSpacing     = 18.0f;
        s.ScrollbarSize     = 14.0f;
        s.GrabMinSize       = 10.0f;
        s.WindowBorderSize  = 1.0f;
        s.ChildBorderSize   = 1.0f;
        s.FrameBorderSize   = 0.0f;
        s.PopupBorderSize   = 1.0f;
        s.TabBorderSize     = 0.0f;
        s.WindowTitleAlign  = ImVec2(0.5f, 0.5f);

        ImVec4* c = s.Colors;
        c[ImGuiCol_WindowBg]             = ImVec4(0.12f, 0.12f, 0.14f, 1.0f);
        c[ImGuiCol_ChildBg]              = ImVec4(0.10f, 0.10f, 0.12f, 1.0f);
        c[ImGuiCol_PopupBg]              = ImVec4(0.10f, 0.10f, 0.13f, 0.96f);
        c[ImGuiCol_Border]               = ImVec4(0.22f, 0.22f, 0.26f, 0.8f);
        c[ImGuiCol_BorderShadow]         = ImVec4(0.0f,  0.0f,  0.0f,  0.0f);
        c[ImGuiCol_FrameBg]              = ImVec4(0.16f, 0.16f, 0.20f, 1.0f);
        c[ImGuiCol_FrameBgHovered]       = ImVec4(0.22f, 0.22f, 0.28f, 1.0f);
        c[ImGuiCol_FrameBgActive]        = ImVec4(0.26f, 0.26f, 0.34f, 1.0f);
        c[ImGuiCol_TitleBg]              = ImVec4(0.08f, 0.08f, 0.10f, 1.0f);
        c[ImGuiCol_TitleBgActive]        = ImVec4(0.12f, 0.14f, 0.20f, 1.0f);
        c[ImGuiCol_TitleBgCollapsed]     = ImVec4(0.08f, 0.08f, 0.10f, 0.75f);
        c[ImGuiCol_MenuBarBg]            = ImVec4(0.14f, 0.14f, 0.16f, 1.0f);
        c[ImGuiCol_ScrollbarBg]          = ImVec4(0.10f, 0.10f, 0.12f, 0.6f);
        c[ImGuiCol_ScrollbarGrab]        = ImVec4(0.30f, 0.30f, 0.36f, 1.0f);
        c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.40f, 0.40f, 0.48f, 1.0f);
        c[ImGuiCol_ScrollbarGrabActive]  = ImVec4(0.50f, 0.50f, 0.58f, 1.0f);
        c[ImGuiCol_Button]               = ImVec4(0.22f, 0.28f, 0.42f, 1.0f);
        c[ImGuiCol_ButtonHovered]        = ImVec4(0.28f, 0.36f, 0.54f, 1.0f);
        c[ImGuiCol_ButtonActive]         = ImVec4(0.18f, 0.24f, 0.40f, 1.0f);
        c[ImGuiCol_Header]               = ImVec4(0.20f, 0.22f, 0.30f, 1.0f);
        c[ImGuiCol_HeaderHovered]        = ImVec4(0.26f, 0.30f, 0.42f, 1.0f);
        c[ImGuiCol_HeaderActive]         = ImVec4(0.30f, 0.34f, 0.48f, 1.0f);
        c[ImGuiCol_Tab]                  = ImVec4(0.14f, 0.14f, 0.18f, 1.0f);
        c[ImGuiCol_TabHovered]           = ImVec4(0.28f, 0.34f, 0.50f, 1.0f);
        c[ImGuiCol_TabSelected]          = ImVec4(0.22f, 0.28f, 0.42f, 1.0f);
        c[ImGuiCol_TabDimmed]            = ImVec4(0.10f, 0.10f, 0.14f, 1.0f);
        c[ImGuiCol_TabDimmedSelected]    = ImVec4(0.16f, 0.18f, 0.26f, 1.0f);
        c[ImGuiCol_DockingPreview]       = ImVec4(0.26f, 0.40f, 0.64f, 0.7f);
        c[ImGuiCol_DockingEmptyBg]       = ImVec4(0.08f, 0.08f, 0.10f, 1.0f);
        c[ImGuiCol_Separator]            = ImVec4(0.22f, 0.22f, 0.26f, 1.0f);
        c[ImGuiCol_SeparatorHovered]     = ImVec4(0.30f, 0.40f, 0.60f, 1.0f);
        c[ImGuiCol_SeparatorActive]      = ImVec4(0.36f, 0.48f, 0.70f, 1.0f);
        c[ImGuiCol_ResizeGrip]           = ImVec4(0.26f, 0.34f, 0.50f, 0.5f);
        c[ImGuiCol_ResizeGripHovered]    = ImVec4(0.30f, 0.40f, 0.60f, 0.7f);
        c[ImGuiCol_ResizeGripActive]     = ImVec4(0.36f, 0.48f, 0.70f, 0.9f);
        c[ImGuiCol_SliderGrab]           = ImVec4(0.36f, 0.46f, 0.66f, 1.0f);
        c[ImGuiCol_SliderGrabActive]     = ImVec4(0.44f, 0.56f, 0.76f, 1.0f);
        c[ImGuiCol_CheckMark]            = ImVec4(0.46f, 0.62f, 0.90f, 1.0f);
        c[ImGuiCol_Text]                 = ImVec4(0.90f, 0.90f, 0.92f, 1.0f);
        c[ImGuiCol_TextDisabled]         = ImVec4(0.45f, 0.45f, 0.50f, 1.0f);
        c[ImGuiCol_TextSelectedBg]       = ImVec4(0.24f, 0.36f, 0.56f, 0.5f);
        c[ImGuiCol_PlotHistogram]        = ImVec4(0.36f, 0.56f, 0.86f, 1.0f);
        c[ImGuiCol_PlotHistogramHovered] = ImVec4(0.46f, 0.66f, 0.96f, 1.0f);
        c[ImGuiCol_NavHighlight]         = ImVec4(0.36f, 0.48f, 0.70f, 1.0f);
    }

    // ---- Primary font: Japanese TTF (covers ASCII + Hiragana + Katakana + Kanji) ----
    static const char* kJapaneseFontPath =
        "asset/font/HigashiOme-Gothic-1.3i.ttf";
    {
        ImFontConfig fc;
        fc.OversampleH = 1;
        fc.OversampleV = 1;
        if (io.Fonts->AddFontFromFileTTF(kJapaneseFontPath, 16.0f, &fc,
                                          io.Fonts->GetGlyphRangesJapanese()) == nullptr)
        {
            LOG_WARNING("EditorLayer: Japanese font not found at '%s', falling back to default",
                        kJapaneseFontPath);
            if (io.Fonts->Fonts.Size == 0)
                io.Fonts->AddFontDefault();
        }
    }

    // ---- Merge FontAwesome icons on top of the primary font ----
    ImFontConfig iconConfig;
    iconConfig.MergeMode        = true;
    iconConfig.PixelSnapH       = true;
    iconConfig.GlyphMinAdvanceX = 12.0f;

    static const ImWchar iconRanges[] =
    {
        0xF007, 0xF008,  // fa-user, fa-film
        0xF03E, 0xF03E,  // fa-image
        0xF04B, 0xF051,  // fa-play/pause/stop/step-forward
        0xF07B, 0xF07C,  // fa-folder / fa-folder-open
        0xF0F6, 0xF0F6,  // fa-file-text-o (script)
        0xF121, 0xF121,  // fa-code
        0xF1B2, 0xF1B3,  // fa-cube / fa-cubes
        0xF1FC, 0xF1FC,  // fa-paint-brush
        0
    };

    for (const char* path : kFontAwesomePaths)
    {
        if (io.Fonts->AddFontFromFileTTF(path, 16.0f, &iconConfig, iconRanges) != nullptr)
        {
            m_hasFontAwesomeIcons = true;
            break;
        }
    }
}

void EditorLayer::SetSceneTextureId(unsigned long long texId)
{
    m_sceneTextureId = texId;
}

void EditorLayer::GetViewportSize(unsigned int& outWidth, unsigned int& outHeight) const
{
    outWidth = m_viewportSizeX;
    outHeight = m_viewportSizeY;
}

bool EditorLayer::ConsumeWantsStepFrame()
{
    bool v = m_wantsStepFrame;
    m_wantsStepFrame = false;
    return v;
}

void EditorLayer::SetPickResult(Entity entity)
{
    m_selectedEntity = entity;
    m_hasPendingDrag = false;
    m_isDragging     = false;

    // If a material was dropped, apply it to the picked entity's MaterialComponent.
    if (!m_pendingMatDropPath.empty())
    {
        const std::string matPath = std::move(m_pendingMatDropPath);
        m_pendingMatDropPath.clear();

        if (m_world && entity != NullEntity)
        {
            MaterialComponent* mat = m_world->GetComponent<MaterialComponent>(entity);
            if (mat)
            {
                if (Resource::LoadMaterial(matPath, *mat))
                {
                    LOG_SUCCESS("EditorLayer: applied material '%s' → entity %u",
                                matPath.c_str(), entity);
                    if (m_world->HasComponent<MaterialSourcePath>(entity))
                        m_world->GetComponent<MaterialSourcePath>(entity)->path = matPath;
                    else
                        m_world->AddComponent<MaterialSourcePath>(entity, MaterialSourcePath{ matPath });
                    m_world->RemoveComponent<MaterialOverride>(entity);
                }
                else
                    LOG_ERROR("EditorLayer: failed to apply material '%s'", matPath.c_str());
            }
            else
                LOG_WARNING("EditorLayer: drop target entity %u has no MaterialComponent", entity);
        }
        else
            LOG_WARNING("EditorLayer: material drop — no entity under cursor");
    }
}

void EditorLayer::OnUIRender()
{
    ImGuizmo::BeginFrame();

    // ---- Sync reflection-probe viz visibility ----
    // Walk TagComponent pool directly. Renderer honors is_visible but probe bake loop doesn't, so capture continues hidden.
    if (m_world)
    {
        if (auto* tagPool = m_world->GetPool<TagComponent>())
        {
            const auto& tagEnts = tagPool->Entities();
            auto&       tagData = tagPool->Data();
            for (size_t i = 0; i < tagData.size(); ++i)
            {
                if (!tagData[i].Has("ReflectionProbe")) continue;
                if (auto* vis = m_world->GetComponent<VisibilityComponent>(tagEnts[i]))
                    vis->SetVisible(m_showProbeVizSpheres);
            }
        }
    }

    // ---- Resolve pending animation drops ------------------------------------
    if (!m_pendingAnimDrops.empty() && m_world && m_animClipSys && m_renderer)
    {
        for (auto it = m_pendingAnimDrops.begin(); it != m_pendingAnimDrops.end(); )
        {
            if (!m_animClipSys->IsReady(it->handle))
            {
                ++it;
                continue;
            }
            const Resource::AnimationResource* animRes = m_animClipSys->GetResource(it->handle);
            if (!animRes || animRes->clips.empty())
            {
                it = m_pendingAnimDrops.erase(it);
                continue;
            }
            // Resolve skeleton entity: new layout uses SkeletonRef, legacy holds SkeletonComponent directly.
            Entity skelEntity = it->target;
            {
                const SkeletonRef* ref = m_world->GetComponent<SkeletonRef>(it->target);
                if (ref && ref->entity != NullEntity)
                    skelEntity = ref->entity;
            }

            // Bind bone clip to skeleton (if skeleton entity has SkeletonComponent)
            auto* skelComp = m_world->GetComponent<SkeletonComponent>(skelEntity);
            if (skelComp && skelComp->assetIndex != kInvalidAnimHandle)
            {
                const SkeletonAsset& skel = m_renderer->GetSkeletonRegistry().Get(skelComp->assetIndex);
                const uint32_t clipIdx = m_animClipSys->BindToSkeleton(
                    it->handle, skel, m_renderer->GetClipLibrary());
                if (clipIdx != kInvalidClipIndex)
                {
                    auto* animComp = m_world->GetComponent<AnimationComponent>(skelEntity);
                    if (!animComp)
                    {
                        AnimationComponent newComp{};
                        newComp.primaryClip = clipIdx;
                        m_world->AddComponent(skelEntity, newComp);
                    }
                    else
                    {
                        animComp->primaryClip = clipIdx;
                        animComp->primaryTime = 0.f;
                    }
                    // Store animation source path for prefab serialization
                    if (!it->filePath.empty())
                        m_world->AddComponent<AnimationSourcePath>(skelEntity,
                            AnimationSourcePath{ it->filePath });
                    LOG_SUCCESS("EditorLayer: bound bone clip %u to skeleton entity %u (dropped on %u)",
                                clipIdx, skelEntity, it->target);
                }
            }

            // Bind morph clip → MorphComponent on skeleton entity
            const uint32_t morphIdx = m_animClipSys->BindMorphClip(
                it->handle, m_renderer->GetMorphClipLibrary());
            if (morphIdx != kInvalidClipIndex)
            {
                auto* morphComp = m_world->GetComponent<MorphComponent>(skelEntity);
                if (!morphComp)
                {
                    MorphComponent newComp{};
                    newComp.primaryMorphClip = morphIdx;
                    m_world->AddComponent(skelEntity, newComp);
                }
                else
                {
                    morphComp->primaryMorphClip = morphIdx;
                    morphComp->time             = 0.f;
                }
                LOG_SUCCESS("EditorLayer: bound morph clip %u to skeleton entity %u", morphIdx, skelEntity);
            }
            it = m_pendingAnimDrops.erase(it);
        }
    }

    // Two-click Hierarchy selection sync: any time m_selectedEntity moved
    // since last frame (external setter, picking, viewport gizmo, scene
    // load, Hierarchy second-click promotion), re-align the Hierarchy
    // highlight so the row visually matches the Inspector content. The
    // first-click case leaves m_selectedEntity untouched, so this check
    // doesn't fire and m_hierarchyHighlight stays divergent — exactly the
    // "highlight an entity to drag without losing Inspector focus" UX.
    if (m_selectedEntity != m_lastSeenSelectedEntity)
    {
        m_hierarchyHighlight     = m_selectedEntity;
        m_lastSeenSelectedEntity = m_selectedEntity;
    }

    if (m_viewportFullscreen)
    {
        RenderFullscreenViewport();
        return;
    }

    SetupDockSpace();
    RenderHierarchyPanel();
    RenderViewportPanel();
    RenderInspectorPanel();
    RenderResourcePanel();
    if (m_showPostProcess) RenderPostProcessPanel();
    RenderAnimationDebugWindow();
    if (m_showTimeline) RenderTimelinePanel();
    if (m_showProfiler) RenderProfilerPanel();
    if (m_showSSRDebug) RenderSSRDebugWindow();
    if (m_showDecalMaterials) RenderDecalMaterialsWindow();
    if (m_showFontEditor) RenderFontEditorWindow();
    if (m_showCameraSwitcher) RenderCameraSwitcherWindow();
}

void EditorLayer::RenderFullscreenViewport()
{
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::SetNextWindowViewport(vp->ID);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));

    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoDocking |
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus |
        ImGuiWindowFlags_NoBackground;

    ImGui::Begin("##FullscreenViewportHost", nullptr, flags);
    ImGui::PopStyleVar(3);

    ImVec2 size = ImGui::GetContentRegionAvail();
    if (size.x <= 0 || size.y <= 0)
        size = vp->Size;

    // Match renderer viewport resolution to the whole window.
    m_viewportSizeX = static_cast<unsigned int>(size.x);
    m_viewportSizeY = static_cast<unsigned int>(size.y);
    if (m_viewportSizeX == 0) m_viewportSizeX = 1;
    if (m_viewportSizeY == 0) m_viewportSizeY = 1;

    const ImVec2 imageTopLeft = ImGui::GetCursorScreenPos();
    const float  contentW     = size.x;
    const float  contentH     = size.y;

    if (m_sceneTextureId != 0)
    {
        ImGui::Image(ImTextureRef(static_cast<ImTextureID>(m_sceneTextureId)), size);
    }
    else
    {
        ImU32 col = IM_COL32(40, 44, 52, 255);
        ImGui::GetWindowDrawList()->AddRectFilled(imageTopLeft, ImVec2(imageTopLeft.x + size.x, imageTopLeft.y + size.y), col);
    }

    const bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
    m_viewportRightDragging = hovered && ImGui::IsMouseDown(ImGuiMouseButton_Right);
    if (m_viewportRightDragging)
    {
        const ImVec2 delta = ImGui::GetIO().MouseDelta;
        m_vpMouseDX = delta.x;
        m_vpMouseDY = delta.y;
    }
    else
    {
        m_vpMouseDX = 0.f;
        m_vpMouseDY = 0.f;
    }

    HandleViewportPicking(imageTopLeft.x, imageTopLeft.y, contentW, contentH, hovered);

    ImGui::End();
}

void EditorLayer::RenderMenuBar()
{
    if (ImGui::BeginMenu("File"))
    {
        if (ImGui::MenuItem("Save Scene..."))
        {
            std::string savePath = SaveFileDialog(
                "Scene (*.iscene)\0*.iscene\0All Files\0*.*\0\0",
                "iscene", "");
            if (!savePath.empty() && m_world)
            {
                // Export baked (non-realtime) reflection-probe cubemaps to
                // .dds beside the .iscene and stamp each probe's
                // bakedCubemapPath BEFORE serializing — SaveScene then
                // persists those paths so the next load restores the
                // cubemaps off disk instead of re-baking from scratch.
                if (m_renderer)
                    m_renderer->ExportBakedProbeCubemaps(*m_world, savePath);
                Resource::SaveScene(*m_world, savePath, "Scene",
                                    m_postProcessConfigPath,
                                    m_navSys ? m_navSys->SourcePath() : std::string{});
            }
        }
        if (ImGui::MenuItem("Load Scene..."))
        {
            std::string loadPath = OpenFileDialog(
                "Scene (*.iscene)\0*.iscene\0All Files\0*.*\0\0", "");
            if (!loadPath.empty() && m_world)
            {
                // Defer the actual teardown / load to ProcessPendingActions
                // so it runs OUTSIDE the in-flight ImGui frame. Running it
                // inline would free descriptor slots that ImGui::Image calls
                // earlier this frame already captured as raw GPU handles,
                // causing the driver to dereference dangling descriptors
                // during EndImGuiFrame.
                m_pendingLoadScenePath = std::move(loadPath);
            }
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Create"))
    {
        if (ImGui::MenuItem("Cube",   nullptr))
            if (m_createMeshCallback) m_createMeshCallback(0);
        if (ImGui::MenuItem("Sphere", nullptr))
            if (m_createMeshCallback) m_createMeshCallback(1);
        if (ImGui::MenuItem("Cone",   nullptr))
            if (m_createMeshCallback) m_createMeshCallback(2);
        if (ImGui::MenuItem("Plane",  nullptr))
            if (m_createMeshCallback) m_createMeshCallback(3);
        if (ImGui::MenuItem("Torus",  nullptr))
            if (m_createMeshCallback) m_createMeshCallback(4);

        ImGui::Separator();

        auto createLight = [this](LightType type, const char* name, float y = 5.f) {
            if (!m_world) return;
            Entity e = m_world->CreateEntity();
            m_world->SetName(e, name);

            // Position via LocalTransform (not LightData)
            LocalTransform lt{};
            lt.translation = { 0.f, y, 0.f };
            m_world->AddComponent<LocalTransform>(e, lt);
            m_world->AddComponent<GlobalTransform>(e, GlobalTransform{});

            LightData ld;
            ld.type = type;
            if (type == LightType::Point)
            {
                ld.radius    = 15.f;
                ld.intensity = 5.f;
            }
            else if (type == LightType::Spot)
            {
                ld.direction = { 0.f, -1.f, 0.f };
                ld.radius    = 20.f;
                ld.intensity = 10.f;
                ld.spotAngle = 0.5236f; // 30 degrees
            }
            m_world->AddComponent<LightData>(e, ld);

            // Billboard icon for the light (transparent forward, unlit)
            BillboardComponent bb;
            bb.mode      = BillboardMode::Transparent;
            bb.worldSize = 1.0f;
            m_world->AddComponent<BillboardComponent>(e, bb);

            m_selectedEntity = e;
        };

        if (ImGui::MenuItem("Point Light"))
            createLight(LightType::Point, "Point Light");
        if (ImGui::MenuItem("Spot Light"))
            createLight(LightType::Spot, "Spot Light");
        if (ImGui::MenuItem("Directional Light"))
            createLight(LightType::Directional, "Directional Light");

        // DDGI volume — regular probe grid; default 10×5×10 for small interior, resize via Inspector.
        if (ImGui::MenuItem("DDGI Volume"))
        {
            if (m_world)
            {
                Entity e = m_world->CreateEntity();
                m_world->SetName(e, "DDGI Volume");

                LocalTransform lt{};
                lt.translation = { 0.f, 0.f, 0.f };
                m_world->AddComponent<LocalTransform>(e, lt);
                m_world->AddComponent<GlobalTransform>(e, GlobalTransform{});

                DDGIVolumeComponent vc{};
                vc.origin = { 0.f, 5.f, 0.f };
                // Default 60×30×60 — covers one medium room. User resizes via Inspector.
                vc.extent = { 30.f, 15.f, 30.f };
                // 256 probes / 64 rays → lightweight first DispatchRays; scale up via Inspector.
                vc.probeCountsX = 8; vc.probeCountsY = 4; vc.probeCountsZ = 8;
                vc.raysPerProbe = 64;
                vc.debugDraw = true; // probe spheres visible by default for feedback

                m_world->AddComponent<DDGIVolumeComponent>(e, vc);
                m_world->AddComponent<DDGIVolumeRuntimeComponent>(e, {});

                m_selectedEntity = e;
            }
        }

        ImGui::Separator();

        if (ImGui::MenuItem("Particle Emitter"))
        {
            if (m_world)
            {
                Entity e = m_world->CreateEntity();
                m_world->SetName(e, "Particle Emitter");

                LocalTransform lt{};
                lt.translation = { 0.f, 2.f, 0.f };
                m_world->AddComponent<LocalTransform>(e, lt);
                m_world->AddComponent<GlobalTransform>(e, GlobalTransform{});

                ParticleEmitterComponent pe{};
                m_world->AddComponent<ParticleEmitterComponent>(e, pe);

                m_selectedEntity = e;
            }
        }

        if (ImGui::MenuItem("Trail"))
        {
            if (m_world)
            {
                Entity e = m_world->CreateEntity();
                m_world->SetName(e, "Trail");

                LocalTransform lt{};
                lt.translation = { 0.f, 2.f, 0.f };
                m_world->AddComponent<LocalTransform>(e, lt);
                m_world->AddComponent<GlobalTransform>(e, GlobalTransform{});

                TrailComponent tc{};
                m_world->AddComponent<TrailComponent>(e, tc);

                m_selectedEntity = e;
            }
        }

        if (ImGui::MenuItem("Beam (Procedural Tube)"))
        {
            if (m_world)
            {
                Entity e = m_world->CreateEntity();
                m_world->SetName(e, "Beam");

                // Identity transform — control points authored in world space.
                m_world->AddComponent<LocalTransform>(e, LocalTransform{});
                m_world->AddComponent<GlobalTransform>(e, GlobalTransform{});

                // Two-point default beam pointing along +X for visibility.
                BeamComponent bc;
                BeamControlPoint p0, p1;
                p0.position = { 0.f, 2.f, 0.f };
                p1.position = { 5.f, 2.f, 0.f };
                p0.radius   = p1.radius   = 0.3f;
                p0.colorTint = p1.colorTint = { 1.0f, 0.5f, 0.2f, 4.0f };
                bc.controlPoints     = { p0, p1 };
                bc.globalRadiusScale = 1.0f;
                bc.wobbleAmplitude   = 0.0f;
                bc.wobbleSpeed       = 1.0f;
                m_world->AddComponent<BeamComponent>(e, std::move(bc));

                // Default opaque inner core; outer glow is a separate entity created on demand.
                MaterialComponent mc;
                mc.useCustomShader  = true;
                mc.customShaderPath = "shaders/BeamTube_InnerCore.ps.hlsl";
                mc.userBlendMode    = BlendMode::Opaque;
                m_world->AddComponent<MaterialComponent>(e, std::move(mc));

                m_selectedEntity = e;
            }
        }

        ImGui::Separator();

        // Reflection Probe — mirror sphere acting as debug viz (rough=0/metal=1) + ReflectionProbeComponent + "ReflectionProbe" tag.
        if (ImGui::MenuItem("Reflection Probe"))
        {
            if (m_world)
            {
                Entity e = MeshSpawner::Spawn(/*Sphere*/ 1, *m_world);
                if (e != NullEntity)
                {
                    m_world->SetName(e, "Reflection Probe");

                    // Raise it above the ground so it's easy to pick.
                    if (auto* lt = m_world->GetComponent<LocalTransform>(e))
                        lt->translation = { 0.f, 2.f, 0.f };

                    // Mirror polish: rough/metal min==max collapses the GBuffer lerp regardless of surface map.
                    if (auto* mat = m_world->GetComponent<MaterialComponent>(e))
                    {
                        mat->SetRoughnessMin(0.0f);
                        mat->SetRoughnessMax(0.0f);
                        mat->SetMetalnessMin(1.0f);
                        mat->SetMetalnessMax(1.0f);
                    }

                    // ReflectionProbeComponent default extents (inner 4 / outer 6) — Renderer auto-queues first bake.
                    ReflectionProbeComponent probe{};
                    m_world->AddComponent<ReflectionProbeComponent>(e, probe);

                    // Tag for hierarchy filters + the viz toggle.
                    TagComponent tags{};
                    tags.Add("ReflectionProbe");
                    m_world->AddComponent<TagComponent>(e, tags);

                    m_selectedEntity = e;
                }
            }
        }

        // ---- UI ---- runtime-UI demos: each option creates an entity with UIRootComponent (+optional UIWorldAnchorComponent).
        if (ImGui::BeginMenu("UI"))
        {
            // -- Screen-space empty canvas — anchor for user-built widgets.
            if (ImGui::MenuItem("Empty Canvas (screen)"))
            {
                if (m_world)
                {
                    Entity e = m_world->CreateEntity();
                    m_world->SetName(e, "UI Canvas");
                    UI::UIRootComponent root;
                    root.name      = "Canvas";
                    root.sortOrder = 0;
                    root.root      = std::make_unique<UI::CanvasWidget>();
                    m_world->AddComponent<UI::UIRootComponent>(e, std::move(root));
                    // Marker tells UISystem this is a HUD-style canvas.
                    m_world->AddComponent<UI::UIScreenSpaceComponent>(e, {});
                    m_selectedEntity = e;
                }
            }

            // -- Screen-space HUD demo — HP bar + score + button.
            if (ImGui::MenuItem("Demo HUD (screen)"))
            {
                if (m_world)
                {
                    auto canvas = std::make_unique<UI::CanvasWidget>();

                    // HP bar — top-left corner.
                    auto hp = std::make_unique<UI::ProgressBarWidget>();
                    hp->spec.anchor = UI::Anchor::TopLeft;
                    hp->spec.offset = { 24.f, 24.f };
                    hp->spec.size   = { 280.f, 22.f };
                    hp->value       = 0.7f;
                    canvas->AddChild(std::move(hp));

                    // Score text — top-right.
                    auto score = std::make_unique<UI::TextWidget>();
                    score->spec.anchor = UI::Anchor::TopRight;
                    score->spec.offset = { -24.f, 24.f };
                    score->spec.pivot  = { 1.f, 0.f };
                    score->text  = "SCORE 12345";
                    score->color = UI::Color32(255, 230, 90, 255);
                    canvas->AddChild(std::move(score));

                    // Centred big-title text.
                    auto title = std::make_unique<UI::TextWidget>();
                    title->spec.anchor = UI::Anchor::Top;
                    title->spec.offset = { 0.f, 64.f };
                    title->spec.pivot  = { 0.5f, 0.f };
                    title->text  = "Hello, UI!";
                    canvas->AddChild(std::move(title));

                    // Centre button.
                    auto btn = std::make_unique<UI::ButtonWidget>();
                    btn->spec.anchor = UI::Anchor::Center;
                    btn->spec.size   = { 220.f, 56.f };
                    btn->spec.pivot  = { 0.5f, 0.5f };
                    btn->text        = "Start";
                    btn->onClick     = []() { /* hook gameplay here */ };
                    canvas->AddChild(std::move(btn));

                    Entity e = m_world->CreateEntity();
                    m_world->SetName(e, "Demo HUD");
                    UI::UIRootComponent root;
                    root.name      = "DemoHUD";
                    root.sortOrder = 10;
                    root.root      = std::move(canvas);
                    m_world->AddComponent<UI::UIRootComponent>(e, std::move(root));
                    m_world->AddComponent<UI::UIScreenSpaceComponent>(e, {});
                    m_selectedEntity = e;
                }
            }

            // -- Flat-ECS screen-space text; edit text live via Inspector → "UI Text".
            if (ImGui::MenuItem("Text Label (screen, flat)"))
            {
                if (m_world)
                {
                    Entity e = m_world->CreateEntity();
                    m_world->SetName(e, "UI Text");
                    UI::UITextComponent t;
                    t.text  = "Edit me!";
                    t.color = { 1.f, 1.f, 1.f, 1.f };
                    t.scale = 1.5f;
                    m_world->AddComponent<UI::UITextComponent>(e, t);

                    UI::UIScreenSpaceComponent ss;
                    ss.anchorX = 0.5f; ss.anchorY = 0.f; // top-centre of canvas
                    ss.offsetX = 0.f;  ss.offsetY = 32.f;
                    ss.pivotX  = 0.5f; ss.pivotY  = 0.f;
                    m_world->AddComponent<UI::UIScreenSpaceComponent>(e, ss);

                    m_selectedEntity = e;
                }
            }

            // ---- Helper: spawn world-space UI entity (LocalTransform + Global + FollowEntity + WorldSpaceUI). ----
            auto spawnWorldUIChild = [&](const char* dbgName, float yOffset,
                                         DirectX::XMFLOAT2 baseSize) -> Entity
            {
                Entity e = m_world->CreateEntity();
                m_world->SetName(e, dbgName);

                LocalTransform lt;
                lt.translation = { 0.f, yOffset, 0.f };
                lt.scale       = { 1.f, 1.f, 1.f };
                XMStoreFloat4(&lt.rotation, XMQuaternionIdentity());
                m_world->AddComponent<LocalTransform>(e, lt);
                m_world->AddComponent<GlobalTransform>(e, GlobalTransform{});

                FollowEntityComponent fe;
                fe.target = m_world->MakeHandle(static_cast<Entity>(m_selectedEntity));
                XMStoreFloat4x4(&fe.localOffset,
                                 XMMatrixTranslation(0.f, yOffset, 0.f));
                m_world->AddComponent<FollowEntityComponent>(e, fe);

                UI::WorldSpaceUIComponent ws;
                ws.baseSize    = baseSize;
                ws.pivot       = { 0.5f, 1.0f };
                ws.scalingMode = UI::ScalingMode::ConstantWorld;
                ws.fadeFar     = 50.0f;
                m_world->AddComponent<UI::WorldSpaceUIComponent>(e, ws);

                return e;
            };

            // -- World-space text floating above selected entity.
            const bool hasTargetText = (m_selectedEntity != NullEntity)
                                       && m_world && m_world->IsAlive(m_selectedEntity)
                                       && m_world->HasComponent<GlobalTransform>(m_selectedEntity);
            if (ImGui::MenuItem("Text Label (world, on selected)", nullptr, false, hasTargetText))
            {
                if (hasTargetText)
                {
                    Entity e = spawnWorldUIChild("Floating Text", 2.5f, { 1.0f, 0.4f });

                    UI::WorldUITextComponent t;
                    t.text  = m_world->GetName(static_cast<Entity>(m_selectedEntity));
                    if (t.text.empty()) t.text = "Entity";
                    t.color = { 1.f, 1.f, 1.f, 1.f };
                    t.scale = 1.0f;
                    m_world->AddComponent<UI::WorldUITextComponent>(e, t);

                    m_selectedEntity = e;
                }
            }

            // -- World-space HP bar (no widget tree).
            const bool hasFlatBarTarget = (m_selectedEntity != NullEntity)
                                          && m_world && m_world->IsAlive(m_selectedEntity)
                                          && m_world->HasComponent<GlobalTransform>(m_selectedEntity);
            if (ImGui::MenuItem("HP Bar (world, on selected)", nullptr, false, hasFlatBarTarget))
            {
                if (hasFlatBarTarget)
                {
                    Entity e = spawnWorldUIChild("HP Bar", 2.0f, { 1.2f, 0.18f });
                    UI::WorldUIBarComponent bar;
                    bar.value = 0.7f;
                    m_world->AddComponent<UI::WorldUIBarComponent>(e, bar);
                    m_selectedEntity = e;
                }
            }
            if (!hasFlatBarTarget && ImGui::IsItemHovered())
                ImGui::SetTooltip("Select an entity with GlobalTransform first.");

            // -- Paired Name + HP Bar above the selected entity (two entities).
            const bool hasTarget = (m_selectedEntity != NullEntity)
                                   && m_world && m_world->IsAlive(m_selectedEntity)
                                   && m_world->HasComponent<GlobalTransform>(m_selectedEntity);
            if (ImGui::MenuItem("Name + HP Bar (world, on selected)", nullptr, false, hasTarget))
            {
                if (hasTarget)
                {
                    // Bar
                    Entity bar = spawnWorldUIChild("World HP Bar", 2.0f, { 1.4f, 0.20f });
                    UI::WorldUIBarComponent bc;
                    bc.value = 1.0f;
                    m_world->AddComponent<UI::WorldUIBarComponent>(bar, bc);

                    // Name label, slightly higher
                    Entity label = spawnWorldUIChild("World Name Label", 2.5f, { 1.4f, 0.30f });
                    UI::WorldUITextComponent tc;
                    tc.text  = m_world->GetName(static_cast<Entity>(m_selectedEntity));
                    if (tc.text.empty()) tc.text = "Entity";
                    m_world->AddComponent<UI::WorldUITextComponent>(label, tc);

                    m_selectedEntity = bar;
                }
            }
            if (!hasTarget && ImGui::IsItemHovered())
                ImGui::SetTooltip("Select an entity in the Hierarchy first.");

            // -- Damage number — bursts upward with auto-fade + auto-destroy.
            if (ImGui::MenuItem("Damage Number (world, on selected)", nullptr, false, hasTarget))
            {
                if (hasTarget)
                {
                    Entity e = spawnWorldUIChild("Damage Number", 1.8f, { 0.6f, 0.4f });
                    UI::DamageNumberComponent dn;
                    dn.text          = "100";
                    dn.lifetime      = 1.5f;
                    dn.totalLifetime = 1.5f;
                    dn.velocity      = { 0.f, 1.5f, 0.f };
                    dn.color         = { 1.0f, 0.4f, 0.3f, 1.0f };
                    m_world->AddComponent<UI::DamageNumberComponent>(e, dn);
                    m_selectedEntity = e;
                }
            }

            ImGui::EndMenu();
        }

        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Import"))
    {
        if (ImGui::MenuItem("Texture..."))   RunImport(AssetType::Texture);
        if (ImGui::MenuItem("Model..."))     RunImport(AssetType::Mesh);
        if (ImGui::MenuItem("Shader..."))    RunImport(AssetType::Shader);
        if (ImGui::MenuItem("Material..."))  RunImport(AssetType::Material);
        if (ImGui::MenuItem("Animation...")) RunImport(AssetType::Animation);
        if (ImGui::MenuItem("Audio..."))     RunImport(AssetType::Audio);
        ImGui::EndMenu();
    }

    // ---- Build ---- spawns tools/package_game.py in a new console; editor stays responsive. ----
    auto launchPackager = [](const char* config)
    {
        // /k keeps the console open after exit so build errors stay visible.
        char cmd[512];
        snprintf(cmd, sizeof(cmd),
            "/k cd /d \"%%CD%%\" && python tools\\package_game.py --config %s",
            config);
        ShellExecuteA(nullptr, "open", "cmd.exe", cmd, nullptr, SW_SHOWNORMAL);
    };

    if (ImGui::BeginMenu("Build"))
    {
        if (ImGui::MenuItem("Set Startup Scene..."))
        {
            std::string path = OpenFileDialog(
                "Scene (*.iscene)\0*.iscene\0All Files\0*.*\0\0", "asset/");
            if (!path.empty())
            {
                // Convert to path relative to working dir (project/package root); fall back to absolute.
                namespace fs = std::filesystem;
                std::error_code ec;
                fs::path rel = fs::relative(fs::path(path), fs::current_path(), ec);
                std::string scenePath = (ec || rel.empty()) ? path : rel.string();
                std::replace(scenePath.begin(), scenePath.end(), '\\', '/');

                FILE* fp = nullptr;
                if (fopen_s(&fp, "game.json", "w") == 0 && fp)
                {
                    fprintf(fp,
                        "{\n"
                        "  \"_comment\": \"Edit startup_scene to the .iscene Game.exe should boot into.\",\n"
                        "  \"startup_scene\": \"%s\"\n"
                        "}\n",
                        scenePath.c_str());
                    fclose(fp);
                    LOG_INFO("game.json updated: startup_scene = '%s'", scenePath.c_str());
                }
                else
                {
                    LOG_ERROR("Failed to write game.json");
                }
            }
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Package Game (Release)"))
            launchPackager("Release");
        if (ImGui::MenuItem("Package Game (Debug)"))
            launchPackager("Debug");
        ImGui::Separator();
        if (ImGui::MenuItem("Open Output Folder"))
            ShellExecuteA(nullptr, "open", "build", nullptr, nullptr, SW_SHOWNORMAL);
        ImGui::EndMenu();
    }

    // --- Tools menu (bake operations) ---
    // Popup body lives outside the menu-bar context (see DrawBakeCollisionPopup
    // call after EndMenuBar in OnUIRender). Modal popups can't be opened from
    // inside BeginMenuBar — the ID stack hash won't match.
    if (ImGui::BeginMenu("Tools"))
    {
        // All one-shot bake / asset-generation actions live here (previously
        // split across two separate "Tools" menus).
        if (ImGui::MenuItem("Bake Collision Meshes..."))
            g_bakeCollisionOpenRequest = true;
        if (ImGui::MenuItem("Bake NavMesh..."))
            g_bakeNavMeshOpenRequest = true;
        ImGui::Separator();
        {
            const bool canBake = m_renderer != nullptr;
            if (!canBake) ImGui::BeginDisabled();
            if (ImGui::MenuItem("Bake All Reflection Probes") && m_renderer)
                m_renderer->BakeAllProbes();
            if (!canBake) ImGui::EndDisabled();
        }
        ImGui::EndMenu();
    }

    // ---- Window ---- one place for every dockable / floating editor panel
    // toggle (was scattered across Settings / Debug / a second Tools menu). ----
    if (ImGui::BeginMenu("Window"))
    {
        // Post-process / color-grading settings panel.
        ImGui::MenuItem("Post Processing", nullptr, &m_showPostProcess);
        // GPU timing overlay — keep the GPUProfiler's own enable flag in sync.
        if (ImGui::MenuItem("GPU Profiler", nullptr, &m_showProfiler))
        {
            if (m_gpuProfiler)
                m_gpuProfiler->enabled = m_showProfiler;
        }
        // Animation / phase inspectors.
        ImGui::MenuItem("Animation Debug", nullptr, &m_showAnimDebug);
        ImGui::MenuItem("Phase Debug",     nullptr, &m_showPhaseDebug);
        // SSR Debug window — full-screen modes, per-stage previews, runtime tuning.
        ImGui::MenuItem("SSR Debug",       nullptr, &m_showSSRDebug);
        // Camera Switcher — list/push/pop/HardCut VCams per channel.
        ImGui::MenuItem("Camera Switcher", nullptr, &m_showCameraSwitcher);

        ImGui::Separator();
        // Animation Timeline editor — AnimNotify track authoring on .ianim clips.
        ImGui::MenuItem("Timeline Editor", nullptr, &m_showTimeline);
        // Decal Materials editor.
        ImGui::MenuItem("Decal Materials", nullptr, &m_showDecalMaterials);
        // UI Font Editor — live re-bake of the runtime UI text font.
        ImGui::MenuItem("UI Font Editor",  nullptr, &m_showFontEditor);
        ImGui::EndMenu();
    }

    // ---- Settings ---- engine feature toggles only (window panels live under
    // "Window"; debug-overlay visibility lives under "Debug"). ----
    if (ImGui::BeginMenu("Settings"))
    {
        if (m_renderer)
        {
            bool indirect = m_renderer->IsIndirectDrawEnabled();
            if (ImGui::MenuItem("Indirect Draw (WIP)", nullptr, &indirect))
                m_renderer->SetIndirectDrawEnabled(indirect);

            bool gpuCull = m_renderer->IsGPUCullingEnabled();
            if (ImGui::MenuItem("Frustum Culling", nullptr, &gpuCull))
                m_renderer->SetGPUCullingEnabled(gpuCull);

            bool animCull = m_renderer->IsAnimCullingEnabled();
            if (ImGui::MenuItem("Animation Culling", nullptr, &animCull))
                m_renderer->SetAnimCullingEnabled(animCull);

            if (IKSystem* ik = m_renderer->GetIKSystem())
            {
                bool ikOn = ik->IsEnabled();
                if (ImGui::MenuItem("IK Solver", nullptr, &ikOn))
                    ik->SetEnabled(ikOn);
            }

            bool ssao = m_renderer->IsSSAOEnabled();
            if (ImGui::MenuItem("SSAO (XeGTAO)", nullptr, &ssao))
                m_renderer->SetSSAOEnabled(ssao);
        }

        if (m_gfx)
        {
            auto& dx12 = static_cast<GraphicsDX12&>(*m_gfx);
            ImGui::MenuItem("VSync", nullptr, &dx12.vsyncEnabled);
        }

        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Debug"))
    {
        // Wireframe overlays (moved out of Settings). Master toggle gates
        // every sub-flag — even with sub-flags on, the wire buffer stays
        // empty unless `Debug Wireframes` is checked.
        if (m_renderer)
        {
            // All wireframe overlays are now driven by the editor-only
            // DebugDrawSystem's central category bitmask (the single source of
            // truth that App pushes onto the renderer each frame), instead of
            // poking DebugWirePass / NavMeshSystem flags directly.
            if (DebugDrawSystem* dd = m_debugDraw)
            {
                ImGui::MenuItem("Debug Wireframes", nullptr, &dd->masterEnabled);
                if (dd->masterEnabled)
                {
                    auto catItem = [&](const char* label, DebugCategory cat)
                    {
                        bool on = DebugCatEnabled(dd->categoryMask, cat);
                        if (ImGui::MenuItem(label, nullptr, &on))
                            DebugCatSet(dd->categoryMask, cat, on);
                    };
                    catItem("  Show AABBs",             DebugCategory::AABB);
                    catItem("  Show Frustum",           DebugCategory::Frustum);
                    catItem("  Show Capsules",          DebugCategory::Capsules);
                    catItem("  Show Reflection Probes", DebugCategory::ReflectionProbes);
                    catItem("  Show DDGI Volumes",      DebugCategory::DDGIVolumes);
                    catItem("  Show Collision",         DebugCategory::Collision);
                    if (DebugCatEnabled(dd->categoryMask, DebugCategory::Collision))
                    {
                        // 0 = unlimited (every Mesh collider in the world).
                        // Dial down when the wire buffer fills (look at the
                        // log — AddLine bails silently past kMaxVertices).
                        ImGui::SetNextItemWidth(180);
                        ImGui::SliderFloat("    Max distance (0 = unlimited)",
                                            &dd->collisionMaxDistance,
                                            0.0f, 200.0f, "%.0f m");
                    }
                    catItem("  Show NavMesh",           DebugCategory::NavMesh);
                }
            }
            if (auto* pdbg = m_renderer->GetDDGIProbeDebugPass())
            {
                ImGui::Separator();
                ImGui::MenuItem("DDGI Probe Spheres",       nullptr, &pdbg->enabled);
                if (pdbg->enabled)
                {
                    static const char* kModes[] = {
                        "0: SH irradiance (default)",
                        "1: grid-coord color",
                        "2: SH L0 magnitude (verify SH has data)",
                        "3: SH L1 direction (verify SH evolves)" };
                    int mode = (int)pdbg->debugMode;
                    if (ImGui::Combo("  Debug Mode", &mode, kModes, IM_ARRAYSIZE(kModes)))
                        pdbg->debugMode = (uint32_t)mode;
                    ImGui::DragFloat("  Sphere Radius",     &pdbg->sphereRadius, 0.005f, 0.01f, 5.0f, "%.3f");
                }
            }

            // ---- Scene-gizmo visibility (overlay billboards / probe viz) ----
            ImGui::Separator();
            // Toggles VisibilityComponent.flags.Visible on all "ReflectionProbe"-tagged entities (capture continues regardless).
            ImGui::MenuItem("Reflection Probe Viz", nullptr, &m_showProbeVizSpheres);
            // Debug icon gizmos — one toggle per registered icon kind. This is
            // data-driven: adding a kind to DebugDrawSystem's kIconKinds[] adds
            // its toggle here automatically (no edit needed in this menu).
            if (DebugDrawSystem* dd = m_debugDraw)
            {
                for (int i = 0; i < DebugDrawSystem::IconKindCount(); ++i)
                {
                    const DebugDrawSystem::IconKindInfo info = DebugDrawSystem::GetIconKindInfo(i);
                    bool on = DebugCatEnabled(dd->categoryMask, info.category);
                    if (ImGui::MenuItem(info.name, nullptr, &on))
                        DebugCatSet(dd->categoryMask, info.category, on);
                }
            }

            ImGui::Separator();
        }

        // Decal cluster heatmap — green/yellow/red overlay for overdraw + bounds-sphere sanity.
        if (m_renderer)
        {
            DecalPass* dp = m_renderer->GetDecalPass();
            if (dp)
            {
                bool heat = dp->GetDebugHeatmap();
                if (ImGui::MenuItem("Decal Cluster Heatmap", nullptr, &heat))
                    dp->SetDebugHeatmap(heat);
            }

            // ---- Tracer test mode: T fires one along view ray; burst entry stress-tests ring cursor. ----
            if (TracerSystem* ts = m_renderer->GetTracerSystem())
            {
                ImGui::MenuItem("Tracer Test Mode (T to fire)",
                                nullptr, &m_tracerTestModeEnabled);
                if (ImGui::MenuItem("Test Spawn Tracer (one-shot)"))
                {
                    using DirectX::XMFLOAT3;
                    using DirectX::XMFLOAT4;
                    const XMFLOAT3& cp = m_renderView.cameraPosition;
                    const XMFLOAT3& cf = m_renderView.cameraForward;
                    XMFLOAT3 start{ cp.x + cf.x * 0.5f,
                                    cp.y + cf.y * 0.5f,
                                    cp.z + cf.z * 0.5f };
                    XMFLOAT3 end  { cp.x + cf.x * 30.0f,
                                    cp.y + cf.y * 30.0f,
                                    cp.z + cf.z * 30.0f };
                    ts->Spawn(start, end,
                              XMFLOAT4{ 1.0f, 0.85f, 0.3f, 6.0f },
                              /*width=*/0.5f,
                              /*lifetime=*/0.8f);
                }
                if (ImGui::MenuItem("Test Spawn Tracer Burst (32)"))
                {
                    using DirectX::XMFLOAT3;
                    using DirectX::XMFLOAT4;
                    const XMFLOAT3& cp = m_renderView.cameraPosition;
                    const XMFLOAT3& cf = m_renderView.cameraForward;
                    // 32-shot fan around view ray — stresses ring cursor + depth-fade math.
                    for (int k = 0; k < 32; ++k)
                    {
                        const float ang   = static_cast<float>(k) * 0.196f;  // ~11.25° steps
                        const float spread = 4.0f;
                        XMFLOAT3 end{ cp.x + cf.x * 25.0f + std::cos(ang) * spread,
                                      cp.y + cf.y * 25.0f + std::sin(ang) * spread,
                                      cp.z + cf.z * 25.0f };
                        XMFLOAT3 start{ cp.x + cf.x * 0.5f,
                                        cp.y + cf.y * 0.5f,
                                        cp.z + cf.z * 0.5f };
                        // Rainbow colour by index for easy visual identification.
                        const float h = static_cast<float>(k) / 32.0f;
                        XMFLOAT4 col{
                            0.5f + 0.5f * std::cos(6.28f * h),
                            0.5f + 0.5f * std::cos(6.28f * (h + 0.33f)),
                            0.5f + 0.5f * std::cos(6.28f * (h + 0.66f)),
                            5.0f
                        };
                        ts->Spawn(start, end, col, 0.04f, 0.6f);
                    }
                }
            }
        }
        ImGui::EndMenu();
    }
}

void EditorLayer::SetupDockSpace()
{
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::SetNextWindowViewport(viewport->ID);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));

    ImGuiWindowFlags hostFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse
        | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove
        | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus
        | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_MenuBar;

    ImGui::Begin("EditorHost", nullptr, hostFlags);
    ImGui::PopStyleVar(3);

    if (ImGui::BeginMenuBar())
    {
        RenderMenuBar();
        ImGui::EndMenuBar();
    }

    // Tools > Bake Collision Meshes... popup. Body lives here (outside the
    // menu bar) so the modal's ID stack matches OpenPopup's caller scope.
    if (g_bakeCollisionOpenRequest)
    {
        ImGui::OpenPopup("Bake Collision Meshes");
        g_bakeCollisionOpenRequest = false;
    }
    {
        static Tools::CollisionMesh::BakeOptions s_opts;
        static Tools::CollisionMesh::BakeJob     s_job;
        static char s_outPath[260] = {};
        static bool s_outPathInit  = false;
        if (!s_outPathInit)
        {
            std::snprintf(s_outPath, sizeof(s_outPath), "%s", s_opts.outputPath.c_str());
            s_outPathInit = true;
        }

        // ---- Preview-swap state ----------------------------------------
        // When the user clicks "Preview baked", every entity that referenced
        // a source mesh gets its MeshLibRef rewritten to point at the baked
        // library so the simplified geometry renders in-place. Originals are
        // saved here so "Restore" can swap them back.
        struct PreviewBackup { MeshLibRef original; };
        static std::unordered_map<Entity, PreviewBackup> s_previewBackups;
        static Resource::Handle                          s_previewBakedLib;   // invalid if not loaded
        static std::string                               s_previewLoadedFor;  // path of currently-loaded baked lib

        ImGui::SetNextWindowSize(ImVec2(700, 0), ImGuiCond_Appearing);
        if (ImGui::BeginPopupModal("Bake Collision Meshes",
                                    nullptr,
                                    ImGuiWindowFlags_AlwaysAutoResize))
        {
            const bool running     = s_job.IsRunning();
            const bool hasPreview  = !s_previewBackups.empty();

            ImGui::TextWrapped(
                "Walks every MeshLibRef entity in the current world, simplifies "
                "each source mesh in parallel via meshoptimizer, and combines "
                "all results into ONE .meshlib (shared VB/IB + per-mesh entries "
                "— engine-native format). Use \"Preview baked\" to render the "
                "simplified meshes in-place for visual comparison.");
            ImGui::Separator();

            // ---- Options form (disabled while a bake is in flight) -----
            ImGui::BeginDisabled(running);
            ImGui::SliderFloat("Target triangle ratio",
                                &s_opts.targetTriangleRatio, 0.01f, 1.0f, "%.2f");
            ImGui::SliderFloat("Target error (normalized)",
                                &s_opts.targetError, 0.001f, 0.5f, "%.3f");
            int minTri = static_cast<int>(s_opts.minTriangles);
            if (ImGui::InputInt("Min triangles", &minTri))
                s_opts.minTriangles = static_cast<uint32_t>(std::max(0, minTri));

            ImGui::Checkbox("Skip skinned", &s_opts.skipSkinned);

            if (ImGui::InputText("Output .meshlib path", s_outPath, sizeof(s_outPath)))
                s_opts.outputPath = s_outPath;
            ImGui::EndDisabled();

            ImGui::Text("Worker threads: %u",  TaskSystem::Get().GetWorkerCount());
            ImGui::Separator();

            const bool hasWorld = (m_world != nullptr);
            if (!hasWorld)
                ImGui::TextColored(ImVec4(1, 0.5f, 0.5f, 1), "No world bound.");

            // ---- Action buttons row ------------------------------------
            ImGui::BeginDisabled(!hasWorld || running || hasPreview);
            if (ImGui::Button("Bake"))
            {
                s_opts.outputPath = s_outPath;
                s_job.Begin(*m_world, s_opts);
            }
            ImGui::EndDisabled();

            ImGui::SameLine();
            ImGui::BeginDisabled(!running);
            if (ImGui::Button("Cancel"))
                s_job.Reset();
            ImGui::EndDisabled();

            ImGui::SameLine();
            ImGui::BeginDisabled(running || hasPreview);
            if (ImGui::Button("Clear results"))
                s_job.Reset();
            ImGui::EndDisabled();

            ImGui::SameLine();
            ImGui::BeginDisabled(running);
            if (ImGui::Button("Close"))
                ImGui::CloseCurrentPopup();
            ImGui::EndDisabled();

            // ---- Progress bar -----------------------------------------
            if (running)
            {
                s_job.Tick();
                const float prog = s_job.Progress();
                char overlay[64];
                std::snprintf(overlay, sizeof(overlay), "%u / %u  (%.0f%%)",
                              s_job.Processed(), s_job.Total(), prog * 100.f);
                ImGui::ProgressBar(prog, ImVec2(-FLT_MIN, 0), overlay);
            }

            // ---- Preview row ------------------------------------------
            if (s_job.IsComplete() && !s_job.WriteFailed())
            {
                ImGui::Separator();
                const bool canPreview = hasWorld && m_renderer && m_gfx && !hasPreview;
                ImGui::BeginDisabled(!canPreview);
                if (ImGui::Button("Preview baked in world"))
                {
                    Resource::MeshLibrary* libSys = m_renderer->GetMeshLibrary();
                    if (libSys)
                    {
                        // Load (or re-load) the baked .meshlib into the runtime
                        // library system; the returned handle is what the
                        // renderer dereferences each frame.
                        Resource::Handle baked = libSys->Load(s_job.OutputPath(), *m_gfx);
                        if (baked.IsValid())
                        {
                            s_previewBakedLib   = baked;
                            s_previewLoadedFor  = s_job.OutputPath();

                            // Build (sourcePath, sourceMeshId) → bakedMeshId.
                            std::unordered_map<std::string,
                                std::unordered_map<uint32_t, uint32_t>> bakedMap;
                            for (const auto& r : s_job.Results())
                            {
                                if (!r.success) continue;
                                bakedMap[r.sourcePath][r.sourceMeshId] = r.bakedMeshId;
                            }

                            // Walk entities; rewrite MeshLibRef in place.
                            uint32_t swapped = 0;
                            m_world->ForEach<MeshLibRef>([&](Entity e, MeshLibRef& ref)
                            {
                                const MeshSourcePath* sp = m_world->GetComponent<MeshSourcePath>(e);
                                if (!sp) return;
                                auto pit = bakedMap.find(sp->path);
                                if (pit == bakedMap.end()) return;
                                auto mit = pit->second.find(ref.meshId);
                                if (mit == pit->second.end()) return;

                                s_previewBackups[e] = { ref };
                                ref.libHandle        = s_previewBakedLib;
                                ref.meshId           = mit->second;
                                ref.cachedGeneration = 0; // force renderer re-lookup
                                ++swapped;
                            });
                            LOG_INFO("CollisionMeshBaker: preview swapped %u entities to '%s'",
                                     swapped, s_previewLoadedFor.c_str());
                        }
                        else
                        {
                            LOG_ERROR("CollisionMeshBaker: failed to load baked .meshlib '%s' for preview",
                                      s_job.OutputPath().c_str());
                        }
                    }
                }
                ImGui::EndDisabled();

                ImGui::SameLine();
                ImGui::BeginDisabled(!hasPreview);
                if (ImGui::Button("Restore originals"))
                {
                    for (auto& [e, backup] : s_previewBackups)
                    {
                        if (MeshLibRef* ref = m_world->GetComponent<MeshLibRef>(e))
                        {
                            *ref = backup.original;
                            ref->cachedGeneration = 0;
                        }
                    }
                    LOG_INFO("CollisionMeshBaker: restored %zu entities from preview swap",
                             s_previewBackups.size());
                    s_previewBackups.clear();

                    // Release the previewed baked lib and FLUSH the renderer's
                    // mesh-descriptor cache. MeshManager keys its cache by
                    // (libIdx << 32) | meshId; MeshLibrary::Release reuses the
                    // libIdx slot on the next Load(), so without this flush the
                    // next preview gets stale descriptor entries pointing at
                    // freed GPU buffers — device removed on next draw.
                    if (s_previewBakedLib.IsValid() && m_renderer && m_gfx)
                    {
                        if (Resource::MeshLibrary* libSys = m_renderer->GetMeshLibrary())
                            libSys->Release(s_previewBakedLib, *m_gfx);
                        m_renderer->OnWorldClear();
                    }
                    s_previewBakedLib  = {};
                    s_previewLoadedFor.clear();
                }
                ImGui::EndDisabled();

                if (hasPreview)
                {
                    ImGui::SameLine();
                    ImGui::TextColored(ImVec4(0.6f, 0.9f, 1.f, 1),
                        "(previewing %zu entities)", s_previewBackups.size());
                }

                // ---- Collision wire-up -------------------------------
                // Walks the world, attaches a Mesh ColliderComponent +
                // Static RigidBodyComponent to every entity whose source
                // mesh was just baked. Mesh colliders are Jolt MeshShapes;
                // they're static-only, which matches level geometry.
                static uint32_t s_lastColliderApplyCount = 0;
                const bool canApplyCollision = hasWorld && !running;
                ImGui::BeginDisabled(!canApplyCollision);
                if (ImGui::Button("Use baked as collision"))
                {
                    std::unordered_map<std::string,
                        std::unordered_map<uint32_t, uint32_t>> bakedMap;
                    for (const auto& r : s_job.Results())
                    {
                        if (!r.success) continue;
                        bakedMap[r.sourcePath][r.sourceMeshId] = r.bakedMeshId;
                    }
                    const std::string libPath = s_job.OutputPath();

                    uint32_t applied  = 0;
                    uint32_t replaced = 0;
                    std::vector<DX12Physics::PhysicsSystem::MeshShapeRequest>
                        prewarmRequests;
                    m_world->ForEach<MeshLibRef>([&](Entity e, MeshLibRef& ref)
                    {
                        const MeshSourcePath* sp = m_world->GetComponent<MeshSourcePath>(e);
                        if (!sp) return;
                        auto pit = bakedMap.find(sp->path);
                        if (pit == bakedMap.end()) return;
                        auto mit = pit->second.find(ref.meshId);
                        if (mit == pit->second.end()) return;

                        ColliderComponent col{};
                        col.shape         = ColliderComponent::Shape::Mesh;
                        col.meshLibPath   = libPath;
                        col.meshLibMeshId = mit->second;
                        prewarmRequests.push_back({ libPath, mit->second });

                        if (m_world->HasComponent<ColliderComponent>(e))
                        {
                            *m_world->GetComponent<ColliderComponent>(e) = col;
                            ++replaced;
                        }
                        else
                        {
                            m_world->AddComponent<ColliderComponent>(e, col);
                        }

                        if (!m_world->HasComponent<RigidBodyComponent>(e))
                        {
                            RigidBodyComponent rb{};
                            rb.motion = RigidBodyComponent::Motion::Static;
                            m_world->AddComponent<RigidBodyComponent>(e, rb);
                        }
                        else
                        {
                            // Mesh colliders require Static — see PhysicsSystem warning.
                            m_world->GetComponent<RigidBodyComponent>(e)->motion
                                = RigidBodyComponent::Motion::Static;
                            // Force the existing body to be torn down so it
                            // re-creates with the new MeshShape on next tick.
                            m_world->GetComponent<RigidBodyComponent>(e)->bodyId
                                = kInvalidPhysicsBodyId;
                        }
                        ++applied;
                    });
                    s_lastColliderApplyCount = applied;
                    if (m_physicsSys && m_world)
                    {
                        // After re-bake the cached MeshShape is stale (same
                        // path, different content). Flush + bump Mesh-collider
                        // generations so the runtime-swap detector tears down
                        // existing bodies and Phase 1 rebuilds them.
                        m_physicsSys->InvalidateMeshShapeCache(*m_world);
                        // Eagerly parallel-build all referenced MeshShapes
                        // now, so the first Play tick doesn't pay the entire
                        // file-read + BVH-build cost on the main thread.
                        m_physicsSys->PrewarmMeshShapes(prewarmRequests);
                    }

                    LOG_INFO("CollisionMeshBaker: attached Mesh collider to %u entities "
                             "(%u replaced existing collider)", applied, replaced);
                }
                ImGui::EndDisabled();
                if (s_lastColliderApplyCount > 0)
                {
                    ImGui::SameLine();
                    ImGui::TextColored(ImVec4(0.6f, 1.f, 0.6f, 1),
                        "(applied to %u entities)", s_lastColliderApplyCount);
                }

            }

            // ---- Summary / results ------------------------------------
            if (s_job.IsComplete() || !s_job.Results().empty())
            {
                ImGui::Separator();
                const auto& results = s_job.Results();
                uint32_t ok = 0, fail = 0, skip = 0;
                for (const auto& r : results)
                {
                    if (r.skipped)      ++skip;
                    else if (r.success) ++ok;
                    else                ++fail;
                }
                ImGui::Text("Baked: %u    Failed: %u    Skipped: %u",
                            ok, fail, skip);
                if (s_job.IsComplete())
                {
                    if (s_job.WriteFailed())
                        ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1),
                            "Write FAILED — see DX12Log.txt");
                    else if (ok > 0)
                        ImGui::TextColored(ImVec4(0.5f, 1.f, 0.5f, 1),
                            "Wrote '%s'  (%llu -> %llu tris total)",
                            s_job.OutputPath().c_str(),
                            (unsigned long long)s_job.OrigTriTotal(),
                            (unsigned long long)s_job.BakedTriTotal());
                    else
                        ImGui::TextColored(ImVec4(1, 0.8f, 0.4f, 1),
                            "No meshes survived — nothing written.");
                }
                if (ImGui::BeginChild("##BakeResultsList", ImVec2(0, 240), true))
                {
                    for (const auto& r : results)
                    {
                        if (r.skipped)
                            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1),
                                "[SKIP] %s  (%s)",
                                r.sourcePath.empty() ? "(no source)"
                                                     : r.sourcePath.c_str(),
                                r.reason.c_str());
                        else if (r.success)
                            ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1),
                                "[ OK ] %s [src=%u baked=%u]   %u -> %u tris   err %.4f",
                                r.sourcePath.c_str(), r.sourceMeshId, r.bakedMeshId,
                                r.origTriCount, r.bakedTriCount, r.actualError);
                        else
                            ImGui::TextColored(ImVec4(1, 0.5f, 0.5f, 1),
                                "[FAIL] %s  %s",
                                r.sourcePath.empty() ? "(no source)"
                                                     : r.sourcePath.c_str(),
                                r.reason.c_str());
                    }
                }
                ImGui::EndChild();
            }

            ImGui::EndPopup();
        }
    }

    // -------------------------------------------------------------------
    // Tools > Bake NavMesh... popup (Recast / Detour).
    // Inputs are Mesh ColliderComponents in the world — apply collision via
    // the "Bake Collision Meshes" tool first, then come here to bake nav.
    // -------------------------------------------------------------------
    if (g_bakeNavMeshOpenRequest)
    {
        ImGui::OpenPopup("Bake NavMesh");
        g_bakeNavMeshOpenRequest = false;
    }
    {
        static Nav::BuildParams s_navParams;
        static Nav::BuildStats  s_navStats;
        static char             s_navOut[260]    = "asset/nav/world.inav";
        static char             s_navLoadPath[260] = "asset/nav/world.inav";

        ImGui::SetNextWindowSize(ImVec2(560, 0), ImGuiCond_Appearing);
        if (ImGui::BeginPopupModal("Bake NavMesh", nullptr,
                                    ImGuiWindowFlags_AlwaysAutoResize))
        {
            const bool hasWorld = (m_world != nullptr);
            const bool hasNav   = (m_navSys != nullptr);

            ImGui::TextWrapped(
                "Feeds every Mesh ColliderComponent in the world through Recast "
                "(voxel rasterise → contour → polymesh → detail mesh → Detour "
                "tile) and writes a .inav. Runtime FindPath / NavAgent path-"
                "following go live as soon as Build completes.");
            ImGui::Separator();

            // Quick status — what's currently loaded.
            if (hasNav && m_navSys->IsReady())
            {
                const std::string& src = m_navSys->SourcePath();
                ImGui::TextColored(ImVec4(0.6f, 1.f, 0.6f, 1),
                    "NavMesh ready%s%s",
                    src.empty() ? "" : "  source: ",
                    src.empty() ? "" : src.c_str());
            }
            else
            {
                ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.8f, 1),
                                    "No navmesh loaded.");
            }
            ImGui::Separator();

            ImGui::TextDisabled("Recast / agent parameters");
            ImGui::SliderFloat("Cell size",       &s_navParams.cellSize,        0.05f, 1.0f, "%.2f");
            ImGui::SliderFloat("Cell height",     &s_navParams.cellHeight,      0.05f, 1.0f, "%.2f");
            ImGui::SliderFloat("Agent height",    &s_navParams.agentHeight,     0.5f, 4.0f, "%.2f");
            ImGui::SliderFloat("Agent radius",    &s_navParams.agentRadius,     0.1f, 2.0f, "%.2f");
            ImGui::SliderFloat("Agent max climb", &s_navParams.agentMaxClimb,   0.05f, 2.0f, "%.2f");
            ImGui::SliderFloat("Max slope (deg)", &s_navParams.agentMaxSlopeDeg,5.0f, 75.0f, "%.0f");
            int minRegion = s_navParams.minRegionArea;
            if (ImGui::InputInt("Min region area (cells^2)", &minRegion))
                s_navParams.minRegionArea = std::max(0, minRegion);

            // Tile size in cells. 0 = single-tile (fastest for small levels).
            // 32/64/128 = tile-based — Recast splits the world into a grid
            // of (tileSize × cellSize) world units. Use this for >1 km² worlds.
            int tileSize = s_navParams.tileSize;
            if (ImGui::SliderInt("Tile size (cells, 0 = single tile)",
                                  &tileSize, 0, 256))
                s_navParams.tileSize = std::max(0, tileSize);
            if (s_navParams.tileSize > 1)
                ImGui::TextDisabled("  tile world size ≈ %.1f m",
                    s_navParams.tileSize * s_navParams.cellSize);
            ImGui::Separator();

            ImGui::InputText("Output .inav path", s_navOut, sizeof(s_navOut));

            // ---- Action row ------------------------------------------
            const bool canBuild = hasWorld && hasNav;
            ImGui::BeginDisabled(!canBuild);
            if (ImGui::Button("Build NavMesh"))
            {
                Nav::TriangleSoup soup;
                if (Nav::BuildSoupFromWorldColliders(*m_world, soup))
                {
                    s_navStats = m_navSys->Build(soup, s_navParams);
                    if (s_navStats.success && s_navOut[0])
                        m_navSys->Save(s_navOut);
                }
                else
                {
                    s_navStats = {};
                    s_navStats.success = false;
                    s_navStats.error   = "no Mesh ColliderComponents in world — "
                                          "apply collision first via "
                                          "Bake Collision Meshes → Use baked as collision";
                    LOG_WARNING("NavMesh: nothing to feed Recast — apply collision first");
                }
            }
            ImGui::EndDisabled();

            ImGui::SameLine();
            ImGui::BeginDisabled(!hasNav || !m_navSys->IsReady() || s_navOut[0] == '\0');
            if (ImGui::Button("Save"))
                m_navSys->Save(s_navOut);
            ImGui::EndDisabled();

            ImGui::SameLine();
            ImGui::SetNextItemWidth(220);
            ImGui::InputText("##loadpath", s_navLoadPath, sizeof(s_navLoadPath));
            ImGui::SameLine();
            ImGui::BeginDisabled(!hasNav || s_navLoadPath[0] == '\0');
            if (ImGui::Button("Load"))
            {
                if (m_navSys->Load(s_navLoadPath))
                {
                    s_navStats = {};   // existing stats from a previous Build no longer match
                    s_navStats.success = true;
                }
            }
            ImGui::EndDisabled();

            ImGui::SameLine();
            ImGui::BeginDisabled(!hasNav || !m_navSys->IsReady());
            if (ImGui::Button("Clear"))
            {
                m_navSys->Clear();
                s_navStats = {};
            }
            ImGui::EndDisabled();

            ImGui::SameLine();
            if (ImGui::Button("Close"))
                ImGui::CloseCurrentPopup();

            // ---- Result line -----------------------------------------
            if (s_navStats.inputTris > 0 || !s_navStats.error.empty() || s_navStats.success)
            {
                ImGui::Separator();
                if (s_navStats.success && s_navStats.inputTris > 0)
                    ImGui::TextColored(ImVec4(0.6f, 1.f, 0.6f, 1),
                        "OK   in: %u verts %u tris    out: %u polys, %u/%u tiles    (%.1f ms)",
                        s_navStats.inputVerts, s_navStats.inputTris,
                        s_navStats.polyCount,
                        s_navStats.tilesBuilt, s_navStats.tilesTotal,
                        s_navStats.buildMs);
                else if (s_navStats.success)
                    ImGui::TextColored(ImVec4(0.6f, 0.9f, 1.f, 1),
                        "Loaded.");
                else
                    ImGui::TextColored(ImVec4(1, 0.5f, 0.5f, 1),
                        "FAIL: %s", s_navStats.error.c_str());
            }

            ImGui::EndPopup();
        }
    }

    ImGuiID dockspaceId = ImGui::GetID("EditorDockSpace");
    ImGui::DockSpace(dockspaceId, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_None);

    // Default layout: left Hierarchy | center Viewport+Resource | right Inspector.
    // Only build it when imgui.ini didn't already restore a dock tree — otherwise
    // we'd wipe the user's runtime layout on every launch.
    if (!m_layoutInitialized)
    {
        m_layoutInitialized = true;

        ImGuiDockNode* existing = ImGui::DockBuilderGetNode(dockspaceId);
        const bool needDefault = (existing == nullptr) || !existing->IsSplitNode();
        if (needDefault)
        {
            ImGui::DockBuilderRemoveNodeChildNodes(dockspaceId);
            ImGui::DockBuilderSetNodeSize(dockspaceId, viewport->WorkSize);

            ImGuiID leftId, rightPartId;
            ImGui::DockBuilderSplitNode(dockspaceId, ImGuiDir_Left, 0.25f, &leftId, &rightPartId);

            ImGuiID inspectorId, centerPartId;
            ImGui::DockBuilderSplitNode(rightPartId, ImGuiDir_Right, 0.3f, &inspectorId, &centerPartId);

            ImGuiID resourceId, viewportId;
            ImGui::DockBuilderSplitNode(centerPartId, ImGuiDir_Down, 0.2f, &resourceId, &viewportId);

            ImGui::DockBuilderDockWindow(kWindowHierarchy, leftId);
            ImGui::DockBuilderDockWindow(kWindowViewport, viewportId);
            ImGui::DockBuilderDockWindow(kWindowInspector, inspectorId);
            ImGui::DockBuilderDockWindow(kWindowResource, resourceId);
            ImGui::DockBuilderFinish(dockspaceId);
        }
    }

    ImGui::End();
}

void EditorLayer::RenderHierarchyPanel()
{
    ImGui::Begin(kWindowHierarchy);

    if (!m_world)
    {
        ImGui::TextDisabled("(no active scene)");
        ImGui::End();
        return;
    }

    // ---- "+" button: create a new empty root entity --------------------------
    if (ImGui::Button("+ Entity"))
    {
        Entity ne = m_world->CreateEntity();
        m_world->SetName(ne, "Entity");
        m_world->AddComponent<LocalTransform>(ne, LocalTransform{});
        m_world->AddComponent<GlobalTransform>(ne, GlobalTransform{});
        m_selectedEntity = ne;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    ImGui::TextDisabled("Right-click entity for options");
    ImGui::Separator();

    // Show roots only; children drawn recursively via DrawEntityTree.
    s_pendingDelete      = NullEntity;
    s_pendingCreateChild = NullEntity;
    s_pendingDuplicate   = NullEntity;
    s_pendingPrefabSave  = NullEntity;
    s_pendingWrapVisual  = NullEntity;
    s_pendingReparent    = {};

    // Build per-frame logical hierarchy map (Parent + FollowSocket + FollowEntity)
    // with cycle detection. See HierarchyMap for the full design.
    const HierarchyMap hmap = BuildHierarchyMap(m_world);

    // Render roots in stable order: anyone whose logical parent is NullEntity
    // (no Parent, no valid Follow target, OR a cycle that got severed).
    std::vector<Entity> roots;
    for (Entity e : m_world->GetEntities())
    {
        if (!m_world->IsAlive(e)) continue;
        auto it = hmap.parent.find(e);
        const Entity p = (it != hmap.parent.end()) ? it->second : NullEntity;
        if (p == NullEntity) roots.push_back(e);
    }
    std::sort(roots.begin(), roots.end());

    for (Entity e : roots)
        DrawEntityTree(m_world, e, m_selectedEntity, m_hierarchyHighlight, hmap);

    // ---- Root-level drop zone — dragging an entity here detaches it from
    // its parent and makes it a top-level entity. Sized to fill the remaining
    // space so users can drop anywhere below the last root.
    {
        const float remaining = std::max(40.0f, ImGui::GetContentRegionAvail().y);
        ImGui::InvisibleButton("##root_drop_zone", ImVec2(-1.f, remaining));
        if (ImGui::BeginDragDropTarget())
        {
            if (const ImGuiPayload* payload =
                    ImGui::AcceptDragDropPayload("ENTITY"))
            {
                const Entity dragged = *static_cast<const Entity*>(payload->Data);
                s_pendingReparent.child     = dragged;
                s_pendingReparent.newParent = NullEntity;
            }
            ImGui::EndDragDropTarget();
        }
        // Light visual hint while a drag is active over the zone.
        if (ImGui::IsItemHovered() && ImGui::GetDragDropPayload() &&
            ImGui::GetDragDropPayload()->IsDataType("ENTITY"))
        {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImVec2 mn = ImGui::GetItemRectMin();
            const ImVec2 mx = ImGui::GetItemRectMax();
            dl->AddRect(mn, mx, IM_COL32(120, 200, 255, 200), 2.0f, 0, 1.5f);
        }
    }

    // ---- Process pending operations (after tree draw to avoid iterator issues)
    if (s_pendingCreateChild != NullEntity)
    {
        Entity parent = s_pendingCreateChild;
        Entity child  = m_world->CreateEntity();
        m_world->SetName(child, "Entity");
        m_world->AddComponent<LocalTransform>(child, LocalTransform{});
        m_world->AddComponent<GlobalTransform>(child, GlobalTransform{});

        Parent pc;  pc.entity = parent;
        m_world->AddComponent<Parent>(child, pc);

        Children* ch = m_world->GetComponent<Children>(parent);
        if (ch)
            ch->entities.push_back(child);
        else
        {
            Children newCh;  newCh.entities.push_back(child);
            m_world->AddComponent<Children>(parent, std::move(newCh));
        }
        m_selectedEntity     = child;
        s_pendingCreateChild = NullEntity;
    }

    if (s_pendingDelete != NullEntity)
    {
        if (m_selectedEntity == s_pendingDelete)
            m_selectedEntity = NullEntity;
        DestroyEntityRecursive(m_world, s_pendingDelete);
        s_pendingDelete = NullEntity;
    }

    if (s_pendingDuplicate != NullEntity)
    {
        // Sibling duplicate: cloned entity inherits source's parent. User re-parents via drag-drop later.
        Entity src       = s_pendingDuplicate;
        Entity srcParent = NullEntity;
        if (const Parent* p = m_world->GetComponent<Parent>(src); p)
            srcParent = p->entity;

        const Entity dup = CloneEntityRecursive(m_world, m_assetMgr, src, srcParent);
        m_selectedEntity = dup;
        LOG_INFO("EditorLayer: duplicated entity %u → %u ('%s')",
                 src, dup, m_world->GetName(dup).c_str());
        s_pendingDuplicate = NullEntity;
    }

    if (s_pendingPrefabSave != NullEntity)
    {
        const Entity src = s_pendingPrefabSave;
        if (m_world->IsAlive(src))
        {
            static const char* kPrefabFilter = "Prefab (*.ipfb)\0*.ipfb\0All Files\0*.*\0\0";
            const std::string savePath = SaveFileDialog(
                kPrefabFilter, "ipfb",
                m_assetDir.empty() ? nullptr : m_assetDir.c_str());
            if (!savePath.empty())
            {
                if (Resource::SavePrefab(src, *m_world, savePath))
                {
                    LOG_SUCCESS("EditorLayer: saved prefab '%s'", savePath.c_str());
                    m_needsRescan = true;
                }
                else
                    LOG_ERROR("EditorLayer: failed to save prefab '%s'", savePath.c_str());
            }
        }
        s_pendingPrefabSave = NullEntity;
    }

    // ---- Reparent (drag-drop / menu) — preserves world transform -----------
    if (s_pendingReparent.child != NullEntity
        && m_world->IsAlive(s_pendingReparent.child))
    {
        const Entity c  = s_pendingReparent.child;
        const Entity np = s_pendingReparent.newParent;
        if (np != NullEntity && !m_world->IsAlive(np))
        {
            LOG_WARNING("EditorLayer: reparent target %u is dead — ignored", np);
        }
        else if (np != NullEntity && WouldCreateCycle(m_world, c, np))
        {
            LOG_WARNING("EditorLayer: reparenting %u under %u would form a cycle — ignored",
                        c, np);
        }
        else
        {
            ReparentEntity(m_world, c, np);
            LOG_INFO("EditorLayer: reparented entity %u under %u",
                     c, static_cast<uint32_t>(np));
        }
        s_pendingReparent = {};
    }

    // ---- Wrap Visual as Child ----------------------------------------------
    // Moves render-only components from the parent entity into a freshly
    // created child positioned at the collider's "Snap to Bottom" Y. Keeps
    // navigation / physics state on the parent (the entity origin sits at
    // ground level after this), while the visual mesh sits half-height up.
    if (s_pendingWrapVisual != NullEntity
        && m_world->IsAlive(s_pendingWrapVisual))
    {
        const Entity parent = s_pendingWrapVisual;

        // Y offset mirrors the Snap-to-Bottom preset on ColliderComponent so
        // collider top and visual centre line up.
        float yLift = 0.0f;
        if (const ColliderComponent* col =
                m_world->GetComponent<ColliderComponent>(parent))
        {
            switch (col->shape)
            {
                case ColliderComponent::Shape::Capsule: yLift = col->halfHeight + col->radius; break;
                case ColliderComponent::Shape::Sphere:  yLift = col->radius;                   break;
                case ColliderComponent::Shape::Box:     yLift = col->halfExtents.y;            break;
                case ColliderComponent::Shape::Mesh:    yLift = 0.0f;                          break;
            }
        }

        const Entity child = m_world->CreateEntity();
        const std::string childName = m_world->GetName(parent) + "_Visual";
        m_world->SetName(child, childName);

        LocalTransform childLT;
        childLT.translation = { 0.f, yLift, 0.f };
        m_world->AddComponent<LocalTransform>(child, childLT);
        m_world->AddComponent<GlobalTransform>(child, GlobalTransform{});
        m_world->AddComponent<Parent>(child, Parent{ parent });
        if (Children* ch = m_world->GetComponent<Children>(parent))
            ch->entities.push_back(child);
        else
        {
            Children c; c.entities.push_back(child);
            m_world->AddComponent<Children>(parent, std::move(c));
        }

        // Move every render-side component the renderer cares about. The
        // collider / RigidBody / AIIntent + NavAgent stay on the parent so physics +
        // pathing keep operating from the entity origin (= ground level).
        int moved = 0;
        moved += MoveComponentBetween<MeshHandle>        (m_world, parent, child) ? 1 : 0;
        moved += MoveComponentBetween<MeshLibRef>        (m_world, parent, child) ? 1 : 0;
        moved += MoveComponentBetween<MeshSourcePath>    (m_world, parent, child) ? 1 : 0;
        moved += MoveComponentBetween<MaterialComponent> (m_world, parent, child) ? 1 : 0;
        moved += MoveComponentBetween<MaterialSourcePath>(m_world, parent, child) ? 1 : 0;
        moved += MoveComponentBetween<MaterialOverride>  (m_world, parent, child) ? 1 : 0;
        moved += MoveComponentBetween<LocalAabb>         (m_world, parent, child) ? 1 : 0;
        moved += MoveComponentBetween<WorldAabb>         (m_world, parent, child) ? 1 : 0;

        m_selectedEntity    = child;
        s_pendingWrapVisual = NullEntity;
        LOG_INFO("EditorLayer: wrapped %d visual component(s) from entity %u into child %u (lift Y=%.3f)",
                 moved, parent, child, yLift);
    }

    // ---- Visual hint when an IPFB drag is active ---------------------------
    if (ImGui::GetDragDropPayload() &&
        ImGui::GetDragDropPayload()->IsDataType("IPFB_PATH"))
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImGuiWindow* win = ImGui::GetCurrentWindow();
        const ImVec2 bMin = win->InnerRect.Min;
        const ImVec2 bMax = win->InnerRect.Max;
        dl->AddRectFilled(bMin, bMax, IM_COL32(30, 80, 30, 60), 3.f);
        dl->AddRect(bMin, bMax, IM_COL32(70, 180, 70, 200), 3.f);
        const char* hint = "Drop .ipfb to spawn prefab";
        const ImVec2 tsz = ImGui::CalcTextSize(hint);
        dl->AddText(
            ImVec2(bMin.x + (bMax.x - bMin.x - tsz.x) * 0.5f,
                   bMax.y - tsz.y - 6.f),
            IM_COL32(120, 220, 120, 230), hint);
    }

    // ---- Full-window IPFB drop target --------------------------------------
    {
        ImGuiWindow* win = ImGui::GetCurrentWindow();
        if (ImGui::BeginDragDropTargetCustom(win->InnerRect, win->ID + 1))
        {
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("IPFB_PATH"))
            {
                const char* pfbPath = static_cast<const char*>(p->Data);
                if (m_assetMgr && m_world)
                {
                    Entity spawned = Resource::LoadPrefab(pfbPath, *m_world, *m_assetMgr,
                                                        m_renderer, m_animClipSys);
                    if (spawned != NullEntity)
                    {
                        m_selectedEntity = spawned;
                        LOG_SUCCESS("EditorLayer: spawned prefab '%s' — root entity %u", pfbPath, spawned);
                    }
                    else
                        LOG_ERROR("EditorLayer: failed to spawn prefab '%s'", pfbPath);
                }
                else
                    LOG_ERROR("EditorLayer: assetMgr not wired — cannot spawn prefab");
            }
            ImGui::EndDragDropTarget();
        }
    }

    ImGui::End();
}

void EditorLayer::RenderViewportToolbar()
{
    const float padding     = 5.0f;
    const float buttonSize  = 25.0f;
    const float itemSpacing = 2.0f;

    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(padding, padding));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,  ImVec2(itemSpacing, 0.0f));

    auto iconOrLabel = [this](const char* icon, const char* label) { return m_hasFontAwesomeIcons ? icon : label; };

    // ---- Gizmo operation buttons (left-aligned) ----------------------------
    auto gizmoBtn = [&](const char* id, ImGuizmo::OPERATION op, const char* tooltip)
    {
        const bool active = (m_gizmoOperation == op);
        if (active)
            ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        if (ImGui::Button(id, ImVec2(buttonSize, buttonSize)))
            m_gizmoOperation = op;
        if (active)
            ImGui::PopStyleColor();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("%s", tooltip);
    };

    gizmoBtn("T##gz", ImGuizmo::TRANSLATE, "Translate [W]");
    ImGui::SameLine();
    gizmoBtn("R##gz", ImGuizmo::ROTATE,    "Rotate [E]");
    ImGui::SameLine();
    gizmoBtn("S##gz", ImGuizmo::SCALE,     "Scale [R]");
    ImGui::SameLine();

    // ---- Play/Pause/Stop buttons (centered in remaining space) -------------
    const int   playCount    = 3;
    const float playTotal    = playCount * buttonSize + (playCount - 1) * itemSpacing;
    const float leftEdge     = ImGui::GetCursorPosX();
    const float remaining    = ImGui::GetContentRegionAvail().x;
    const float centerOffset = (remaining - playTotal) * 0.5f;
    ImGui::SetCursorPosX(leftEdge + std::max(0.f, centerOffset));

    const bool isPlaying = (m_playState == ViewportPlayState::Playing);
    if (ImGui::Button(iconOrLabel(isPlaying ? FontAwesome::kPause : FontAwesome::kPlay,
                                  isPlaying ? "Pause" : "Play"),
                      ImVec2(buttonSize, buttonSize)))
    {
        m_playState = isPlaying ? ViewportPlayState::Paused : ViewportPlayState::Playing;
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip(isPlaying ? "Pause" : "Play");

    ImGui::SameLine();

    if (ImGui::Button(iconOrLabel(FontAwesome::kStop, "Stop"), ImVec2(buttonSize, buttonSize)))
        m_playState = ViewportPlayState::Stopped;
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Stop");

    ImGui::SameLine();

    if (ImGui::Button(iconOrLabel(FontAwesome::kStepForward, "Step"), ImVec2(buttonSize, buttonSize)))
        m_wantsStepFrame = true;
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Update 1 frame");

    // ---- Global View Mode dropdown (right-aligned on the toolbar row) ------
    // Lit / Unlit / Wireframe — applies to ALL objects. Wireframe additionally
    // suppresses sky/clouds/fog/TAA in the Renderer for a clean dark-bg result.
    if (m_renderer)
    {
        static const char* kViewModes[] = { "Lit", "Unlit", "Wireframe" };
        const float comboW = 110.0f;
        ImGui::SameLine();
        const float rightX = ImGui::GetCursorPosX()
                           + ImGui::GetContentRegionAvail().x - comboW;
        ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), rightX));
        ImGui::SetNextItemWidth(comboW);
        int vm = (int)m_renderer->GetViewMode();
        if (ImGui::Combo("##viewmode", &vm, kViewModes, IM_ARRAYSIZE(kViewModes)))
            m_renderer->SetViewMode((Renderer::ViewMode)vm);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("View Mode: Lit / Unlit / Wireframe");
    }

    ImGui::PopStyleVar(2);
}

void EditorLayer::RenderViewportPanel()
{
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin(kWindowViewport);
    ImGui::PopStyleVar();

    // Toolbar: Play / Pause / Stop (with normal padding for this section)
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(50.0f, 4.0f));
    RenderViewportToolbar();
    ImGui::PopStyleVar();
    ImGui::Separator();

    ImVec2 size = ImGui::GetContentRegionAvail();
    if (size.x > 0 && size.y > 0)
    {
        m_viewportSizeX = static_cast<unsigned int>(size.x);
        m_viewportSizeY = static_cast<unsigned int>(size.y);
        if (m_viewportSizeX == 0) m_viewportSizeX = 1;
        if (m_viewportSizeY == 0) m_viewportSizeY = 1;
    }
    if (size.x > 0 && size.y > 0)
    {
        const float contentW = size.x;
        const float contentH = size.y;

        // Pre-compute viewport rect (vpMin/vpMax used by both draw list and ImGuizmo).
        const ImVec2 vpMin = ImGui::GetCursorScreenPos();
        const ImVec2 vpMax = ImVec2(vpMin.x + contentW, vpMin.y + contentH);
        m_viewportMin = vpMin;  // cached for material drag-drop pick coordinate

        // ---- Draw scene texture / background (draw list, no item created) --
        ImDrawList* vpDL = ImGui::GetWindowDrawList();
        if (m_sceneTextureId != 0)
        {
            vpDL->AddImage(
                ImTextureRef(static_cast<ImTextureID>(m_sceneTextureId)),
                vpMin, vpMax);
        }
        else
        {
            vpDL->AddRectFilled(vpMin, vpMax, IM_COL32(40, 44, 52, 255));
            const char* hint = "Drag .imsh or .iscn here to spawn";
            ImVec2 tsz = ImGui::CalcTextSize(hint);
            vpDL->AddText(
                ImVec2(vpMin.x + (size.x - tsz.x) * 0.5f,
                       vpMin.y + (size.y - tsz.y) * 0.5f),
                IM_COL32(120, 120, 140, 200), hint);
        }

        ImGui::SetCursorScreenPos(vpMin);
        RenderViewportGizmo(vpMin, vpMax);

        ImGui::SetCursorScreenPos(vpMin);
        ImGui::SetNextItemAllowOverlap();
        ImGui::InvisibleButton("##viewport", size,
            ImGuiButtonFlags_MouseButtonLeft |
            ImGuiButtonFlags_MouseButtonRight |
            ImGuiButtonFlags_MouseButtonMiddle);

        const bool vpClicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
        const bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
        // ImGuizmo overlay must come before InvisibleButton — CanActivate() requires no item hovered/active.

        // Highlight drop zone when a drag is in flight

        if (ImGui::IsDragDropActive())
        {
            vpDL->AddRectFilled(vpMin, vpMax, IM_COL32(60, 120, 255, 25));
            vpDL->AddRect(vpMin, vpMax, IM_COL32(80, 160, 255, 180), 0.f, 0, 2.f);
        }

        // ---- Drag-drop target: accept .iscn and .imsh drops ---------------
        if (ImGui::BeginDragDropTarget())
        {
            if (const ImGuiPayload* payload =
                    ImGui::AcceptDragDropPayload("ISCN_PATH"))
            {
                const char* path = static_cast<const char*>(payload->Data);
                if (!m_world || !m_assetMgr)
                    LOG_ERROR("EditorLayer: world/assetMgr not wired — cannot spawn scene");
                else
                {
                    Resource::MeshLibrary* meshLib = m_renderer
                        ? m_renderer->GetMeshLibrary() : nullptr;
                    if (!meshLib)
                    {
                        LOG_ERROR("EditorLayer: MeshLibrary not wired — cannot spawn scene");
                    }
                    else
                    {
                        auto res = SceneInstanceLoader::Load(path, *m_world, *m_assetMgr, *meshLib, m_renderer);
                        if (res.success)
                        {
                            m_selectedEntity = res.rootEntity;
                            LOG_SUCCESS("EditorLayer: spawned scene '%s' — %u nodes, %u meshes",
                                        path, res.nodeCount, res.meshEntityCount);
                        }
                        else
                            LOG_ERROR("EditorLayer: failed to spawn scene '%s'", path);
                    }
                }
            }

            // IMSH_PATH drop removed (P1-P6 rewrite); .iscn drop now handles single-mesh spawn.

            // Material drop: request a pick at the drop location, apply when resolved.
            if (const ImGuiPayload* payload =
                    ImGui::AcceptDragDropPayload("IMAT_PATH"))
            {
                const char* path = static_cast<const char*>(payload->Data);
                m_pendingMatDropPath = path;
                // Pick at viewport-relative mouse pos; m_viewportMin reliable inside drag-drop.
                const ImVec2 mousePos = ImGui::GetMousePos();
                if (m_pickCallback)
                    m_pickCallback(mousePos.x - m_viewportMin.x,
                                   mousePos.y - m_viewportMin.y);
                m_hasPendingDrag = true;
            }

            // Animation drop: begin async load, resolve when clip is ready.
            if (const ImGuiPayload* payload =
                    ImGui::AcceptDragDropPayload("IANIM_PATH"))
            {
                const char* path = static_cast<const char*>(payload->Data);
                if (!m_world || !m_animClipSys)
                    LOG_ERROR("EditorLayer: world/animClipSys not wired — cannot assign animation");
                else if (m_selectedEntity == NullEntity)
                    LOG_WARNING("EditorLayer: no entity selected — select an entity first");
                else
                {
                    Resource::AnimHandle ah = m_animClipSys->AcquireClip(path);
                    m_pendingAnimDrops.push_back({ ah, m_selectedEntity, std::string(path) });
                    LOG_INFO("EditorLayer: queued animation '%s' for entity %u", path, m_selectedEntity);
                }
            }

            ImGui::EndDragDropTarget();
        }

        // Right-mouse drag → camera rotation.
        m_viewportRightDragging = hovered && ImGui::IsMouseDown(ImGuiMouseButton_Right);
        if (m_viewportRightDragging)
        {
            const ImVec2 delta = ImGui::GetIO().MouseDelta;
            m_vpMouseDX = delta.x;
            m_vpMouseDY = delta.y;
        }
        else
        {
            m_vpMouseDX = 0.f;
            m_vpMouseDY = 0.f;
        }

        // Left-click picking (guarded by ImGuizmo::IsOver to avoid gizmo conflicts).
        HandleViewportPicking(vpMin.x, vpMin.y, contentW, contentH, hovered);

        // Keyboard shortcuts for gizmo operation (when viewport is hovered)
        if (hovered || ImGuizmo::IsUsing())
        {
            if (ImGui::IsKeyPressed(ImGuiKey_W)) m_gizmoOperation = ImGuizmo::TRANSLATE;
            if (ImGui::IsKeyPressed(ImGuiKey_E)) m_gizmoOperation = ImGuizmo::ROTATE;
            if (ImGui::IsKeyPressed(ImGuiKey_R)) m_gizmoOperation = ImGuizmo::SCALE;
        }

        // Tracer test: T fires along view ray. Gated on m_tracerTestModeEnabled + hover.
        if (hovered
            && m_tracerTestModeEnabled
            && m_renderer
            && ImGui::IsKeyPressed(ImGuiKey_T, /*repeat=*/false))
        {
            if (TracerSystem* ts = m_renderer->GetTracerSystem())
            {
                using DirectX::XMFLOAT3;
                using DirectX::XMFLOAT4;
                const XMFLOAT3& cp = m_renderView.cameraPosition;
                const XMFLOAT3& cf = m_renderView.cameraForward;
                XMFLOAT3 start{ cp.x + cf.x * 0.5f,
                                cp.y + cf.y * 0.5f,
                                cp.z + cf.z * 0.5f };
                XMFLOAT3 end  { cp.x + cf.x * 30.0f,
                                cp.y + cf.y * 30.0f,
                                cp.z + cf.z * 30.0f };
                ts->Spawn(start, end,
                          XMFLOAT4{ 1.0f, 0.85f, 0.3f, 6.0f },
                          /*width=*/0.5f,
                          /*lifetime=*/0.8f);
            }
        }
    }

    ImGui::End();
}

void EditorLayer::HandleViewportPicking(float imageMinX, float imageMinY,
                                        float contentW,  float contentH,
                                        bool  hovered)
{
    if (contentW <= 0.f || contentH <= 0.f) return;

    const ImGuiIO& io      = ImGui::GetIO();
    const float    px      = io.MousePos.x - imageMinX;
    const float    py      = io.MousePos.y - imageMinY;
    const bool     rightHeld = ImGui::IsMouseDown(ImGuiMouseButton_Right);

    // ---- Left-click → GPU pick callback (skip when gizmo handles are active). ----
    if (hovered && !rightHeld && ImGui::IsMouseClicked(ImGuiMouseButton_Left)
        && !ImGuizmo::IsOver() && contentW > 0.f && contentH > 0.f)
    {
        if (m_pickCallback)
            m_pickCallback(px, py);
        m_hasPendingDrag = true;
        m_isDragging = false;  // drag starts when pick result arrives
    }

    // ---- Left-hold: move entity on drag plane (off while gizmo is manipulating). ----
    if (m_isDragging
        && ImGui::IsMouseDown(ImGuiMouseButton_Left)
        && !ImGuizmo::IsUsing()
        && m_dragEntity != NullEntity
        && m_world)
    {
        const Ray ray = Ray::FromViewport(px, py, contentW, contentH, m_renderView);

        float t;
        if (ray.IntersectPlane(m_dragPlanePoint, m_dragPlaneNormal, t))
        {
            const XMFLOAT3 newHit = ray.At(t);
            if (LocalTransform* lt = m_world->GetComponent<LocalTransform>(m_dragEntity))
            {
                lt->translation.x = newHit.x + m_dragEntityOffset.x;
                lt->translation.y = newHit.y + m_dragEntityOffset.y;
                lt->translation.z = newHit.z + m_dragEntityOffset.z;
            }
        }
    }

    // ---- Release: end drag -------------------------------------------------
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left))
        m_isDragging = false;
}

void EditorLayer::RenderViewportGizmo(ImVec2 vpMin, ImVec2 vpMax)
{
    if (m_selectedEntity == NullEntity || !m_world || !m_world->IsAlive(m_selectedEntity))
        return;

    // GlobalTransform is the single source of truth (DX row-major matches ImGuizmo).
    const GlobalTransform* gt = m_world->GetComponent<GlobalTransform>(m_selectedEntity);
    if (!gt) return;
    const DirectX::XMFLOAT4X4 worldMatrix = gt->matrix;

    // Frustum-cull: ImGuizmo's foreground draw list pins handles to the screen edge for off-screen entities otherwise.
    {
        using namespace DirectX;
        const XMMATRIX view = XMLoadFloat4x4(&m_renderView.viewMatrix);
        const XMMATRIX proj = XMLoadFloat4x4(&m_renderView.projMatrixNoJitter);
        const XMMATRIX vp   = XMMatrixMultiply(view, proj);
        const XMVECTOR worldPos = XMVectorSet(worldMatrix._41, worldMatrix._42,
                                              worldMatrix._43, 1.0f);
        const XMVECTOR clipPos  = XMVector4Transform(worldPos, vp);
        const float w = XMVectorGetW(clipPos);
        if (w <= 0.0f) return;                           // behind camera
        const float ndcX = XMVectorGetX(clipPos) / w;
        const float ndcY = XMVectorGetY(clipPos) / w;
        // 1.5x margin keeps gizmo arms visible for near-edge entities so handles don't snap off mid-drag.
        const float kMargin = 1.5f;
        if (ndcX < -kMargin || ndcX > kMargin ||
            ndcY < -kMargin || ndcY > kMargin) return;
    }

    // Freeze ImGuizmo input while hovered/dragged — float drift from TransformSystem flickers hit-tests.
    if (m_selectedEntity != m_gizmoCachedEntity)
    {
        m_gizmoCachedEntity = m_selectedEntity;
        m_gizmoWorldMatrix  = worldMatrix;
    }
    else if (!ImGuizmo::IsUsing() && !ImGuizmo::IsOver())
    {
        m_gizmoWorldMatrix = worldMatrix;
    }

    float worldMut[16];
    memcpy(worldMut, &m_gizmoWorldMatrix, sizeof(worldMut));

    const ImGuizmo::MODE mode = ImGuizmo::LOCAL;

    ImGuizmo::SetOrthographic(false);
    ImGuizmo::SetRect(vpMin.x, vpMin.y, vpMax.x - vpMin.x, vpMax.y - vpMin.y);
    // Foreground drawlist + SetAlternativeWindow — needed for correct hover resolution in docked layouts.
    ImGuizmo::SetDrawlist(ImGui::GetForegroundDrawList());
    ImGuizmo::SetAlternativeWindow(ImGui::GetCurrentWindow());

    if (!ImGuizmo::Manipulate(
            reinterpret_cast<const float*>(&m_renderView.viewMatrix),
            reinterpret_cast<const float*>(&m_renderView.projMatrixNoJitter),
            m_gizmoOperation, mode, worldMut))
        return;

    // Only apply if left mouse is actually held — prevents hover-only activation.
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
        return;

    // worldMut is now the modified world matrix.
    XMFLOAT4X4 newWorld;
    memcpy(&newWorld, worldMut, sizeof(newWorld));

    if (LocalTransform* lt = m_world->GetComponent<LocalTransform>(m_selectedEntity))
    {
        const Parent* par = m_world->GetComponent<Parent>(m_selectedEntity);
        const GlobalTransform* pgt = (par && par->entity != NullEntity)
            ? m_world->GetComponent<GlobalTransform>(par->entity) : nullptr;

        if (pgt)
        {
            const XMMATRIX newLocal = XMMatrixMultiply(
                XMLoadFloat4x4(&newWorld),
                XMMatrixInverse(nullptr, XMLoadFloat4x4(&pgt->matrix)));

            XMVECTOR sVec, qVec, tVec;
            if (XMMatrixDecompose(&sVec, &qVec, &tVec, newLocal))
            {
                XMStoreFloat3(&lt->translation, tVec);
                XMStoreFloat4(&lt->rotation, qVec);
                XMStoreFloat3(&lt->scale, sVec);
            }
        }
        else
        {
            XMVECTOR sVec, qVec, tVec;
            if (XMMatrixDecompose(&sVec, &qVec, &tVec, XMLoadFloat4x4(&newWorld)))
            {
                XMStoreFloat3(&lt->translation, tVec);
                XMStoreFloat4(&lt->rotation, qVec);
                XMStoreFloat3(&lt->scale, sVec);
            }
        }

        // Recompute matching GlobalTransform so m_gizmoWorldMatrix stays in sync (IsOver() won't oscillate).
        const XMMATRIX ltMat = lt->ToMatrix();
        const XMMATRIX expectedGT = pgt
            ? XMMatrixMultiply(ltMat, XMLoadFloat4x4(&pgt->matrix))
            : ltMat;

        XMStoreFloat4x4(&m_gizmoWorldMatrix, expectedGT);
        if (GlobalTransform* gt = m_world->GetComponent<GlobalTransform>(m_selectedEntity))
            XMStoreFloat4x4(&gt->matrix, expectedGT);
    }
}

void EditorLayer::RenderInspectorPanel()
{
    ImGui::Begin(kWindowInspector);

    if (!m_world || m_selectedEntity == NullEntity ||
        !m_world->IsAlive(m_selectedEntity))
    {
        ImGui::TextDisabled("Select an entity to inspect");
        ImGui::End();
        return;
    }

    // ---- Entity header ------------------------------------------------------
    const std::string& name = m_world->GetName(m_selectedEntity);
    ImGui::TextColored(ImVec4(1.f, 0.85f, 0.4f, 1.f), "%s", name.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("[id=%u]", m_selectedEntity);
    // If the entity carries a GuidComponent (i.e. something stable refs it
    // across save/load), surface its GUID next to the volatile Entity id —
    // copy-paste-able from the header without diving into the GUID
    // component row. Click the badge to copy the full hex to the clipboard.
    if (auto* gc = m_world->GetComponent<GuidComponent>(m_selectedEntity);
        gc && gc->guid.IsValid())
    {
        const std::string fullGuid = gc->guid.ToString();
        ImGui::SameLine();
        char shortBadge[24];
        // Show only the first 8 hex chars in the header — full GUID is
        // 36 chars and would dominate the line. Tooltip + click-to-copy
        // expose the full value when needed.
        std::snprintf(shortBadge, sizeof(shortBadge), "[guid:%.8s]", fullGuid.c_str());
        ImGui::TextDisabled("%s", shortBadge);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s\n(click to copy)", fullGuid.c_str());
        if (ImGui::IsItemClicked())
            ImGui::SetClipboardText(fullGuid.c_str());
    }
    ImGui::Separator();

    // ---- Component badge strip — pills for every type attached to entity. ----
    const auto typeIndices = m_world->GetComponentTypeIndices(m_selectedEntity);

    if (!typeIndices.empty())
    {
        ImGui::TextDisabled("Components (%zu):", typeIndices.size());

        // Sort by display label for stable order across frames.
        std::vector<std::pair<std::string, std::type_index>> sorted;
        sorted.reserve(typeIndices.size());
        for (const auto& ti : typeIndices)
        {
            auto it = m_componentLabels.find(ti);
            const std::string label = (it != m_componentLabels.end())
                ? it->second
                : StripTypePrefix(ti.name());
            sorted.push_back({ label, ti });
        }
        std::sort(sorted.begin(), sorted.end(),
                  [](const auto& a, const auto& b){ return a.first < b.first; });

        const float panelWidth  = ImGui::GetContentRegionAvail().x;
        const float badgePadX   = 6.f;
        const float badgePadY   = 2.f;
        const float spacing     = 4.f;
        const float lineH       = ImGui::GetTextLineHeight() + badgePadY * 2.f + 1.f;
        ImDrawList* drawList    = ImGui::GetWindowDrawList();
        const float startX      = ImGui::GetCursorScreenPos().x;
        float       screenX     = startX;
        float       screenY     = ImGui::GetCursorScreenPos().y;

        for (const auto& [label, ti] : sorted)
        {
            const float textW = ImGui::CalcTextSize(label.c_str()).x;
            const float badgeW = textW + badgePadX * 2.f;

            // Wrap to next line if needed
            if (screenX + badgeW > startX + panelWidth && screenX > startX)
            {
                screenX = startX;
                screenY += lineH + spacing;
            }

            const bool hasEditor = m_componentEditors.count(ti) > 0;
            const ImVec4 bgColF  = hasEditor
                ? ImVec4(0.25f, 0.45f, 0.70f, 0.85f)   // blue  = has editor
                : ImVec4(0.28f, 0.28f, 0.28f, 0.85f);  // gray  = tag only
            const ImU32 bgCol  = ImGui::ColorConvertFloat4ToU32(bgColF);
            const ImU32 txtCol = IM_COL32(230, 230, 230, 255);

            const ImVec2 bMin = { screenX,          screenY };
            const ImVec2 bMax = { screenX + badgeW, screenY + lineH };
            drawList->AddRectFilled(bMin, bMax, bgCol, 4.f);
            drawList->AddText({ bMin.x + badgePadX, bMin.y + badgePadY }, txtCol, label.c_str());

            screenX += badgeW + spacing;
        }

        // Advance the ImGui cursor past the drawn badges
        const float totalH = (screenY - ImGui::GetCursorScreenPos().y) + lineH + spacing;
        ImGui::Dummy(ImVec2(panelWidth, totalH));
    }

    ImGui::Separator();

    // ---- Registered component editors (collapsing headers); priority asc, label tiebreak. ----
    std::vector<ComponentEditorEntry*> activeEditors;
    for (auto& [typeIdx, entry] : m_componentEditors)
    {
        if (!entry.fetch(*m_world, m_selectedEntity)) continue;
        if (entry.condition && !entry.condition(*m_world, m_selectedEntity)) continue;
        activeEditors.push_back(&entry);
    }
    std::sort(activeEditors.begin(), activeEditors.end(),
              [](const ComponentEditorEntry* a, const ComponentEditorEntry* b){
                  if (a->priority != b->priority) return a->priority < b->priority;
                  return a->label < b->label;
              });

    // Track if we need to remove a component (deferred to avoid modifying during iteration)
    std::function<void()> pendingRemove;

    for (ComponentEditorEntry* entry : activeEditors)
    {
        void* compPtr = entry->fetch(*m_world, m_selectedEntity);
        ImGui::PushID(entry->label.c_str());
        const ImGuiTreeNodeFlags hdrFlags = entry->defaultCollapsed
                                          ? ImGuiTreeNodeFlags_None
                                          : ImGuiTreeNodeFlags_DefaultOpen;
        bool open = ImGui::CollapsingHeader(entry->label.c_str(), hdrFlags);

        // Right-click on the header to remove
        if (entry->remove && ImGui::BeginPopupContextItem("##comp_ctx"))
        {
            if (ImGui::MenuItem("Remove Component"))
            {
                auto removeFn = entry->remove;
                Entity ent    = m_selectedEntity;
                pendingRemove = [this, removeFn, ent]() { removeFn(*m_world, ent); };
            }
            ImGui::EndPopup();
        }

        if (open)
            entry->draw(compPtr, m_world, m_selectedEntity);
        ImGui::PopID();
    }

    if (pendingRemove) pendingRemove();

    // ---- Built-in: Material editor -----------------------------------------
    MaterialComponent* mat = m_world->GetComponent<MaterialComponent>(m_selectedEntity);
    if (mat)
    {
        ImGui::PushID("__material__");
        if (ImGui::CollapsingHeader("Material", ImGuiTreeNodeFlags_DefaultOpen))
            RenderMaterialInspector(*mat);
        ImGui::PopID();
    }

    // ---- Animation clip drop zone (only for entities with SkeletonComponent) --
    if (m_world && m_world->HasComponent<SkeletonComponent>(m_selectedEntity))
    {
        ImGui::Separator();

        const ImVec2 dropSz  = ImVec2(ImGui::GetContentRegionAvail().x, 28.f);
        const ImVec2 dropMin = ImGui::GetCursorScreenPos();
        const ImVec2 dropMax = { dropMin.x + dropSz.x, dropMin.y + dropSz.y };

        const bool isDragActive = ImGui::GetDragDropPayload() &&
                                  ImGui::GetDragDropPayload()->IsDataType("IANIM_PATH");
        const ImU32 bgCol = isDragActive
            ? IM_COL32(40, 100, 40, 160)
            : IM_COL32(40, 40, 40, 80);
        ImGui::GetWindowDrawList()->AddRectFilled(dropMin, dropMax, bgCol, 4.f);
        ImGui::GetWindowDrawList()->AddRect(dropMin, dropMax,
            isDragActive ? IM_COL32(80, 200, 80, 200) : IM_COL32(80, 80, 80, 120), 4.f);
        const char* hint = isDragActive ? "Release to assign animation" : "Drop .ianim here to assign animation";
        const ImVec2 tsz  = ImGui::CalcTextSize(hint);
        ImGui::GetWindowDrawList()->AddText(
            { dropMin.x + (dropSz.x - tsz.x) * 0.5f, dropMin.y + (dropSz.y - tsz.y) * 0.5f },
            IM_COL32(180, 180, 180, 200), hint);

        ImGui::Dummy(dropSz);

        if (ImGui::BeginDragDropTarget())
        {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("IANIM_PATH"))
            {
                const char* path = static_cast<const char*>(payload->Data);
                if (m_animClipSys && m_selectedEntity != NullEntity)
                {
                    Resource::AnimHandle ah = m_animClipSys->AcquireClip(path);
                    m_pendingAnimDrops.push_back({ ah, m_selectedEntity, std::string(path) });
                    LOG_INFO("EditorLayer (inspector): queued animation '%s' for entity %u",
                             path, m_selectedEntity);
                }
            }
            ImGui::EndDragDropTarget();
        }
    }

    // ---- Add Component button -----------------------------------------------
    ImGui::SetNextItemWidth(-1.f);
    if (ImGui::Button("Add Component", ImVec2(-1.f, 0.f)))
        ImGui::OpenPopup("##add_comp_popup");

    if (ImGui::BeginPopup("##add_comp_popup"))
    {
        ImGui::TextDisabled("Select component to add:");
        ImGui::Separator();

        // Filter out unaddable / already-present / condition-failing entries.
        std::vector<ComponentEditorEntry*> addable;
        for (auto& [typeIdx, entry] : m_componentEditors)
        {
            if (!entry.add) continue;
            if (entry.fetch(*m_world, m_selectedEntity)) continue; // already has it
            if (entry.condition && !entry.condition(*m_world, m_selectedEntity)) continue;
            addable.push_back(&entry);
        }

        if (addable.empty())
        {
            ImGui::TextDisabled("(all registered components already added)");
        }
        else
        {
            // Group by category, each sorted by priority/label.
            std::map<std::string, std::vector<ComponentEditorEntry*>> byCategory;
            for (ComponentEditorEntry* entry : addable)
                byCategory[entry->category].push_back(entry);
            for (auto& [_cat, entries] : byCategory)
            {
                std::sort(entries.begin(), entries.end(),
                    [](const ComponentEditorEntry* a, const ComponentEditorEntry* b) {
                        if (a->priority != b->priority) return a->priority < b->priority;
                        return a->label < b->label;
                    });
            }

            // Preferred category order; unlisted go alphabetical, "Misc" pinned last.
            static const char* const kCategoryOrder[] = {
                "Transform",
                "Rendering",
                "Lighting",
                "Environment",
                "Camera",
                "Animation",
                "Physics",
                "Gameplay",
                "VFX",
                "Post-Process",
                "UI",
                "AI / Script",
                "Attachment",
            };

            auto drawCategory = [&](const std::string& cat,
                                    std::vector<ComponentEditorEntry*>& entries)
            {
                if (entries.empty()) return;
                if (ImGui::BeginMenu(cat.c_str()))
                {
                    for (ComponentEditorEntry* entry : entries)
                    {
                        if (ImGui::MenuItem(entry->label.c_str()))
                        {
                            entry->add(*m_world, m_selectedEntity);
                            ImGui::CloseCurrentPopup();
                        }
                    }
                    ImGui::EndMenu();
                }
            };

            // Pass 1: preferred order. Erase as we go so pass 2 only sees the
            // leftovers (alphabetical), with Misc always last.
            for (const char* cat : kCategoryOrder)
            {
                auto it = byCategory.find(cat);
                if (it == byCategory.end()) continue;
                drawCategory(it->first, it->second);
                byCategory.erase(it);
            }
            std::vector<ComponentEditorEntry*> miscBucket;
            for (auto& [cat, entries] : byCategory)
            {
                if (cat == "Misc") { miscBucket = std::move(entries); continue; }
                drawCategory(cat, entries);
            }
            if (!miscBucket.empty())
                drawCategory("Misc", miscBucket);
        }
        ImGui::EndPopup();
    }

    ImGui::End();
}

void EditorLayer::RenderMaterialInspector(MaterialComponent& mat)
{
    // Texture lifetime managed by Renderer::SyncMaterialTextures (no editor-side acquire/release).

    // ---- Material source reference + dirty status ----
    static const char* kImatFilter = "Material (*.imat)\0*.imat\0All Files\0*.*\0\0";

    // Show .imat reference path
    const Entity selEnt = m_selectedEntity;
    MaterialSourcePath* matSrcPath = (selEnt != NullEntity && m_world)
        ? m_world->GetComponent<MaterialSourcePath>(selEnt) : nullptr;

    if (matSrcPath && !matSrcPath->path.empty())
    {
        ImGui::TextDisabled("Source: %s", matSrcPath->path.c_str());
        if (mat.IsAssetDirty())
        {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.f, 0.6f, 0.2f, 1.f), "(unsaved)");
        }
    }
    else
        ImGui::TextDisabled("Source: (none — inline material)");

    // ---- Save to .imat (writes back to source file or new file) ----
    if (ImGui::Button("Save to .imat"))
    {
        std::string savePath;
        if (matSrcPath && !matSrcPath->path.empty())
            savePath = matSrcPath->path; // save back to source
        else
            savePath = SaveFileDialog(kImatFilter, "imat", "asset/");

        if (!savePath.empty())
        {
            Resource::SaveMaterial(mat, savePath);
            mat.SetAssetDirty(false);
            // Update source path if saving to a new file
            if (selEnt != NullEntity && m_world)
            {
                if (matSrcPath) matSrcPath->path = savePath;
                else m_world->AddComponent<MaterialSourcePath>(selEnt, MaterialSourcePath{ savePath });
                m_world->RemoveComponent<MaterialOverride>(selEnt); // override no longer needed
            }
        }
    }

    // ---- Save as New (always prompts for a fresh path) ----------------------
    // Save as new .imat and re-point MaterialSourcePath. Used for "save as variant" without touching the original.
    ImGui::SameLine();
    if (ImGui::Button("Save as New"))
    {
        const std::string savePath = SaveFileDialog(kImatFilter, "imat", "asset/");
        if (!savePath.empty())
        {
            Resource::SaveMaterial(mat, savePath);
            mat.SetAssetDirty(false);
            if (selEnt != NullEntity && m_world)
            {
                if (matSrcPath) matSrcPath->path = savePath;
                else m_world->AddComponent<MaterialSourcePath>(selEnt, MaterialSourcePath{ savePath });
                m_world->RemoveComponent<MaterialOverride>(selEnt);
            }
        }
    }

    // ---- Save as Override (per-entity diff, doesn't touch .imat file) ----
    ImGui::SameLine();
    if (ImGui::Button("Save as Override"))
    {
        if (selEnt != NullEntity && m_world && matSrcPath && !matSrcPath->path.empty())
        {
            // Load the base material from .imat to compute diff
            MaterialComponent baseMat{};
            Resource::LoadMaterial(matSrcPath->path, baseMat);

            MaterialOverride ovr;

            // Compare and record differences
            auto fne = [](float a, float b) { return std::fabsf(a - b) > 1e-5f; };
            auto v4ne = [](const DirectX::XMFLOAT4& a, const DirectX::XMFLOAT4& b) {
                return std::fabsf(a.x-b.x) > 1e-5f || std::fabsf(a.y-b.y) > 1e-5f ||
                       std::fabsf(a.z-b.z) > 1e-5f || std::fabsf(a.w-b.w) > 1e-5f;
            };

            if (v4ne(mat.baseColor, baseMat.baseColor))
                ovr.Set("baseColor", MaterialOverride::Value::MakeVec4(
                    mat.baseColor.x, mat.baseColor.y, mat.baseColor.z, mat.baseColor.w));
            if (v4ne(mat.emissiveColor, baseMat.emissiveColor))
                ovr.Set("emissiveColor", MaterialOverride::Value::MakeVec4(
                    mat.emissiveColor.x, mat.emissiveColor.y, mat.emissiveColor.z, mat.emissiveColor.w));
            if (fne(mat.roughnessMax, baseMat.roughnessMax))
                ovr.Set("roughnessMax", MaterialOverride::Value::MakeFloat(mat.roughnessMax));
            if (fne(mat.roughnessMin, baseMat.roughnessMin))
                ovr.Set("roughnessMin", MaterialOverride::Value::MakeFloat(mat.roughnessMin));
            if (fne(mat.metalnessMax, baseMat.metalnessMax))
                ovr.Set("metalnessMax", MaterialOverride::Value::MakeFloat(mat.metalnessMax));
            if (fne(mat.metalnessMin, baseMat.metalnessMin))
                ovr.Set("metalnessMin", MaterialOverride::Value::MakeFloat(mat.metalnessMin));
            if (fne(mat.reflectance, baseMat.reflectance))
                ovr.Set("reflectance", MaterialOverride::Value::MakeFloat(mat.reflectance));
            if (fne(mat.normalMapStrength, baseMat.normalMapStrength))
                ovr.Set("normalMapStrength", MaterialOverride::Value::MakeFloat(mat.normalMapStrength));
            if (fne(mat.alphaRef, baseMat.alphaRef))
                ovr.Set("alphaRef", MaterialOverride::Value::MakeFloat(mat.alphaRef));
            if (fne(mat.outlinePixels, baseMat.outlinePixels))
                ovr.Set("outlinePixels", MaterialOverride::Value::MakeFloat(mat.outlinePixels));

            // Texture diffs
            for (int ti = 0; ti < MaterialComponent::TEXTURESLOT_COUNT; ++ti)
            {
                if (mat.textures[ti].name != baseMat.textures[ti].name)
                {
                    static const char* kSlotNames[] = {
                        "tex_BASECOLORMAP", "tex_NORMALMAP", "tex_SURFACEMAP",
                        "tex_METALLICMAP", "tex_ROUGHNESSMAP", "tex_OCCLUSIONMAP", "tex_EMISSIVEMAP"
                    };
                    if (ti < 7)
                        ovr.Set(kSlotNames[ti], MaterialOverride::Value::MakeTex(mat.textures[ti].name));
                }
            }

            if (!ovr.IsEmpty())
            {
                m_world->AddComponent<MaterialOverride>(selEnt, std::move(ovr));
                mat.SetAssetDirty(false);
                LOG_INFO("EditorLayer: saved %zu material override(s) on entity %u",
                         m_world->GetComponent<MaterialOverride>(selEnt)->props.size(), selEnt);
            }
            else
                LOG_INFO("EditorLayer: no differences from base — no override needed");
        }
        else
            LOG_WARNING("EditorLayer: no MaterialSourcePath — save as .imat first");
    }

    // ---- Load .imat (replaces runtime data + updates ref) ----
    ImGui::SameLine();
    if (ImGui::Button("Load .imat"))
    {
        const std::string path = OpenFileDialog(kImatFilter, "asset/");
        if (!path.empty())
        {
            Resource::LoadMaterial(path, mat);
            mat.SetAssetDirty(false);
            if (selEnt != NullEntity && m_world)
            {
                if (matSrcPath) matSrcPath->path = path;
                else m_world->AddComponent<MaterialSourcePath>(selEnt, MaterialSourcePath{ path });
                m_world->RemoveComponent<MaterialOverride>(selEnt);
            }
        }
    }

    // Show override status
    if (selEnt != NullEntity && m_world)
    {
        const MaterialOverride* ovr = m_world->GetComponent<MaterialOverride>(selEnt);
        if (ovr && !ovr->IsEmpty())
            ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.f, 1.f), "Override: %zu prop(s)", ovr->props.size());
    }

    ImGui::Separator();

    // ---- Shader type --------------------------------------------------------
    static const char* kShaderNames[] = {
#define SHADER_LABEL_(e, label) label,
        MATERIAL_SHADER_TYPES(SHADER_LABEL_)
#undef SHADER_LABEL_
    };
    int shaderIdx = static_cast<int>(mat.shaderType);
    if (ImGui::Combo("Shader Type", &shaderIdx, kShaderNames, IM_ARRAYSIZE(kShaderNames)))
    {
        mat.shaderType = static_cast<MaterialComponent::SHADERTYPE>(shaderIdx);
        mat.SetDirty();
    }

    // ---- Custom GBuffer PS (Phase B): swaps GBuffer.ps.hlsl; ShadingModel decides lighting interpretation. ----
    if (ImGui::Checkbox("Use Custom Shader", &mat.useCustomShader))
    {
        mat.customShaderID = -1;   // force re-resolution on next GBuffer tick
        mat.SetDirty();
    }
    if (mat.useCustomShader)
    {
        char pathBuf[256] = {};
        const size_t copy = (std::min)(mat.customShaderPath.size(), sizeof(pathBuf) - 1);
        std::memcpy(pathBuf, mat.customShaderPath.data(), copy);
        pathBuf[copy] = '\0';
        if (ImGui::InputText("Shader Path", pathBuf, sizeof(pathBuf)))
        {
            mat.customShaderPath = pathBuf;
            mat.customShaderID   = -1;   // path changed, reload
            mat.SetDirty();
        }
        // Drag-drop accepts IHLSL_PATH from Resource panel `.hlsl` rows.
        if (ImGui::BeginDragDropTarget())
        {
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("IHLSL_PATH"))
            {
                mat.customShaderPath = static_cast<const char*>(p->Data);
                mat.customShaderID   = -1;
                // Drop old schema; new shader's reflection repopulates next tick.
                mat.customTextures.clear();
                mat.customParams.clear();
                mat.SetDirty();
            }
            ImGui::EndDragDropTarget();
        }

        static const char* kShadingModelNames[static_cast<int>(ShadingModel::Count)] = {
            ShadingModelName(ShadingModel::Standard),
            ShadingModelName(ShadingModel::Unlit),
            ShadingModelName(ShadingModel::ClearCoat),
            ShadingModelName(ShadingModel::Subsurface),
            ShadingModelName(ShadingModel::Anisotropic),
        };
        int smIdx = static_cast<int>(mat.customShadingModel);
        if (ImGui::Combo("Shading Model", &smIdx, kShadingModelNames, IM_ARRAYSIZE(kShadingModelNames)))
        {
            mat.customShadingModel = static_cast<ShadingModel>(smIdx);
            mat.SetDirty();
        }

        if (mat.customShaderID < 0)
            ImGui::TextColored(ImVec4(1.f, 0.6f, 0.2f, 1.f),
                "  (shader not yet resolved — will compile on next frame)");

        // Surface DXC compile error (cached by ShaderLibrary; cleared on .hlsl edit via hot-reload).
        if (m_renderer && mat.customShaderID > 0)
        {
            const std::string err = m_renderer->GetCustomShaderError(mat.customShaderID);
            if (!err.empty())
            {
                ImGui::Spacing();
                ImGui::TextColored(ImVec4(1.f, 0.35f, 0.35f, 1.f),
                                   "  Compile Error:");
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 0.55f, 0.55f, 1.f));
                ImGui::TextWrapped("%s", err.c_str());
                ImGui::PopStyleColor();
            }
        }

        // ---- Reflection-driven custom textures + params -------------------
        const ShaderReflect::Reflection* refl = m_renderer
            ? m_renderer->GetCustomShaderReflection(mat.customShaderID)
            : nullptr;

        if (!mat.customTextures.empty())
        {
            ImGui::Separator();
            ImGui::Text("Custom Textures");
            for (auto& [name, tex] : mat.customTextures)
            {
                if (DrawTextureSlotWidget(name.c_str(), tex))
                    mat.SetDirty();
            }
        }

        if (!mat.customParams.empty())
        {
            ImGui::Separator();
            ImGui::Text("Custom Parameters");

            // name → reflected VarType for typed widgets; absent falls back to DragFloat4.
            std::unordered_map<std::string, ShaderReflect::VarType> typeByName;
            if (refl)
            {
                for (const auto& cb : refl->cbuffers)
                {
                    if (MaterialReflectionSync::IsReservedCBuffer(cb.name.c_str())) continue;
                    for (const auto& v : cb.vars)
                        typeByName.emplace(v.name, v.type);
                }
            }

            for (auto& [name, val] : mat.customParams)
            {
                ImGui::PushID(name.c_str());

                // "color" substring (case-insensitive) → ColorEdit instead of DragFloat for HDR tints.
                bool colorHint = false;
                for (std::size_t i = 0; i + 5 <= name.size() && !colorHint; ++i)
                {
                    const char* s = name.c_str() + i;
                    if ((s[0]=='c'||s[0]=='C') && (s[1]=='o'||s[1]=='O') &&
                        (s[2]=='l'||s[2]=='L') && (s[3]=='o'||s[3]=='O') &&
                        (s[4]=='r'||s[4]=='R')) colorHint = true;
                }

                auto it = typeByName.find(name);
                const ShaderReflect::VarType type = (it != typeByName.end())
                    ? it->second
                    : ShaderReflect::VarType::Float4;     // fallback

                using V = ShaderReflect::VarType;
                bool changed = false;
                switch (type)
                {
                    case V::Bool:
                    {
                        bool b = val[0] > 0.5f;
                        if (ImGui::Checkbox(name.c_str(), &b))
                        { val[0] = b ? 1.0f : 0.0f; changed = true; }
                        break;
                    }
                    case V::Float:
                        changed = ImGui::DragFloat(name.c_str(), &val[0], 0.01f);
                        break;
                    case V::Float2:
                        changed = ImGui::DragFloat2(name.c_str(), val.data(), 0.01f);
                        break;
                    case V::Float3:
                        changed = colorHint
                            ? ImGui::ColorEdit3(name.c_str(), val.data(),
                                ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR)
                            : ImGui::DragFloat3(name.c_str(), val.data(), 0.01f);
                        break;
                    case V::Float4:
                        changed = colorHint
                            ? ImGui::ColorEdit4(name.c_str(), val.data(),
                                ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR)
                            : ImGui::DragFloat4(name.c_str(), val.data(), 0.01f);
                        break;
                    case V::Int:  case V::UInt:
                    {
                        int i = static_cast<int>(val[0]);
                        if (ImGui::DragInt(name.c_str(), &i))
                        { val[0] = static_cast<float>(i); changed = true; }
                        break;
                    }
                    case V::Int2: case V::UInt2:
                    {
                        int iv[2] = { (int)val[0], (int)val[1] };
                        if (ImGui::DragInt2(name.c_str(), iv))
                        { val[0] = (float)iv[0]; val[1] = (float)iv[1]; changed = true; }
                        break;
                    }
                    case V::Int3: case V::UInt3:
                    {
                        int iv[3] = { (int)val[0], (int)val[1], (int)val[2] };
                        if (ImGui::DragInt3(name.c_str(), iv))
                        { val[0] = (float)iv[0]; val[1] = (float)iv[1]; val[2] = (float)iv[2]; changed = true; }
                        break;
                    }
                    case V::Int4: case V::UInt4:
                    {
                        int iv[4] = { (int)val[0], (int)val[1], (int)val[2], (int)val[3] };
                        if (ImGui::DragInt4(name.c_str(), iv))
                        { for (int k = 0; k < 4; ++k) val[k] = (float)iv[k]; changed = true; }
                        break;
                    }
                    default:
                        changed = ImGui::DragFloat4(name.c_str(), val.data(), 0.01f);
                        break;
                }
                if (changed) mat.SetDirty();
                ImGui::PopID();
            }
        }
    }

    // ---- Blend mode ---------------------------------------------------------
    static const char* kBlendNames[] = { "Opaque", "Alpha", "Additive", "Premultiplied","Multiply"};
    int blendIdx = static_cast<int>(mat.userBlendMode);
    if (ImGui::Combo("Blend Mode", &blendIdx, kBlendNames, IM_ARRAYSIZE(kBlendNames)))
    {
        mat.userBlendMode = static_cast<BlendMode>(blendIdx);
        mat.SetEditDirty();
    }

    // ---- Shadow cull mode (Default=back-cull, Front=closed-mesh, None=single-sided; alpha-test always None). ----
    static const char* kShadowCullNames[] = { "Default (Back)", "Front (Closed-Mesh)", "None (Single-Sided)" };
    int shadowCullIdx = static_cast<int>(mat.shadowCullMode);
    if (ImGui::Combo("Shadow Cull", &shadowCullIdx, kShadowCullNames, IM_ARRAYSIZE(kShadowCullNames)))
    {
        mat.shadowCullMode = static_cast<ShadowCullMode>(shadowCullIdx);
        mat.SetEditDirty();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "Default: back-face cull, relies on depth bias to fight self-shadow acne.\n"
            "Front:   front-face cull, eliminates self-shadow acne on closed meshes\n"
            "         (character bodies, organic solids). Shadows on other surfaces\n"
            "         gain a tiny gap (peter-pan). Wrong for single-sided geometry.\n"
            "None:    no cull — use for hair cards, eyelash planes, cloth, leaves.");

    // ---- Flags --------------------------------------------------------------
    {
        bool castShadow    = mat.IsCastingShadow();
        bool receiveShadow = mat.IsReceiveShadow();
        bool doubleSided   = mat.IsDoubleSided();
        bool outline       = mat.IsOutlineEnabled();
        if (ImGui::Checkbox("Cast Shadow",    &castShadow))    mat.SetCastShadow(castShadow);
        ImGui::SameLine();
        if (ImGui::Checkbox("Receive Shadow", &receiveShadow)) mat.SetReceiveShadow(receiveShadow);
        if (ImGui::Checkbox("Double Sided",   &doubleSided))
        { mat.SetEditDirty(); if (doubleSided) mat._flags |= MaterialComponent::DOUBLE_SIDED; else mat._flags &= ~MaterialComponent::DOUBLE_SIDED; }
        ImGui::SameLine();
        if (ImGui::Checkbox("Outline",        &outline))
        { mat.SetEditDirty(); if (outline) mat._flags |= MaterialComponent::OUTLINE; else mat._flags &= ~MaterialComponent::OUTLINE; }
        if (outline)
        {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(120.0f);
            if (ImGui::DragFloat("##OutlinePixels", &mat.outlinePixels, 0.1f, 0.5f, 20.0f, "%.1f px"))
                mat.SetEditDirty();
            ImGui::SameLine();
            ImGui::TextDisabled("Thickness");

            bool ssOutline = mat.IsOutlineScreenSpaceEnabled();
            if (ImGui::Checkbox("Screen Space Outline", &ssOutline))
            { mat.SetEditDirty(); mat.SetOutlineScreenSpaceEnabled(ssOutline); }
        }

        // Skip XeGTAO for characters/skin (avoids temporal ghosts on moving meshes); flag short-circuits to 1.0.
        bool excludeSSAO = (mat._flags & MaterialComponent::EXCLUDE_FROM_SSAO) != 0;
        if (ImGui::Checkbox("Exclude from SSAO", &excludeSSAO))
        {
            mat.SetEditDirty();
            if (excludeSSAO) mat._flags |=  MaterialComponent::EXCLUDE_FROM_SSAO;
            else             mat._flags &= ~MaterialComponent::EXCLUDE_FROM_SSAO;
        }
    }

    // ---- Engine PBR/NPR/Unlit params ---- Walks kMaterialPBRSchema (visibleMask gated). Add fields by extending the schema.
    if (!mat.useCustomShader)
    {
        ImGui::Separator();
        ImGui::Text("Material Parameters");

        const uint32_t shaderMask = 1u << static_cast<uint32_t>(mat.shaderType);
        const char* lastGroup = nullptr;

        for (const auto& f : MaterialSchema::kMaterialPBRSchema)
        {
            if (!(f.visibleMask & shaderMask)) continue;

            // Emit group label header on first sighting (mirrors hand-written subheadings).
            if (f.group && (!lastGroup || std::strcmp(f.group, lastGroup) != 0))
            {
                if (lastGroup) ImGui::Spacing();
                ImGui::TextDisabled("%s", f.group);
                lastGroup = f.group;
            }

            float* base    = MaterialSchema::FieldPtr(mat, f);
            bool   changed = false;

            switch (f.kind)
            {
            case MaterialSchema::Kind::Float:
                changed = ImGui::SliderFloat(f.label, base, f.min, f.max);
                break;
            case MaterialSchema::Kind::Color3:
                changed = ImGui::ColorEdit4(f.label, base, ImGuiColorEditFlags_NoAlpha);
                break;
            case MaterialSchema::Kind::Color4:
                changed = ImGui::ColorEdit4(f.label, base);
                break;
            case MaterialSchema::Kind::FloatComponent:
                changed = ImGui::SliderFloat(f.label, base + f.component, f.min, f.max);
                break;
            }

            if (changed) mat.SetEditDirty();
        }
    } // if (!mat.useCustomShader)

    // ---- Texture slots ---- engine PBR/NPR/Unlit paths only; custom shaders use reflection-driven Custom Textures.
    if (!mat.useCustomShader)
    {
    ImGui::Separator();
    ImGui::Text("Texture Slots");

    static const char* kSlotNames[] = {
#define SLOT_LABEL_(e, label, path) label,
        MATERIAL_TEXTURE_SLOTS(SLOT_LABEL_)
#undef SLOT_LABEL_
    };

    constexpr float kPreviewSize = 64.f;

    for (int s = 0; s < MaterialComponent::TEXTURESLOT_COUNT; ++s)
    {
        // Filter texture slots by shader type
        const bool isNprRampTex = (mat.shaderType == MaterialComponent::SHADERTYPE_NPR_RAMP);
        const bool isNprColTex  = (mat.shaderType == MaterialComponent::SHADERTYPE_NPR_COLOR);
        const bool isUnlitTex   = (mat.shaderType == MaterialComponent::SHADERTYPE_UNLIT);
        if (isNprRampTex)
        {
            // NPR Ramp: show Base Color, Normal Map, Surface Map, Ramp Texture
            if (s != MaterialComponent::BASECOLORMAP &&
                s != MaterialComponent::NORMALMAP &&
                s != MaterialComponent::SURFACEMAP &&
                s != MaterialComponent::RAMPMAP)
                continue;
        }
        else if (isNprColTex)
        {
            // NPR Color: same as NPR Ramp minus the Ramp Texture (colors replace it).
            if (s != MaterialComponent::BASECOLORMAP &&
                s != MaterialComponent::NORMALMAP &&
                s != MaterialComponent::SURFACEMAP)
                continue;
        }
        else if (isUnlitTex)
        {
            // Unlit: only Base Color + Emissive textures are meaningful.
            if (s != MaterialComponent::BASECOLORMAP &&
                s != MaterialComponent::EMISSIVEMAP)
                continue;
        }
        else
        {
            // PBR: skip Ramp Texture slot
            if (s == MaterialComponent::RAMPMAP)
                continue;
        }

        if (DrawTextureSlotWidget(kSlotNames[s], mat.textures[s]))
            mat.SetEditDirty();

        if (s < MaterialComponent::TEXTURESLOT_COUNT - 1)
            ImGui::Spacing();
    }
    } // if (!mat.useCustomShader)

    // ---- UV tiling / offset -------------------------------------------------
    ImGui::Separator();
    ImGui::Text("UV Transform");
    if (ImGui::DragFloat2("Tiling",  &mat.texMulAdd.x, 0.01f)) mat.SetEditDirty();
    if (ImGui::DragFloat2("Offset",  &mat.texMulAdd.z, 0.01f)) mat.SetEditDirty();
}

// ---- Asset browser — filesystem scan + import ----
void EditorLayer::RunImport(AssetType type)
{
    // Build per-type filter strings (OPENFILENAMEA uses NUL separators)
    const char* filter = nullptr;
    const char* outExt = nullptr;

    static const char kFilterTexture[]   = "Image Files\0*.png;*.jpg;*.dds;*.jpeg;*.tga;*.bmp;*.hdr\0All Files\0*.*\0\0";
    static const char kFilterModel[]     = "3D Model Files\0*.obj;*.gltf;*.pmx;*.glb;*.vrm;*.fbx;*.dae;*.3ds;*.ply;*.stl\0All Files\0*.*\0\0";
    static const char kFilterShader[]    = "HLSL Shader\0*.hlsl\0All Files\0*.*\0\0";
    static const char kFilterMaterial[]  = "Material\0*.mat\0All Files\0*.*\0\0";
    static const char kFilterAnimation[] = "Animation Files\0*.fbx;*.gltf;*.glb;*.dae;*.bvh;*.vmd\0All Files\0*.*\0\0";
    static const char kFilterAudio[]     = "Audio Files\0*.wav\0All Files\0*.*\0\0";

    switch (type)
    {
    case AssetType::Texture:   filter = kFilterTexture;   outExt = ".itex";  break;
    case AssetType::Mesh:      filter = kFilterModel;     outExt = nullptr;  break;
    case AssetType::Shader:    filter = kFilterShader;    outExt = ".ishdr"; break;
    case AssetType::Material:  filter = kFilterMaterial;  outExt = ".imat";  break;
    case AssetType::Animation: filter = kFilterAnimation; outExt = nullptr;  break;
    case AssetType::Audio:     filter = kFilterAudio;     outExt = ".aclip"; break;
    default: return;
    }

    const std::vector<std::string> srcPaths =
        OpenFileDialogMulti(filter, m_assetDir.empty() ? nullptr : m_assetDir.c_str());
    if (srcPaths.empty())
        return;  // user cancelled

    namespace fs = std::filesystem;
    const fs::path outDir = m_assetDir.empty()
        ? fs::path(srcPaths[0]).parent_path()
        : fs::path(m_assetDir);

    for (const std::string& srcPath : srcPaths)
    {
        const std::vector<uint8_t> srcData = ReadFileFully(srcPath);
        if (srcData.empty())
        {
            LOG_ERROR("EditorLayer: failed to read '%s'", srcPath.c_str());
            continue;
        }

        // ---- Animation: single .ianim output via AnimationImporter ----
        if (type == AssetType::Animation)
        {
            if (srcPath.ends_with(".vmd"))
            {
                Resource::VmdImporter imp;
				const std::vector<uint8_t> blob = imp.Import(srcPath, srcData);
                if (blob.empty())
                {
                    LOG_ERROR("EditorLayer: animation import failed for '%s'", srcPath.c_str());
                    continue;
                }
                const std::string outPath =
                    (outDir / (fs::path(srcPath).stem().string() + ".ianim")).string();
                if (!WriteFileFully(outPath, blob))
                    LOG_ERROR("EditorLayer: failed to write '%s'", outPath.c_str());
                else
                    LOG_INFO("EditorLayer: imported '%s' → '%s'", srcPath.c_str(), outPath.c_str());
                m_needsRescan = true;
                continue;
            }

            Resource::AnimationImporter imp;
            const std::vector<uint8_t> blob = imp.Import(srcPath, srcData);
            if (blob.empty())
            {
                LOG_ERROR("EditorLayer: animation import failed for '%s'", srcPath.c_str());
                continue;
            }
            const std::string outPath =
                (outDir / (fs::path(srcPath).stem().string() + ".ianim")).string();
            if (!WriteFileFully(outPath, blob))
                LOG_ERROR("EditorLayer: failed to write '%s'", outPath.c_str());
            else
                LOG_INFO("EditorLayer: imported '%s' → '%s'", srcPath.c_str(), outPath.c_str());
            m_needsRescan = true;
            continue;
        }

        // ---- Model: multi-file output ----
        if (type == AssetType::Mesh)
        {
            const std::string ext = fs::path(srcPath).extension().string();
            const bool isPmx = (ext == ".pmx" || ext == ".PMX");
            const bool isVrm = (ext == ".vrm" || ext == ".VRM");

            if (isVrm)
            {
                // ---- VRM: parse extension JSON, then delegate geometry to SceneImporter ----
                Resource::VrmImportResult vrmRes =
                    Resource::VrmImporter::Import(srcData.data(), srcData.size());

                std::vector<Resource::VrmExportedFile> vrmFiles =
                    Resource::VrmImporter::BuildBlobs(srcPath, srcData, vrmRes);

                if (vrmFiles.empty())
                {
                    LOG_ERROR("EditorLayer: VRM import failed for '%s'", srcPath.c_str());
                    continue;
                }

                int written = 0;
                for (const auto& f : vrmFiles)
                {
                    const std::string outPath = (outDir / f.relativePath).string();
                    fs::create_directories(fs::path(outPath).parent_path());
                    if (WriteFileFully(outPath, f.blob))
                    {
                        LOG_INFO("EditorLayer:   wrote '%s'", outPath.c_str());
                        ++written;
                    }
                    else
                        LOG_ERROR("EditorLayer:   failed to write '%s'", outPath.c_str());
                }

                LOG_INFO("EditorLayer: VRM imported '%s' → %d file(s)", srcPath.c_str(), written);
                m_needsRescan = true;
                continue;
            }

            if (isPmx)
            {
                // ---- PMX: custom importer (no Assimp) ----
                Resource::PmxImportResult pmxRes =
                    Resource::PmxImporter::Import(srcData.data(), srcData.size());

                if (!pmxRes.success)
                {
                    LOG_ERROR("EditorLayer: PMX import failed for '%s'", srcPath.c_str());
                    continue;
                }

                const std::string pmxStem = fs::path(srcPath).stem().string();
                std::vector<Resource::PmxExportedFile> pmxFiles =
                    Resource::PmxImporter::BuildBlobs(pmxRes, pmxStem);

                int written = 0;
                for (const auto& f : pmxFiles)
                {
                    const std::string outPath = (outDir / f.relativePath).string();
                    fs::create_directories(fs::path(outPath).parent_path());
                    if (WriteFileFully(outPath, f.blob))
                    {
                        LOG_INFO("EditorLayer:   wrote '%s'", outPath.c_str());
                        ++written;
                    }
                    else
                        LOG_ERROR("EditorLayer:   failed to write '%s'", outPath.c_str());
                }

                LOG_INFO("EditorLayer: PMX imported '%s' → %d file(s)", srcPath.c_str(), written);
                m_needsRescan = true;
                continue;
            }

            // ---- Non-PMX models: Assimp-based SceneImporter ----
            Resource::SceneImporter::ImportResult res =
                Resource::SceneImporter::Import(srcPath, srcData);

            if (!res.success || res.files.empty())
            {
                LOG_ERROR("EditorLayer: scene import failed for '%s'", srcPath.c_str());
                continue;
            }

            int written = 0;
            for (const auto& f : res.files)
            {
                const std::string outPath = (outDir / f.relativePath).string();
                if (WriteFileFully(outPath, f.blob))
                {
                    LOG_INFO("EditorLayer:   wrote '%s'", outPath.c_str());
                    ++written;
                }
                else
                {
                    LOG_ERROR("EditorLayer:   failed to write '%s'", outPath.c_str());
                }
            }

            LOG_INFO("EditorLayer: imported '%s' → %d file(s)", srcPath.c_str(), written);
            m_needsRescan = true;
            continue;
        }

        // ---- Single-file importers (Texture / Shader / Material) ----
        std::vector<uint8_t> blob;
        switch (type)
        {
        case AssetType::Texture:
        {
            Resource::TextureImporter imp;
            blob = imp.Import(srcPath, srcData);
            break;
        }
        case AssetType::Shader:
        {
            Resource::ShaderImporter imp;
            blob = imp.Import(srcPath, srcData);
            break;
        }
        case AssetType::Material:
        {
            Resource::MaterialImporter imp;
            blob = imp.Import(srcPath, srcData);
            break;
        }
        case AssetType::Audio:
        {
            Audio::AudioImporter imp;
            blob = imp.Import(srcPath, srcData);
            break;
        }
        default: break;
        }

        if (blob.empty())
        {
            LOG_ERROR("EditorLayer: import failed for '%s'", srcPath.c_str());
            continue;
        }

        assert(outExt != nullptr);
        const std::string outPath =
            (outDir / (fs::path(srcPath).stem().string() + outExt)).string();

        if (!WriteFileFully(outPath, blob))
        {
            LOG_ERROR("EditorLayer: failed to write '%s'", outPath.c_str());
            continue;
        }

        LOG_INFO("EditorLayer: imported '%s' → '%s'", srcPath.c_str(), outPath.c_str());
        m_needsRescan = true;
    }
}

void EditorLayer::ScanAssets()
{
    // ---- Step 1: snapshot existing preview state keyed by path ----
    // Preview state (GPU handle + visibility timer) carries over across rescans so thumbnails don't flicker.
    struct PreviewState
    {
        Resource::TextureHandle thumbnail;
        float                   lastVisibleTime;
    };
    std::unordered_map<std::string, PreviewState> saved;
    saved.reserve(m_assets.size());
    for (auto& ae : m_assets)
        saved[ae.path] = { ae.thumbnail, ae.lastVisibleTime };

    m_assets.clear();

    // Helper: release all orphaned handles remaining in `saved`.
    auto releaseOrphans = [&]()
    {
        if (m_textureSys && m_gfx)
            for (auto& [path, ps] : saved)
                if (ps.thumbnail != Resource::kInvalidTextureHandle)
                    m_textureSys->Release(ps.thumbnail, *m_gfx);
        saved.clear();
    };

    if (m_assetDir.empty()) { releaseOrphans(); return; }

    // --- Step 2: Scan directory ---------------------------------------------
    const std::string& scanDir = m_currentDir.empty() ? m_assetDir : m_currentDir;
    std::error_code ec;
    if (!std::filesystem::exists(scanDir, ec)) { releaseOrphans(); return; }

    auto typeOf = [](const std::string& ext) -> AssetType
    {
        if (ext == ".itex")  return AssetType::Texture;
        if (ext == ".ishdr") return AssetType::Shader;
        if (ext == ".hlsl")  return AssetType::Shader;   // source; draggable to custom-shader field
        if (ext == ".imsh")  return AssetType::Mesh;
        if (ext == ".imat")  return AssetType::Material;
        if (ext == ".iscn")  return AssetType::Scene;
        if (ext == ".ipfb")  return AssetType::Prefab;
        if (ext == ".iskel") return AssetType::Skeleton;
        if (ext == ".ianim") return AssetType::Animation;
        if (ext == ".aclip") return AssetType::Audio;
        if (ext == ".lua")   return AssetType::Script;
        return AssetType::Unknown;
    };

    for (const auto& entry : std::filesystem::directory_iterator(scanDir, ec))
    {
        if (entry.is_directory())
        {
            AssetEntry ae;
            ae.path = entry.path().string();
            ae.stem = entry.path().filename().string();
            ae.type = AssetType::Folder;
            m_assets.push_back(std::move(ae));
            continue;
        }

        if (!entry.is_regular_file()) continue;

        std::string ext = entry.path().extension().string();
        for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

        AssetType t = typeOf(ext);
        if (t == AssetType::Unknown) continue;

        AssetEntry ae;
        ae.path      = entry.path().string();
        ae.stem      = entry.path().stem().string();
        ae.type      = t;
        ae.thumbnail = Resource::kInvalidTextureHandle;
        m_assets.push_back(std::move(ae));
    }

    // Sort: folders first, then by type enum, then alphabetically.
    std::sort(m_assets.begin(), m_assets.end(), [](const AssetEntry& a, const AssetEntry& b)
    {
        const bool af = (a.type == AssetType::Folder);
        const bool bf = (b.type == AssetType::Folder);
        if (af != bf) return af > bf;
        if (a.type != b.type) return static_cast<int>(a.type) < static_cast<int>(b.type);
        return a.stem < b.stem;
    });

    // ---- Step 3: restore preview state; remaining `saved` entries are orphans to release. ----
    for (auto& ae : m_assets)
    {
        auto it = saved.find(ae.path);
        if (it != saved.end())
        {
            ae.thumbnail       = it->second.thumbnail;
            ae.lastVisibleTime = it->second.lastVisibleTime;
            saved.erase(it);   // consumed — not an orphan
        }
    }

    // Release handles for paths that disappeared from disk.
    releaseOrphans();
}

// ---- Asset browser — grid renderer ----
void EditorLayer::RenderAssetGrid()
{
    constexpr float kTileSize    = 80.f;   // thumbnail/icon square size
    constexpr float kTilePadX   = 10.f;   // horizontal gap between tiles
    constexpr float kTilePadY   = 22.f;   // space below tile for label
    constexpr float kTileStepX  = kTileSize + kTilePadX;
    constexpr float kTileStepY  = kTileSize + kTilePadY + 6.f;

    const float panelWidth = ImGui::GetContentRegionAvail().x;
    const int   columns    = (std::max)(1, static_cast<int>(panelWidth / kTileStepX));

    auto iconChar = [&](AssetType t) -> const char*
    {
        if (!m_hasFontAwesomeIcons) return nullptr;
        switch (t)
        {
        case AssetType::Texture:  return FontAwesome::kImage;
        case AssetType::Shader:   return FontAwesome::kCode;
        case AssetType::Mesh:     return FontAwesome::kCube;
        case AssetType::Material: return FontAwesome::kPaintBrush;
        case AssetType::Scene:    return FontAwesome::kCubes;
        case AssetType::Prefab:   return FontAwesome::kCubes;
        case AssetType::Skeleton:  return FontAwesome::kUser;
        case AssetType::Animation: return FontAwesome::kFilm;
        case AssetType::Audio:     return FontAwesome::kVolumeUp;
        case AssetType::Script:    return FontAwesome::kFileText;
        case AssetType::Folder:   return FontAwesome::kFolder;
        default:                  return nullptr;
        }
    };

    auto tileColor = [](AssetType t) -> ImU32
    {
        switch (t)
        {
        case AssetType::Texture:  return IM_COL32( 50,  90, 160, 255);  // blue
        case AssetType::Shader:   return IM_COL32(140,  60, 180, 255);  // purple
        case AssetType::Mesh:     return IM_COL32( 60, 140,  60, 255);  // green
        case AssetType::Material: return IM_COL32(180, 100,  30, 255);  // orange
        case AssetType::Scene:    return IM_COL32( 30, 130, 140, 255);  // teal
        case AssetType::Prefab:   return IM_COL32( 80,  40, 160, 255);  // purple
        case AssetType::Skeleton:  return IM_COL32(200,  80,  80, 255);  // red
        case AssetType::Animation: return IM_COL32( 80, 160, 100, 255);  // green
        case AssetType::Audio:     return IM_COL32(180, 110, 200, 255);  // pink-violet
        case AssetType::Script:    return IM_COL32(100, 150, 200, 255);  // light blue
        case AssetType::Folder:   return IM_COL32(180, 150,  30, 255);  // gold
        default:                  return IM_COL32( 70,  70,  70, 255);
        }
    };

    // Per-frame constants (captured once before any child window changes context).
    const float currentTime = static_cast<float>(ImGui::GetTime());
    constexpr float kVisBuffer = kTileStepY;  // 1-row lookahead beyond viewport edges

    // Reset per-frame load budget.
    m_previewLoadsThisFrame = 0;

    // ---- Eviction pass: release items off-screen longer than m_previewEvictDelay; skip never-visible. ----
    if (m_textureSys && m_gfx)
    {
        for (auto& ae : m_assets)
        {
            if (ae.thumbnail == Resource::kInvalidTextureHandle) continue;
            if (ae.lastVisibleTime < 0.f) continue;

            const float hiddenFor = currentTime - ae.lastVisibleTime;
            if (hiddenFor > m_previewEvictDelay)
            {
                m_textureSys->Release(ae.thumbnail, *m_gfx);
                ae.thumbnail       = Resource::kInvalidTextureHandle;
                ae.lastVisibleTime = -1.f;
            }
        }
    }

    // Scroll region for the grid
    ImGui::BeginChild("AssetGridScroll", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_None);

    if (m_assets.empty())
    {
        ImGui::TextDisabled("No converted assets found in '%s'", m_assetDir.c_str());
        ImGui::TextDisabled("(place .itex / .ishdr files here and hit Refresh)");
        ImGui::EndChild();
        return;
    }

    // Case-insensitive substring filter (lowercase needle once).
    char filterLower[sizeof(m_assetFilter)] = { 0 };
    const bool filterActive = (m_assetFilter[0] != '\0');
    if (filterActive)
    {
        for (size_t k = 0; m_assetFilter[k] && k + 1 < sizeof(filterLower); ++k)
            filterLower[k] = static_cast<char>(std::tolower(static_cast<unsigned char>(m_assetFilter[k])));
    }
    const size_t needleLen = std::strlen(filterLower);
    auto matchesFilter = [&](const std::string& stem) -> bool
    {
        if (!filterActive) return true;
        const size_t n = stem.size();
        if (needleLen == 0 || needleLen > n) return needleLen == 0;
        for (size_t i = 0; i + needleLen <= n; ++i)
        {
            size_t j = 0;
            for (; j < needleLen; ++j)
                if (std::tolower(static_cast<unsigned char>(stem[i + j])) != filterLower[j])
                    break;
            if (j == needleLen) return true;
        }
        return false;
    };

    int col = 0;
    int shownCount = 0;
    for (int i = 0; i < static_cast<int>(m_assets.size()); ++i)
    {
        AssetEntry& ae = m_assets[i];

        // Apply name filter — folders are always shown so navigation isn't blocked.
        if (ae.type != AssetType::Folder && !matchesFilter(ae.stem))
            continue;
        ++shownCount;

        // Screen position of this tile — read before any ImGui state mutation.
        ImVec2 tileMin = ImGui::GetCursorScreenPos();

        // ---- Visibility culling + load throttling ---------------------------
        if (ae.type == AssetType::Texture && m_textureSys && m_resourceMgr && m_gfx)
        {
            const float winTop   = ImGui::GetWindowPos().y;
            const float winBot   = winTop + ImGui::GetWindowHeight();
            const float tileBotY = tileMin.y + kTileSize + kTilePadY;
            const bool  visible  = (tileBotY + kVisBuffer > winTop) &&
                                   (tileMin.y  - kVisBuffer < winBot);

            if (visible)
            {
                // Stamp "last seen" — eviction timer only advances when this stops updating.
                ae.lastVisibleTime = currentTime;

                if (ae.thumbnail == Resource::kInvalidTextureHandle
                    && m_previewLoadsThisFrame < m_previewLoadBudgetPerFrame)
                {
                    ae.thumbnail = m_textureSys->Acquire(ae.path, *m_resourceMgr, *m_gfx);
                    ++m_previewLoadsThisFrame;
                }
            }
            // Off-screen: don't touch lastVisibleTime — gap grows naturally.
        }

        const bool selected = (m_selectedAsset == i);

        // Invisible button for interaction (full tile area including label space)
        ImGui::PushID(i);
        const bool tileClicked = ImGui::InvisibleButton("##tile", ImVec2(kTileSize, kTileSize + kTilePadY));

        // ---- Drag source — must come right after the button, before PopID ----
        if (ae.type == AssetType::Scene)
        {
            // Only .iscn is drag-droppable (single-.imsh spawn retired with P1-P6 rewrite).
            if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None))
            {
                ImGui::SetDragDropPayload("ISCN_PATH",
                    ae.path.c_str(), ae.path.size() + 1);
                ImGui::Text("Spawn: %s", ae.stem.c_str());
                ImGui::EndDragDropSource();
            }
        }
        else if (ae.type == AssetType::Prefab)
        {
            if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None))
            {
                ImGui::SetDragDropPayload("IPFB_PATH",
                    ae.path.c_str(), ae.path.size() + 1);
                ImGui::Text("Prefab: %s", ae.stem.c_str());
                ImGui::EndDragDropSource();
            }
        }
        else if (ae.type == AssetType::Texture)
        {
            if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None))
            {
                ImGui::SetDragDropPayload("ITEX_PATH",
                    ae.path.c_str(), ae.path.size() + 1);
                ImGui::Text("%s", ae.stem.c_str());
                ImGui::EndDragDropSource();
            }
        }
        else if (ae.type == AssetType::Material)
        {
            if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None))
            {
                ImGui::SetDragDropPayload("IMAT_PATH",
                    ae.path.c_str(), ae.path.size() + 1);
                ImGui::Text("Material: %s", ae.stem.c_str());
                ImGui::EndDragDropSource();
            }
        }
        else if (ae.type == AssetType::Skeleton)
        {
            if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None))
            {
                ImGui::SetDragDropPayload("ISKEL_PATH",
                    ae.path.c_str(), ae.path.size() + 1);
                ImGui::Text("Skeleton: %s", ae.stem.c_str());
                ImGui::EndDragDropSource();
            }
        }
        else if (ae.type == AssetType::Animation)
        {
            if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None))
            {
                ImGui::SetDragDropPayload("IANIM_PATH",
                    ae.path.c_str(), ae.path.size() + 1);
                ImGui::Text("Animation: %s", ae.stem.c_str());
                ImGui::EndDragDropSource();
            }
        }
        else if (ae.type == AssetType::Script)
        {
            if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None))
            {
                ImGui::SetDragDropPayload("ILUA_PATH",
                    ae.path.c_str(), ae.path.size() + 1);
                ImGui::Text("Script: %s", ae.stem.c_str());
                ImGui::EndDragDropSource();
            }
        }
        else if (ae.type == AssetType::Shader)
        {
            // Only .hlsl source is draggable; compiled .ishdr blobs are derived and won't recompile on edit.
            const bool isHlslSource =
                ae.path.size() >= 5 &&
                std::string_view(ae.path).substr(ae.path.size() - 5) == ".hlsl";
            if (isHlslSource && ImGui::BeginDragDropSource(ImGuiDragDropFlags_None))
            {
                ImGui::SetDragDropPayload("IHLSL_PATH",
                    ae.path.c_str(), ae.path.size() + 1);
                ImGui::Text("Shader: %s", ae.stem.c_str());
                ImGui::EndDragDropSource();
            }
        }

        if (tileClicked)
        {
            if (ae.type == AssetType::Folder)
            {
                m_currentDir    = ae.path;
                m_needsRescan   = true;
                m_selectedAsset = -1;
            }
            else
            {
                m_selectedAsset = i;
            }
        }
        ImGui::PopID();

        const bool hovered = ImGui::IsItemHovered();
        ImDrawList* dl = ImGui::GetWindowDrawList();

        ImVec2 tileMax = ImVec2(tileMin.x + kTileSize, tileMin.y + kTileSize);

        // ---- Draw background rect ----
        ImU32 bgColor = selected ? IM_COL32(60, 120, 200, 200)
                      : hovered  ? IM_COL32(80,  80, 100, 180)
                                 : IM_COL32(40,  40,  50, 200);
        dl->AddRectFilled(tileMin, ImVec2(tileMin.x + kTileSize, tileMin.y + kTileSize + kTilePadY + 2.f), bgColor, 4.f);

        // ---- Draw thumbnail or icon ----
        bool usedThumbnail = false;
        if (ae.type == AssetType::Texture && m_textureSys && m_gfx)
        {
            const RHI::Texture* tex = m_textureSys->GetTexture(ae.thumbnail);
            if (tex)
            {
                auto& dx12 = static_cast<GraphicsDX12&>(*m_gfx);
                uint64_t gpuHandle = dx12.GetTexturePreviewSrvGpuHandle(*tex);
                if (gpuHandle != 0)
                {
                    dl->AddImage(
                        ImTextureRef(static_cast<ImTextureID>(gpuHandle)),
                        ImVec2(tileMin.x + 2.f, tileMin.y + 2.f),
                        ImVec2(tileMax.x - 2.f, tileMax.y - 2.f));
                    usedThumbnail = true;
                }
            }
        }

        if (!usedThumbnail)
        {
            // Colored background + large icon character
            dl->AddRectFilled(ImVec2(tileMin.x + 2.f, tileMin.y + 2.f),
                              ImVec2(tileMax.x - 2.f, tileMax.y - 2.f),
                              tileColor(ae.type), 2.f);

            const char* icon = iconChar(ae.type);
            if (icon)
            {
                // Center the icon in the tile
                ImVec2 iconSize = ImGui::CalcTextSize(icon);
                ImVec2 iconPos  = ImVec2(
                    tileMin.x + (kTileSize - iconSize.x) * 0.5f,
                    tileMin.y + (kTileSize - iconSize.y) * 0.5f);
                dl->AddText(iconPos, IM_COL32(255, 255, 255, 230), icon);
            }
            else
            {
                // Fallback: first letter of type
                const char* typeLabel = "?";
                switch (ae.type)
                {
                case AssetType::Texture:  typeLabel = "T"; break;
                case AssetType::Shader:   typeLabel = "S"; break;
                case AssetType::Mesh:     typeLabel = "M"; break;
                case AssetType::Material: typeLabel = "A"; break;
                case AssetType::Scene:    typeLabel = "N"; break;
                case AssetType::Prefab:   typeLabel = "P"; break;
                case AssetType::Skeleton:  typeLabel = "K"; break;
                case AssetType::Animation: typeLabel = "A"; break;
                default: break;
                }
                ImVec2 sz  = ImGui::CalcTextSize(typeLabel);
                ImVec2 pos = ImVec2(tileMin.x + (kTileSize - sz.x) * 0.5f,
                                    tileMin.y + (kTileSize - sz.y) * 0.5f);
                dl->AddText(pos, IM_COL32(255, 255, 255, 230), typeLabel);
            }
        }

        // ---- Label below tile ----
        {
            // Truncate stem to fit kTileSize
            const std::string& stem = ae.stem;
            const float maxLabelW   = kTileSize - 2.f;
            ImVec2 labelPos = ImVec2(tileMin.x + 1.f, tileMax.y + 2.f);

            // Clip label to width via binary search for longest fitting substring + ellipsis.
            const char* textBegin = stem.c_str();
            const char* textEnd   = textBegin + stem.size();
            ImVec2 fullSz = ImGui::CalcTextSize(textBegin, textEnd);
            if (fullSz.x > maxLabelW)
            {
                const char* ellipsis  = "\xe2\x80\xa6";  // UTF-8 "…"
                float       ellipsisW = ImGui::CalcTextSize(ellipsis).x;
                int lo = 0, hi = static_cast<int>(stem.size());
                while (lo < hi)
                {
                    int mid = (lo + hi + 1) / 2;
                    ImVec2 sz = ImGui::CalcTextSize(textBegin, textBegin + mid);
                    if (sz.x + ellipsisW <= maxLabelW) lo = mid;
                    else                               hi = mid - 1;
                }
                std::string clipped = stem.substr(0, lo) + "\xe2\x80\xa6";
                dl->AddText(labelPos, IM_COL32(200, 200, 200, 255), clipped.c_str());
            }
            else
            {
                dl->AddText(labelPos, IM_COL32(200, 200, 200, 255), textBegin, textEnd);
            }
        }

        // ---- Selection border ----
        if (selected)
            dl->AddRect(tileMin, ImVec2(tileMin.x + kTileSize, tileMin.y + kTileSize + kTilePadY + 2.f),
                        IM_COL32(100, 180, 255, 255), 4.f, 0, 2.f);

        // Tooltip: full path
        if (hovered)
            ImGui::SetTooltip("%s", ae.path.c_str());

        // ---- Layout: advance to next column ----
        ++col;
        if (col < columns)
        {
            ImGui::SameLine(0.f, kTilePadX);
        }
        else
        {
            col = 0;
            ImGui::Dummy(ImVec2(0.f, kTilePadY));  // row gap
        }
    }
    // Ensure last row has proper height
    if (col != 0)
        ImGui::Dummy(ImVec2(0.f, kTilePadY));

    ImGui::EndChild();
}

// ---- Resource panel ----
void EditorLayer::RenderResourcePanel()
{
    ImGui::Begin(kWindowResource);

    if (ImGui::BeginTabBar("ResourceLogTabs", ImGuiTabBarFlags_None))
    {
        // ---- Assets tab ----
        if (ImGui::BeginTabItem("Assets"))
        {
            // ---- Navigation bar: Up button + relative path + Refresh ----
            namespace fs = std::filesystem;
            {
                const bool atRoot = m_currentDir.empty() || m_assetDir.empty() ||
                    (fs::path(m_currentDir) == fs::path(m_assetDir));

                if (atRoot) ImGui::BeginDisabled();
                if (ImGui::SmallButton("^ Up"))
                {
                    m_currentDir  = fs::path(m_currentDir).parent_path().string();
                    m_needsRescan = true;
                    m_selectedAsset = -1;
                    // Clamp: never go above m_assetDir
                    std::error_code ec;
                    if (!fs::path(m_currentDir).is_relative() &&
                        !m_assetDir.empty())
                    {
                        // If somehow we went above root, reset to root
                        auto rel = fs::relative(m_currentDir, m_assetDir, ec);
                        if (ec || rel.string().find("..") != std::string::npos)
                            m_currentDir = m_assetDir;
                    }
                }
                if (atRoot) ImGui::EndDisabled();
                ImGui::SameLine();

                // Show path relative to assetDir
                std::string displayPath;
                if (!m_assetDir.empty() && !m_currentDir.empty())
                {
                    std::error_code ec;
                    auto rel = fs::relative(m_currentDir, m_assetDir, ec);
                    displayPath = ec ? m_currentDir : rel.string();
                    if (displayPath == ".") displayPath.clear();
                }
                if (displayPath.empty())
                    ImGui::TextDisabled("(root)");
                else
                    ImGui::TextDisabled("%s", displayPath.c_str());

            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Refresh") || m_needsRescan)
            {
                ScanAssets();
                m_needsRescan    = false;
                m_lastScanTime   = ImGui::GetTime();
            }

            // Auto-rescan every 5 seconds
            if (ImGui::GetTime() - m_lastScanTime > 5.0)
            {
                ScanAssets();
                m_lastScanTime = ImGui::GetTime();
            }

            // ---- Filter row (case-insensitive substring vs AssetEntry::stem). ----
            {
                ImGui::SetNextItemWidth(220.f);
                ImGui::InputTextWithHint("##AssetFilter", "Filter (name)",
                    m_assetFilter, sizeof(m_assetFilter));
                if (m_assetFilter[0] != '\0')
                {
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Clear##AssetFilter"))
                        m_assetFilter[0] = '\0';
                }
            }

            // Asset count summary (totals across all assets — unfiltered).
            int nTex = 0, nShd = 0, nMsh = 0, nMat = 0, nScn = 0, nPfb = 0, nSkel = 0, nAnim = 0;
            for (const auto& a : m_assets)
            {
                switch (a.type)
                {
                case AssetType::Texture:   ++nTex;  break;
                case AssetType::Shader:    ++nShd;  break;
                case AssetType::Mesh:      ++nMsh;  break;
                case AssetType::Material:  ++nMat;  break;
                case AssetType::Scene:     ++nScn;  break;
                case AssetType::Prefab:    ++nPfb;  break;
                case AssetType::Skeleton:  ++nSkel; break;
                case AssetType::Animation: ++nAnim; break;
                default: break;
                }
            }
            ImGui::TextDisabled("Textures: %d   Shaders: %d   Meshes: %d   Materials: %d   Scenes: %d   Prefabs: %d   Skeletons: %d   Animations: %d",
                                nTex, nShd, nMsh, nMat, nScn, nPfb, nSkel, nAnim);
            ImGui::Separator();

            RenderAssetGrid();

            ImGui::EndTabItem();
        }

        // ---- Mesh Library tab ----
        if (ImGui::BeginTabItem("Mesh Library"))
        {
            RenderMeshLibraryTab();
            ImGui::EndTabItem();
        }

        // ---- Log tab ----
        if (ImGui::BeginTabItem("Log"))
        {
            // Per-level "!" filter + text substring; auto-scroll requires user pinned at bottom.
            auto LevelFilter = [](const char* label, ImVec4 color, bool* v) {
                ImGui::PushStyleColor(ImGuiCol_Text, color);
                ImGui::TextUnformatted("!");
                ImGui::PopStyleColor();
                ImGui::SameLine(0.0f, 4.0f);
                ImGui::Checkbox(label, v);
            };

            LevelFilter("Success", LogLevelToColor(LogLevel::Success), &m_logShowSuccess);
            ImGui::SameLine();
            LevelFilter("Info",    LogLevelToColor(LogLevel::Info),    &m_logShowInfo);
            ImGui::SameLine();
            LevelFilter("Warning", LogLevelToColor(LogLevel::Warning), &m_logShowWarning);
            ImGui::SameLine();
            LevelFilter("Error",   LogLevelToColor(LogLevel::Error),   &m_logShowError);
            ImGui::SameLine();
            ImGui::Checkbox("Auto-scroll", &m_logAutoScroll);
            ImGui::SameLine();
            if (ImGui::SmallButton("Clear filter"))
                m_logFilter[0] = 0;

            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::InputTextWithHint("##logFilter", "Filter (substring, case-insensitive)...",
                                     m_logFilter, sizeof(m_logFilter));

            ImGui::Separator();

            std::vector<std::pair<LogLevel, std::string>> lines;
            Logger::Get().GetLinesForImGui(lines);

            // Case-insensitive substring match. Empty filter accepts everything.
            const bool hasTextFilter = m_logFilter[0] != 0;
            const size_t needleLen = hasTextFilter ? std::strlen(m_logFilter) : 0;
            auto matchesText = [&](const std::string& s) {
                if (!hasTextFilter) return true;
                auto it = std::search(
                    s.begin(), s.end(),
                    m_logFilter, m_logFilter + needleLen,
                    [](char a, char b) {
                        return std::tolower(static_cast<unsigned char>(a)) ==
                               std::tolower(static_cast<unsigned char>(b));
                    });
                return it != s.end();
            };

            if (ImGui::BeginChild("LogContent", ImVec2(0, 0), ImGuiChildFlags_None,
                                  ImGuiWindowFlags_HorizontalScrollbar))
            {
                for (const auto& p : lines)
                {
                    switch (p.first)
                    {
                    case LogLevel::Success: if (!m_logShowSuccess) continue; break;
                    case LogLevel::Info:    if (!m_logShowInfo)    continue; break;
                    case LogLevel::Warning: if (!m_logShowWarning) continue; break;
                    case LogLevel::Error:   if (!m_logShowError)   continue; break;
                    }
                    if (!matchesText(p.second)) continue;

                    const ImVec4 col = LogLevelToColor(p.first);
                    ImGui::PushStyleColor(ImGuiCol_Text, col);
                    ImGui::TextUnformatted("!");
                    ImGui::PopStyleColor();
                    ImGui::SameLine(0.0f, 6.0f);

                    ImGui::PushStyleColor(ImGuiCol_Text, col);
                    ImGui::TextUnformatted(p.second.c_str());
                    ImGui::PopStyleColor();
                }

                // Auto-scroll only when pinned to bottom (1px epsilon vs subpixel drift).
                if (m_logAutoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f)
                    ImGui::SetScrollHereY(1.0f);
            }
            ImGui::EndChild();

            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }

    // ---- Full-window IPFB drop target (spawn prefab anywhere in panel) -----
    {
        ImGuiWindow* win = ImGui::GetCurrentWindow();
        if (ImGui::GetDragDropPayload() &&
            ImGui::GetDragDropPayload()->IsDataType("IPFB_PATH"))
        {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImVec2 bMin = win->InnerRect.Min;
            const ImVec2 bMax = win->InnerRect.Max;
            dl->AddRectFilled(bMin, bMax, IM_COL32(30, 80, 30, 60), 3.f);
            dl->AddRect(bMin, bMax, IM_COL32(70, 180, 70, 200), 3.f);
            const char* hint = "Drop .ipfb to spawn prefab";
            const ImVec2 tsz = ImGui::CalcTextSize(hint);
            dl->AddText(
                ImVec2(bMin.x + (bMax.x - bMin.x - tsz.x) * 0.5f,
                       bMax.y - tsz.y - 6.f),
                IM_COL32(120, 220, 120, 230), hint);
        }
        if (ImGui::BeginDragDropTargetCustom(win->InnerRect, win->ID + 1))
        {
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("IPFB_PATH"))
            {
                const char* pfbPath = static_cast<const char*>(p->Data);
                if (m_assetMgr && m_world)
                {
                    Entity spawned = Resource::LoadPrefab(pfbPath, *m_world, *m_assetMgr,
                                                        m_renderer, m_animClipSys);
                    if (spawned != NullEntity)
                    {
                        m_selectedEntity = spawned;
                        LOG_SUCCESS("EditorLayer: spawned prefab '%s' — root entity %u", pfbPath, spawned);
                    }
                    else
                        LOG_ERROR("EditorLayer: failed to spawn prefab '%s'", pfbPath);
                }
                else
                    LOG_ERROR("EditorLayer: assetMgr not wired — cannot spawn prefab");
            }
            ImGui::EndDragDropTarget();
        }
    }

    ImGui::End();
}

// MeshLibrary tab — lists every loaded .meshlib with per-mesh metadata. Rendered inside Resource panel's tab bar.
void EditorLayer::RenderMeshLibraryTab()
{
    Resource::MeshLibrary* meshLib = m_renderer ? m_renderer->GetMeshLibrary() : nullptr;
    if (!meshLib)
    {
        ImGui::TextDisabled("(MeshLibrary not wired)");
        return;
    }

    const auto handles = meshLib->GetLoadedHandles();
    ImGui::Text("Loaded libraries: %zu", handles.size());
    ImGui::Separator();

    if (handles.empty())
    {
        ImGui::TextDisabled("No .meshlib files loaded yet.");
        return;
    }

    for (size_t li = 0; li < handles.size(); ++li)
    {
        const Resource::Handle h = handles[li];
        const std::string path   = meshLib->GetSourcePath(h);
        const uint32_t   count   = meshLib->GetMeshCount(h);
        const auto       totals  = meshLib->GetTotals(h);

        // Rough memory: vertex bytes + index bytes (32B/vtx interleaved, 4B/idx).
        const uint32_t vStride   = meshLib->GetVertexStride(h);
        const double   mb        = double(totals.vertexCount) * vStride / (1024.0 * 1024.0)
                                 + double(totals.indexCount)  * 4.0    / (1024.0 * 1024.0);

        ImGui::PushID(static_cast<int>(li));

        // Header shows basename only; full path on hover + below for long paths.
        std::string basename = path;
        const auto slash = path.find_last_of("/\\");
        if (slash != std::string::npos) basename = path.substr(slash + 1);

        char header[256];
        std::snprintf(header, sizeof(header), "[%zu] %s   (%u meshes, %.1f MB)",
                      li, basename.c_str(), count, mb);

        const bool open = ImGui::TreeNodeEx(header, ImGuiTreeNodeFlags_SpanFullWidth);
        if (ImGui::IsItemHovered() && !path.empty())
            ImGui::SetTooltip("%s", path.c_str());

        if (open)
        {
            ImGui::Text("Path   : %s", path.c_str());
            ImGui::Text("Meshes : %u   Verts: %u   Idx: %u   Stride: %u B",
                        count, totals.vertexCount, totals.indexCount, vStride);
            ImGui::Separator();

            // Per-mesh table is lazy + clipped (Bistro: 22k meshes) so ImGui pass doesn't stall.
            if (ImGui::BeginTable("meshes", 6,
                                  ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInner |
                                  ImGuiTableFlags_RowBg   | ImGuiTableFlags_Resizable,
                                  ImVec2(0.0f, 240.0f)))
            {
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableSetupColumn("meshId",  ImGuiTableColumnFlags_WidthFixed, 60);
                ImGui::TableSetupColumn("vStart",  ImGuiTableColumnFlags_WidthFixed, 70);
                ImGui::TableSetupColumn("vCount",  ImGuiTableColumnFlags_WidthFixed, 70);
                ImGui::TableSetupColumn("iStart",  ImGuiTableColumnFlags_WidthFixed, 80);
                ImGui::TableSetupColumn("iCount",  ImGuiTableColumnFlags_WidthFixed, 70);
                ImGui::TableSetupColumn("AABB (min → max)", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableHeadersRow();

                ImGuiListClipper clipper;
                clipper.Begin(static_cast<int>(count));
                while (clipper.Step())
                {
                    for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row)
                    {
                        const auto* e = meshLib->GetEntry(h, static_cast<uint32_t>(row));
                        if (!e) continue;
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0); ImGui::Text("%d",  row);
                        ImGui::TableSetColumnIndex(1); ImGui::Text("%u",  e->vertexStart);
                        ImGui::TableSetColumnIndex(2); ImGui::Text("%u",  e->vertexCount);
                        ImGui::TableSetColumnIndex(3); ImGui::Text("%u",  e->indexStart);
                        ImGui::TableSetColumnIndex(4); ImGui::Text("%u",  e->indexCount);
                        ImGui::TableSetColumnIndex(5);
                        ImGui::Text("(%.2f,%.2f,%.2f) → (%.2f,%.2f,%.2f)",
                                    e->aabbMin.x, e->aabbMin.y, e->aabbMin.z,
                                    e->aabbMax.x, e->aabbMax.y, e->aabbMax.z);
                    }
                }
                ImGui::EndTable();
            }
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
}

// Decal Materials window — CRUD + inspector for DecalMaterialLibrary; toggled from Tools menu.
void EditorLayer::RenderDecalMaterialsWindow()
{
    // Sized large enough for the split layout on first open; user can resize.
    ImGui::SetNextWindowSize(ImVec2(720.f, 540.f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Decal Materials", &m_showDecalMaterials))
    {
        ImGui::End();
        return;
    }

    if (!m_renderer)
    {
        ImGui::TextDisabled("(Renderer not wired)");
        ImGui::End();
        return;
    }
    auto& lib = m_renderer->GetDecalMaterialLibrary();
    const auto& assets = lib.GetAll();   // std::unordered_map<string, shared_ptr<DecalMaterialAsset>>

    const bool haveTexSys = (m_textureSys != nullptr && m_gfx != nullptr);

    // ---- Top bar: Create new asset -----------------------------------------
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Name:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(200.f);
    ImGui::InputTextWithHint("##new_decal_mat_name", "e.g. blood_a",
                             m_newDecalMatNameBuf, sizeof(m_newDecalMatNameBuf));
    ImGui::SameLine();
    const bool canCreate = (m_newDecalMatNameBuf[0] != '\0'
                         && assets.find(m_newDecalMatNameBuf) == assets.end());
    if (!canCreate) ImGui::BeginDisabled();
    if (ImGui::Button("Create"))
    {
        lib.GetOrCreate(m_newDecalMatNameBuf);
        m_selectedDecalMatName = m_newDecalMatNameBuf;
        m_newDecalMatNameBuf[0] = '\0';
    }
    if (!canCreate) ImGui::EndDisabled();

    // Load .idecalmat — adds (or overwrites) an asset in the library.
    ImGui::SameLine();
    static constexpr const char* kDecalMatFilter =
        "Decal Material (*.idecalmat)\0*.idecalmat\0All Files\0*.*\0\0";
    if (ImGui::Button("Load..."))
    {
        std::string path = OpenFileDialog(
            kDecalMatFilter, m_assetDir.empty() ? nullptr : m_assetDir.c_str());
        if (!path.empty())
        {
            // Load to scratch first to read embedded name; copy into named slot so key matches file identity.
            Resource::DecalMaterialAsset scratch;
            if (Resource::LoadDecalMaterial(scratch, path))
            {
                const std::string libName = scratch.name.empty() ? path : scratch.name;
                auto asset = lib.GetOrCreate(libName);
                // Copy fields; keep the library-assigned name as the key.
                for (uint32_t s = 0; s < Resource::DecalMaterialAsset::SLOT_COUNT; ++s)
                    asset->texPaths[s] = scratch.texPaths[s];
                asset->baseColorTint    = scratch.baseColorTint;
                asset->opacity          = scratch.opacity;
                asset->roughness        = scratch.roughness;
                asset->specular         = scratch.specular;
                asset->ao               = scratch.ao;
                asset->normalStrength   = scratch.normalStrength;
                asset->bumpStrength     = scratch.bumpStrength;
                asset->cavityStrength   = scratch.cavityStrength;
                asset->displacementScale = scratch.displacementScale;
                asset->flags            = scratch.flags;
                asset->normalBlendMode  = scratch.normalBlendMode;
                asset->angleFadeStart   = scratch.angleFadeStart;
                asset->sortLayer        = scratch.sortLayer;
                m_selectedDecalMatName  = libName;
            }
        }
    }

    ImGui::SameLine();
    ImGui::TextDisabled("(%zu asset%s)", assets.size(), assets.size() == 1 ? "" : "s");

    ImGui::Separator();

    // ---- Split layout: list | inspector -------------------------------------
    const float availW  = ImGui::GetContentRegionAvail().x;
    const float listW   = (availW > 500.f) ? 200.f : availW * 0.35f;

    // Left: selectable list
    if (ImGui::BeginChild("##decal_mat_list", ImVec2(listW, 0), ImGuiChildFlags_Borders))
    {
        // Sort names alphabetically for stable UI ordering.
        std::vector<const std::string*> sortedNames;
        sortedNames.reserve(assets.size());
        for (const auto& [name, _] : assets) sortedNames.push_back(&name);
        std::sort(sortedNames.begin(), sortedNames.end(),
                  [](const std::string* a, const std::string* b) { return *a < *b; });

        for (const std::string* namePtr : sortedNames)
        {
            const bool selected = (m_selectedDecalMatName == *namePtr);
            if (ImGui::Selectable(namePtr->c_str(), selected))
                m_selectedDecalMatName = *namePtr;
        }
        if (assets.empty())
            ImGui::TextDisabled("(empty)");
    }
    ImGui::EndChild();

    ImGui::SameLine();

    // Right: inspector for selected asset
    ImGui::BeginChild("##decal_mat_inspector", ImVec2(0, 0), ImGuiChildFlags_Borders);

    auto selectedAsset = lib.Find(m_selectedDecalMatName);
    if (!selectedAsset)
    {
        ImGui::TextDisabled("Select a decal material on the left, or Create a new one.");
    }
    else
    {
        auto& a = *selectedAsset;

        // Name (read-only: renaming would invalidate library key + serialization)
        ImGui::Text("Name: %s", a.name.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Save..."))
        {
            std::string path = SaveFileDialog(
                kDecalMatFilter, "idecalmat",
                m_assetDir.empty() ? nullptr : m_assetDir.c_str());
            if (!path.empty())
                Resource::SaveDecalMaterial(a, path);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Delete"))
        {
            if (haveTexSys)
                lib.Destroy(a.name, *m_textureSys, *m_gfx);
            m_selectedDecalMatName.clear();
            ImGui::EndChild();
            ImGui::End();
            return;
        }

        ImGui::Separator();

        // ---- Per-slot texture widget (Unreal-style 9 channels): path + Clear + ITEX drop + 64×64 thumb. ----
        auto drawSlot = [&](Resource::DecalMaterialAsset::TextureSlot slot)
        {
            constexpr float kPreviewSize = 48.f;
            ImGui::PushID(static_cast<int>(slot));

            const int32_t      bindless = a.texBindless[slot];
            const std::string& pathField = a.texPaths[slot];

            // Thumbnail
            uint64_t gpuHandle = 0;
            if (bindless >= 0 && haveTexSys)
            {
                Resource::TextureHandle h = a.texHandles[slot];
                if (h != Resource::kInvalidTextureHandle)
                    if (const RHI::Texture* tex = m_textureSys->GetTexture(h))
                        gpuHandle = m_gfx->GetTextureSRVGpuHandle(*tex);
            }
            if (gpuHandle != 0)
                ImGui::Image(ImTextureRef(static_cast<ImTextureID>(gpuHandle)),
                             ImVec2(kPreviewSize, kPreviewSize));
            else
            {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.18f, 0.18f, 1.f));
                ImGui::Button("##prev", ImVec2(kPreviewSize, kPreviewSize));
                ImGui::PopStyleColor();
            }
            ImGui::SameLine();

            ImGui::BeginGroup();
            ImGui::TextUnformatted(Resource::DecalMaterialAsset::SlotLabel(slot));

            char buf[256];
            strncpy_s(buf, sizeof(buf), pathField.c_str(), _TRUNCATE);
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 70.f);
            if (ImGui::InputText("##path", buf, sizeof(buf),
                                 ImGuiInputTextFlags_EnterReturnsTrue))
            {
                if (haveTexSys)
                    a.SetTexturePath(slot, std::string(buf), *m_textureSys, *m_gfx);
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Clear") && haveTexSys)
                a.SetTexturePath(slot, std::string{}, *m_textureSys, *m_gfx);

            ImGui::TextDisabled("%s",
                bindless >= 0       ? "bound"
                : pathField.empty() ? "empty"
                                    : "loading");
            ImGui::EndGroup();

            if (ImGui::BeginDragDropTarget())
            {
                if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("ITEX_PATH"))
                {
                    const char* texPath = static_cast<const char*>(p->Data);
                    if (haveTexSys && texPath)
                        a.SetTexturePath(slot, std::string(texPath), *m_textureSys, *m_gfx);
                }
                ImGui::EndDragDropTarget();
            }
            ImGui::PopID();
        };

        auto flagCheckbox = [&](const char* label, uint32_t bit)
        {
            bool v = (a.flags & bit) != 0;
            if (ImGui::Checkbox(label, &v))
            {
                if (v) a.flags |=  bit;
                else   a.flags &= ~bit;
            }
        };

        // ---- Group: Base --------------------------------------------------
        if (ImGui::CollapsingHeader("Base", 0))
        {
            drawSlot(Resource::DecalMaterialAsset::SLOT_BASECOLOR);
            drawSlot(Resource::DecalMaterialAsset::SLOT_OPACITY);
            ImGui::ColorEdit4("Tint", &a.baseColorTint.x, ImGuiColorEditFlags_AlphaBar);
            ImGui::SliderFloat("Opacity Scalar", &a.opacity, 0.0f, 1.0f,
                               "%.2f  (× Opacity tex × Tint.a)");
            flagCheckbox("Write BaseColor", Resource::DecalMaterialAsset::WRITE_BASECOLOR);
        }

        // ---- Group: Surface (Roughness / Specular / AO) -------------------
        if (ImGui::CollapsingHeader("Surface", 0))
        {
            drawSlot(Resource::DecalMaterialAsset::SLOT_ROUGHNESS);
            ImGui::SliderFloat("Roughness##s", &a.roughness, 0.0f, 1.0f,
                               "%.2f  (× Roughness tex.r if set)");
            flagCheckbox("Write Roughness", Resource::DecalMaterialAsset::WRITE_ROUGHNESS);
            ImGui::Separator();

            drawSlot(Resource::DecalMaterialAsset::SLOT_SPECULAR);
            ImGui::SliderFloat("Specular##s", &a.specular, 0.0f, 1.0f,
                               "%.2f  (dielectric F0)");
            flagCheckbox("Write Specular", Resource::DecalMaterialAsset::WRITE_SPECULAR);
            ImGui::Separator();

            drawSlot(Resource::DecalMaterialAsset::SLOT_AO);
            ImGui::SliderFloat("AO##s", &a.ao, 0.0f, 1.0f,
                               "%.2f  (× AO tex.r if set)");
            flagCheckbox("Write AO", Resource::DecalMaterialAsset::WRITE_AO);
        }

        // ---- Group: Normal / Bump -----------------------------------------
        if (ImGui::CollapsingHeader("Normal", 0))
        {
            drawSlot(Resource::DecalMaterialAsset::SLOT_NORMAL);
            ImGui::SliderFloat("Normal Strength", &a.normalStrength, 0.0f, 4.0f, "%.2f");

            drawSlot(Resource::DecalMaterialAsset::SLOT_BUMP);
            ImGui::SliderFloat("Bump Strength", &a.bumpStrength, 0.0f, 2.0f,
                               "%.3f  (0 = flat, 0.1 = subtle, 1.0 = physical, 2+ = exaggerated)");

            flagCheckbox("Write Normal", Resource::DecalMaterialAsset::WRITE_NORMAL);

            static const char* kBlendLabels[] = { "RNM (recommended)", "Lerp" };
            int blendIdx = static_cast<int>(a.normalBlendMode);
            if (ImGui::Combo("Normal Blend", &blendIdx, kBlendLabels, IM_ARRAYSIZE(kBlendLabels)))
                a.normalBlendMode = static_cast<Resource::DecalMaterialAsset::NormalBlendMode>(blendIdx);
        }

        // ---- Group: Detail (Cavity + Displacement) -------------------------
        if (ImGui::CollapsingHeader("Detail"))
        {
            drawSlot(Resource::DecalMaterialAsset::SLOT_CAVITY);
            ImGui::SliderFloat("Cavity Strength", &a.cavityStrength, 0.0f, 1.0f,
                               "%.2f  (0 = ignore, 1 = full darkening)");
            ImGui::Separator();
            drawSlot(Resource::DecalMaterialAsset::SLOT_DISPLACEMENT);
            ImGui::SliderFloat("Displacement Scale", &a.displacementScale, 0.0f, 0.2f,
                               "%.3f  (parallax UV offset; 0 = off)");
        }

        // ---- Group: Blend & Sort -------------------------------------------
        if (ImGui::CollapsingHeader("Blend & Sort", 0))
        {
            ImGui::SliderFloat("Angle Fade Start", &a.angleFadeStart, 0.0f, 1.0f,
                               "%.2f  (dot(N,Z) threshold)");
            ImGui::DragInt("Sort Layer", &a.sortLayer, 1.0f, -128, 127);
        }

        // ---- Status line ----------------------------------------------------
        ImGui::Separator();
        const bool resolved = a.IsFullyResolved();
        ImGui::TextColored(
            resolved ? ImVec4(0.4f, 0.9f, 0.4f, 1.f) : ImVec4(0.9f, 0.8f, 0.3f, 1.f),
            resolved ? "All textures resolved." : "Some textures still loading...");
    }

    ImGui::EndChild();
    ImGui::End();
}

// ---- Post Processing / Color Grading panel ----
void EditorLayer::RenderPostProcessPanel()
{
    ImGui::Begin("Post Processing");

    ToneMapPass* toneMap = m_renderer ? m_renderer->GetToneMapPass() : nullptr;
    if (!toneMap)
    {
        ImGui::TextDisabled("(renderer not available)");
        ImGui::End();
        return;
    }

    // PostProcess::Stack hosts the authoritative ParameterStore; adapters push values on Execute().
    PostProcess::Stack* ppStack = m_renderer->GetPostProcessStack();
    if (!ppStack)
    {
        ImGui::TextDisabled("(post-process stack not initialized)");
        ImGui::End();
        return;
    }
    PostProcess::ParameterStore& ppParams = ppStack->GetParameters();

    // ---- Config Save/Load — persists panel state as .ippc; bound path stamped into next SaveScene. ----
    if (ImGui::CollapsingHeader("Config", 0))
    {
        static constexpr const char* kPPCFilter =
            "Post-Process Config (*.ippc)\0*.ippc\0All Files\0*.*\0\0";

        if (ImGui::Button("Save Config..."))
        {
            std::string path = SaveFileDialog(kPPCFilter, "ippc",
                m_assetDir.empty() ? nullptr : m_assetDir.c_str());
            if (!path.empty())
            {
                Resource::PostProcessConfig cfg;
                cfg.CaptureFrom(*m_renderer);
                if (Resource::SavePostProcessConfig(cfg, path))
                    m_postProcessConfigPath = path;
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Load Config..."))
        {
            std::string path = OpenFileDialog(kPPCFilter,
                m_assetDir.empty() ? nullptr : m_assetDir.c_str());
            if (!path.empty())
            {
                Resource::PostProcessConfig cfg;
                if (Resource::LoadPostProcessConfig(path, cfg))
                {
                    cfg.ApplyTo(*m_renderer);
                    m_postProcessConfigPath = path;
                }
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Clear Binding"))
            m_postProcessConfigPath.clear();

        if (m_postProcessConfigPath.empty())
            ImGui::TextDisabled("(no config bound — scene will save without one)");
        else
            ImGui::TextWrapped("Bound: %s", m_postProcessConfigPath.c_str());
    }

    ImGui::Separator();

    // ---- Bloom Strength ----------------------------------------------------------------

    ImGui::SliderFloat("Bloom Strength",
        &ppParams.GetTonemapping().bloomStrength, 0.f, 1.f, "%.3f");
    ImGui::Separator();

    // ---- AA mode selector --------------------------------------------------
    // Drives both TAA and FXAA enabled flags via Renderer::SetAAMode. Jitter
    // follows TAA's IsEnabled, so FXAA-only / None modes render without jitter.
    if (m_renderer)
    {
        const char* kAAModes[] = { "None", "FXAA", "TAA", "FXAA + TAA" };
        int aaMode = static_cast<int>(m_renderer->GetAAMode());
        if (ImGui::Combo("AA Mode", &aaMode, kAAModes, IM_ARRAYSIZE(kAAModes)))
            m_renderer->SetAAMode(static_cast<Renderer::AAMode>(aaMode));
        ImGui::SameLine();
        ImGui::TextDisabled("(FXAA spatial, TAA temporal, combo for both)");
    }

    // ---- TAA ----------------------------------------------------------------
    if (TAAPass* taa = m_renderer ? m_renderer->GetTAAPass() : nullptr)
    {
        if (ImGui::CollapsingHeader("Temporal AA (TAA)", 0))
        {

            // Direct history blend weight. α_diffuse = 1 - w; α_spec = α_diffuse*0.125.
            // 0.0 ≈ TAA off (each frame fully replaced), 0.9 default (UE-class
            // integration), 0.95 cinematic. Drop toward 0.5 for ghost-prone
            // surfaces (foliage / curtains). Replaced the τ(seconds) slider on
            // 2026-05-24 — see TAA_Common.hlsli cbuffer comment for rationale.
            ImGui::SliderFloat("History weight", &taa->historyWeight, 0.0f, 0.99f, "%.3f");
            ImGui::SameLine();
            ImGui::TextDisabled("(higher = more history, more ghost-prone)");

            // Variance-clip box widths (YCoCg AABB). Lower=tighter clamp; Falcor default 1.0, 1.5 reduces distant shimmer.
            ImGui::SliderFloat("Box σ (base)",       &taa->colorBoxSigma,         0.5f, 4.0f, "%.2f");
            ImGui::SliderFloat("Box σ (specular)",   &taa->colorBoxSigmaSpecular, 0.5f, 4.0f, "%.2f");
            ImGui::SliderFloat("Specular roughness", &taa->specularRoughnessMax,  0.0f, 1.0f, "%.2f");

            // Distance-to-clamp anti-flicker (Karis 2014); leans on history when pre-clip near AABB boundary.
            ImGui::Checkbox("Anti-flicker (distance-to-clamp)", &taa->antiFlicker);

            // Motion-proportional AABB widening. 0=tight clamp (Karis-dim peaks); 1=AABB doubles at full motion. Higher for HDR scenes.
            ImGui::SliderFloat("Velocity widen", &taa->velocityWiden, 0.0f, 2.0f, "%.2f");

            // Karis 5-tap unsharp on de-jittered curr. 0=off, 0.1 default. >0.2 ringy on high-contrast edges.
            ImGui::SliderFloat("Sharpen strength", &taa->sharpenStrength, 0.0f, 0.3f, "%.2f");

            // Outline-aware history weakening. OutlinePass tags rim pixels via
            // stencil bit; TAA floors α to this value on those pixels so the
            // outline tracks the silhouette instead of ghosting. 0 disables;
            // 0.5 ≈ 3-frame convergence; 1.0 = no history (slightly aliased rim).
            ImGui::SliderFloat("Outline α floor", &taa->outlineMinAlpha, 0.0f, 1.0f, "%.2f");
        }
    }

    // ---- FXAA --------------------------------------------------------------
    if (FXAAPass* fxaa = m_renderer ? m_renderer->GetFXAAPass() : nullptr)
    {
        if (ImGui::CollapsingHeader("Fast Approximate AA (FXAA)", 0))
        {
            ImGui::TextDisabled("Active when AA Mode is FXAA or FXAA + TAA");
            // Quality knobs from NVIDIA FXAA 3.11 reference.
            // qualitySubpix: subpixel softening. 0 hard edges, 0.75 default, 1 max blur.
            ImGui::SliderFloat("Subpixel quality",     &fxaa->qualitySubpix,           0.0f, 1.0f,  "%.2f");
            // Edge threshold: relative contrast minimum. 0.063 high quality, 0.166 default.
            ImGui::SliderFloat("Edge threshold",       &fxaa->qualityEdgeThreshold,    0.04f, 0.4f, "%.3f");
            // Edge threshold min: absolute floor. 0.0312 high, 0.0833 default.
            ImGui::SliderFloat("Edge threshold (min)", &fxaa->qualityEdgeThresholdMin, 0.02f, 0.2f, "%.4f");
        }
    }

    // ---- Outline DIAG -------------------------------------------------------
    // TEMP: toggles to isolate which sub-pass causes the perceived "outline
    // lag" during character motion.  See OutlinePass.h for details.
    if (OutlinePass* op = m_renderer ? m_renderer->GetOutlinePass() : nullptr)
    {
        if (ImGui::CollapsingHeader("Outline DIAG", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::Checkbox("Skip Hull (sub-pass 1)",         &op->diagSkipHull);
            ImGui::Checkbox("Skip Screen-space (sub-pass 3)", &op->diagSkipScreenSpace);
            ImGui::TextDisabled("Flip during motion to find which layer lags.");
        }
    }

    // ---- XeGTAO (Screen-space AO) -------------------------------------------
    if (XeGTAOPass* gtao = m_renderer ? m_renderer->GetXeGTAOPass() : nullptr)
    {
        if (ImGui::CollapsingHeader("Screen-space AO (XeGTAO)"))
        {
            bool aoOn = m_renderer->IsSSAOEnabled();
            if (ImGui::Checkbox("Enabled##XeGTAO", &aoOn))
                m_renderer->SetSSAOEnabled(aoOn);

            ImGui::TextDisabled("Quality");
            // Reference presets: Low=1×2  Medium=2×2  High=3×3  Ultra=9×3
            int sliceCount    = int(gtao->sliceCount);
            int stepsPerSlice = int(gtao->stepsPerSlice);
            if (ImGui::SliderInt("Slice Count",      &sliceCount,    1, 9))
                gtao->sliceCount    = uint32_t(sliceCount);
            if (ImGui::SliderInt("Steps per Slice",  &stepsPerSlice, 1, 6))
                gtao->stepsPerSlice = uint32_t(stepsPerSlice);

            ImGui::TextDisabled("Effect shape");
            ImGui::SliderFloat("Radius",                  &gtao->effectRadius,             0.05f, 5.0f,  "%.3f");
            ImGui::SliderFloat("Radius Multiplier",       &gtao->radiusMultiplier,         0.5f,  3.0f,  "%.3f");
            ImGui::SliderFloat("Falloff Range",           &gtao->effectFalloffRange,       0.0f,  1.0f,  "%.3f");
            ImGui::SliderFloat("Sample Distribution Pow", &gtao->sampleDistributionPower,  1.0f,  3.0f,  "%.2f");
            ImGui::SliderFloat("Thin Occluder Comp",      &gtao->thinOccluderCompensation, 0.0f,  1.0f,  "%.2f");
            ImGui::SliderFloat("Final Value Power",       &gtao->finalValuePower,          0.5f,  4.0f,  "%.2f");
            ImGui::SliderFloat("Depth MIP Offset",        &gtao->depthMIPSamplingOffset,   0.0f,  5.0f,  "%.2f");

            ImGui::TextDisabled("Temporal + denoise");
            ImGui::SliderFloat("Denoise Blur β",          &gtao->denoiseBlurBeta,          0.0f,  5.0f,  "%.2f");
            ImGui::SliderFloat("Temporal History α",      &gtao->temporalHistoryAlpha,     0.0f,  0.98f, "%.3f");
            ImGui::SameLine();
            ImGui::TextDisabled("(0 = off)");
            ImGui::SliderFloat("Temporal Reject |Δ|",     &gtao->temporalRejectionDiff,    0.1f,  1.0f,  "%.2f");

            if (ImGui::Button("Reset to XeGTAO Defaults"))
            {
                gtao->effectRadius             = 0.5f;
                gtao->radiusMultiplier         = 1.457f;
                gtao->effectFalloffRange       = 0.615f;
                gtao->sampleDistributionPower  = 2.0f;
                gtao->thinOccluderCompensation = 0.0f;
                gtao->finalValuePower          = 2.2f;
                gtao->depthMIPSamplingOffset   = 3.30f;
                gtao->denoiseBlurBeta          = 1.2f;
                gtao->sliceCount               = 3;
                gtao->stepsPerSlice            = 3;
                gtao->temporalHistoryAlpha     = 0.9f;
                gtao->temporalRejectionDiff    = 0.5f;
            }
        }
    }

    // ---- CAS Sharpening -----------------------------------------------------
    if (ImGui::CollapsingHeader("Sharpening (AMD CAS)", 0))
    {
        PostProcess::CASParams& cas = ppParams.GetCAS();
        ImGui::Checkbox("Enabled##CAS", &cas.enabled);
        ImGui::SameLine();
        ImGui::TextDisabled("(FidelityFX Contrast Adaptive Sharpening)");

        // 0=off, 1=max; ~0.4 cleans TAA softness, >0.6 reveals noise.
        ImGui::SliderFloat("Sharpness", &cas.sharpness, 0.0f, 1.0f, "%.2f");
        ImGui::TextDisabled("0 = off | 0.3-0.5 subtle | 0.8+ aggressive");
    }




    ImGui::Separator();

    // ---- Auto Exposure ------------------------------------------------------
    if (ImGui::CollapsingHeader("Auto Exposure", 0))
    {
        PostProcess::AutoExposureParams& ae = ppParams.GetAutoExposure();

        ImGui::Checkbox("Enabled", &ae.enabled);
        ImGui::SameLine();
        ImGui::TextDisabled("(off = use manual exposure)");

        if (!ae.enabled)
        {
            ImGui::SliderFloat("Manual Exposure", &ae.manualExposure,
                0.05f, 16.f, "%.3f", ImGuiSliderFlags_Logarithmic);
            ImGui::Separator();
        }

        ImGui::BeginDisabled(!ae.enabled);

        // EV bias: the main "subject too dark because of bright sky" knob.
        ImGui::SliderFloat("EV Compensation", &ae.evBias, -3.f, 3.f, "%.2f EV");
        ImGui::SameLine();
        ImGui::TextDisabled("(+ = brighter)");

        ImGui::SliderFloat("Key (middle-grey)", &ae.keyValue, 0.05f, 0.5f, "%.3f");
        ImGui::SliderFloat("Min Exposure",      &ae.minExposure, 0.01f, 5.f, "%.2f");
        ImGui::SliderFloat("Max Exposure",      &ae.maxExposure, 0.1f, 32.f, "%.2f");

        // Histogram clipping — tighten to ignore sky/highlight pixels.
        bool pctChanged = false;
        pctChanged |= ImGui::SliderFloat("Low Percentile",  &ae.lowPercent,  0.f, 0.9f, "%.2f");
        pctChanged |= ImGui::SliderFloat("High Percentile", &ae.highPercent, 0.1f, 1.f, "%.2f");
        if (pctChanged && ae.lowPercent > ae.highPercent - 0.01f)
            ae.lowPercent = ae.highPercent - 0.01f;

        ImGui::TextDisabled("Tip: raise EV Compensation or lower High Percentile");
        ImGui::TextDisabled("when a bright IBL sky crushes character exposure.");

        ImGui::EndDisabled();
    }

    ImGui::Separator();

    // ---- Sky / Atmosphere knobs now live on AtmosphereComponent. ------------
    // Select the Sky entity in Hierarchy to edit procedural-atmosphere fields.

    // ---- Volumetric Fog -----------------------------------------------------
    VolumetricFogPass* fogPass = m_renderer ? m_renderer->GetVolumetricFogPass() : nullptr;
    if (fogPass && ImGui::CollapsingHeader("Volumetric Fog", 0))
    {
        bool fogOn = fogPass->IsEnabled();
        if (ImGui::Checkbox("Enable Volumetric Fog", &fogOn))
            fogPass->SetEnabled(fogOn);

        float density = fogPass->GetDensity();
        if (ImGui::SliderFloat("Fog Density", &density, 0.0f, 0.5f, "%.4f"))
            fogPass->SetDensity(density);

        float scat = fogPass->GetScattering();
        if (ImGui::SliderFloat("Scattering", &scat, 0.0f, 2.0f, "%.2f"))
            fogPass->SetScattering(scat);

        float absorp = fogPass->GetAbsorption();
        if (ImGui::SliderFloat("Absorption", &absorp, 0.0f, 0.2f, "%.4f"))
            fogPass->SetAbsorption(absorp);

        float aniso = fogPass->GetAnisotropy();
        if (ImGui::SliderFloat("Anisotropy (HG g)", &aniso, -0.99f, 0.99f, "%.2f"))
            fogPass->SetAnisotropy(aniso);

        float hStart = fogPass->GetHeightStart();
        float hFall  = fogPass->GetHeightFalloff();
        bool hChanged = false;
        hChanged |= ImGui::SliderFloat("Height Start", &hStart, -50.0f, 200.0f, "%.1f");
        hChanged |= ImGui::SliderFloat("Height Falloff", &hFall, 0.0f, 1.0f, "%.4f");
        if (hChanged) fogPass->SetHeight(hStart, hFall);

        float zNear = fogPass->GetRangeNear();
        float zFar  = fogPass->GetRangeFar();
        bool rChanged = false;
        rChanged |= ImGui::SliderFloat("Froxel Near", &zNear, 0.1f, 10.0f, "%.2f");
        rChanged |= ImGui::SliderFloat("Froxel Far",  &zFar, 10.0f, 500.0f, "%.1f");
        if (rChanged) fogPass->SetRange(zNear, zFar);

        float ambContrib = fogPass->GetAmbientContribution();
        if (ImGui::SliderFloat("Ambient Contribution", &ambContrib, 0.0f, 5.0f, "%.2f"))
            fogPass->SetAmbient({0.4f, 0.55f, 0.85f}, ambContrib);

        // Temporal reprojection (smooths noise / amortises samples).
        bool tempOn = fogPass->IsTemporalEnabled();
        if (ImGui::Checkbox("Temporal Reprojection", &tempOn))
            fogPass->SetTemporalEnabled(tempOn);
        if (tempOn)
        {
            float tAlpha = fogPass->GetTemporalAlpha();
            if (ImGui::SliderFloat("Temporal Alpha", &tAlpha, 0.01f, 1.0f, "%.3f"))
                fogPass->SetTemporalAlpha(tAlpha);
            ImGui::TextDisabled("Lower = more smoothing, slower to react.");
        }

        ImGui::TextDisabled("Apply pass blends scattering between Lighting & post-process.");

        // Live diagnostic — verifies VolumetricLightComponent is reaching the shader.
        ImGui::Separator();
        ImGui::TextDisabled("Live State (from scene)");
        float sunStr = fogPass->GetSunStrengthDebug();
        const auto&  sd = fogPass->GetSunDirDebug();
        const auto&  sc = fogPass->GetSunColorDebug();
        ImGui::Text("Sun Strength: %.3f %s", sunStr,
                    sunStr > 0.001f ? "[ACTIVE]" : "[OFF — add VolumetricLightComponent]");
        ImGui::Text("Sun Dir:  (%.2f, %.2f, %.2f)", sd.x, sd.y, sd.z);
        ImGui::Text("Sun Color:(%.2f, %.2f, %.2f)", sc.x, sc.y, sc.z);
        ImGui::Text("Volumetric Lights: %u", fogPass->GetVolumetricLightCount());
    }

    ImGui::Separator();

    // Color Grading
    PostProcess::TonemappingParams& tm = ppParams.GetTonemapping();
    ImGui::Checkbox("Color Grading", &tm.colorGradingEnabled);

    if (tm.colorGradingEnabled)
    {
        ColorGradingParams& p = tm.grading;

        if (ImGui::CollapsingHeader("Tone", 0))
        {
            ImGui::SliderFloat("Exposure",   &p.exposure,   -5.f, 5.f,  "%.2f EV");
            ImGui::SliderFloat("Contrast",   &p.contrast,    0.2f, 3.f,  "%.2f");
            ImGui::SliderFloat("Brightness", &p.brightness, -0.5f, 0.5f, "%.3f");
        }

        if (ImGui::CollapsingHeader("White Balance"))
        {
            ImGui::SliderFloat("Temperature", &p.temperature, -1.f, 1.f, "%.2f");
            ImGui::SliderFloat("Tint",        &p.tint,        -1.f, 1.f, "%.2f");
        }

        if (ImGui::CollapsingHeader("Color"))
        {
            ImGui::SliderFloat("Saturation", &p.saturation, 0.f, 2.f, "%.2f");
            ImGui::SliderFloat("Vibrance",   &p.vibrance,   0.f, 2.f, "%.2f");
            ImGui::SliderFloat("Hue Shift",  &p.hueShift, -180.f, 180.f, "%.1f deg");
        }

        if (ImGui::CollapsingHeader("Lift / Gamma / Gain"))
        {
            ImGui::ColorEdit3("Lift",  &p.lift.x);
            ImGui::ColorEdit3("Gamma", &p.gamma.x);
            ImGui::ColorEdit3("Gain",  &p.gain.x);
        }

        if (ImGui::Button("Reset to Defaults"))
            p = ColorGradingParams{};
    }

    // ---- Volumes — spatial overrides; each layers per-stage override onto base by camera-distance weight. ----
    ImGui::Separator();
    if (ImGui::CollapsingHeader("Volumes", 0))
    {
        PostProcess::VolumeSystem* vs = m_renderer->GetPostProcessVolumes();
        if (!vs)
        {
            ImGui::TextDisabled("(volume system not available)");
        }
        else
        {
            if (ImGui::Button("+ Global"))
            {
                PostProcess::Volume v;
                v.shape = PostProcess::VolumeShape::Global;
                vs->Register(v);
            }
            ImGui::SameLine();
            if (ImGui::Button("+ Box"))
            {
                PostProcess::Volume v;
                v.shape = PostProcess::VolumeShape::Box;
                vs->Register(v);
            }
            ImGui::SameLine();
            if (ImGui::Button("+ Sphere"))
            {
                PostProcess::Volume v;
                v.shape = PostProcess::VolumeShape::Sphere;
                vs->Register(v);
            }
            ImGui::SameLine();
            ImGui::TextDisabled("%zu volume(s)", vs->Size());

            // Snapshot handles — delete range is invalidation-safe; pending deletes flushed at end.
            std::vector<PostProcess::VolumeHandle> handles;
            vs->ForEach([&](PostProcess::VolumeHandle h, const PostProcess::Volume&)
            {
                handles.push_back(h);
            });

            PostProcess::VolumeHandle pendingDelete = PostProcess::kInvalidVolumeHandle;

            for (PostProcess::VolumeHandle h : handles)
            {
                PostProcess::Volume* v = vs->Get(h);
                if (!v) continue;

                ImGui::PushID(static_cast<int>(h));

                const char* shapeName =
                      (v->shape == PostProcess::VolumeShape::Global) ? "Global"
                    : (v->shape == PostProcess::VolumeShape::Box)    ? "Box"
                    :                                                  "Sphere";
                char header[96];
                snprintf(header, sizeof(header), "[%u] %s%s%s",
                    h, shapeName,
                    v->label[0] ? " - " : "",
                    v->label);

                if (ImGui::TreeNode(header))
                {
                    ImGui::Checkbox("Enabled", &v->enabled);
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Delete"))
                        pendingDelete = h;

                    ImGui::InputText("Label", v->label, sizeof(v->label));

                    int shape = static_cast<int>(v->shape);
                    static const char* kShapes[] = { "Global", "Box", "Sphere" };
                    if (ImGui::Combo("Shape", &shape, kShapes, 3))
                        v->shape = static_cast<PostProcess::VolumeShape>(shape);

                    if (v->shape != PostProcess::VolumeShape::Global)
                    {
                        ImGui::DragFloat3("Center", &v->center.x, 0.1f);
                        if (v->shape == PostProcess::VolumeShape::Sphere)
                            ImGui::DragFloat("Radius", &v->extents.x, 0.1f, 0.0f, 1000.0f);
                        else
                            ImGui::DragFloat3("Half-Extents", &v->extents.x,
                                0.1f, 0.0f, 1000.0f);
                        ImGui::DragFloat("Blend Distance", &v->blendDistance,
                            0.05f, 0.0f, 100.0f);
                    }
                    ImGui::InputInt("Priority", &v->priority);

                    ImGui::Separator();
                    ImGui::TextDisabled("Overrides");

                    // Enabling an override seeds the optional with current base params (sane starting point).

                    // --- CAS ---
                    {
                        bool has = v->override.cas.has_value();
                        if (ImGui::Checkbox("Override CAS", &has))
                        {
                            if (has) v->override.cas = ppParams.GetCAS();
                            else     v->override.cas.reset();
                        }
                        if (v->override.cas)
                        {
                            auto& cas = *v->override.cas;
                            ImGui::Indent();
                            ImGui::Checkbox("Enabled##vol_cas", &cas.enabled);
                            ImGui::SliderFloat("Sharpness##vol_cas",
                                &cas.sharpness, 0.0f, 1.0f, "%.2f");
                            ImGui::Unindent();
                        }
                    }

                    // --- AutoExposure ---
                    {
                        bool has = v->override.autoExposure.has_value();
                        if (ImGui::Checkbox("Override AutoExposure", &has))
                        {
                            if (has) v->override.autoExposure = ppParams.GetAutoExposure();
                            else     v->override.autoExposure.reset();
                        }
                        if (v->override.autoExposure)
                        {
                            auto& ae = *v->override.autoExposure;
                            ImGui::Indent();
                            ImGui::Checkbox("Enabled##vol_ae", &ae.enabled);
                            ImGui::SliderFloat("Manual Exposure##vol_ae",
                                &ae.manualExposure, 0.05f, 16.0f, "%.3f",
                                ImGuiSliderFlags_Logarithmic);
                            ImGui::SliderFloat("EV Bias##vol_ae",
                                &ae.evBias, -3.0f, 3.0f, "%.2f EV");
                            ImGui::SliderFloat("Key##vol_ae",
                                &ae.keyValue, 0.05f, 0.5f, "%.3f");
                            ImGui::Unindent();
                        }
                    }

                    // --- Bloom (reserved; no fields) ---
                    {
                        bool has = v->override.bloom.has_value();
                        if (ImGui::Checkbox("Override Bloom", &has))
                        {
                            if (has) v->override.bloom = ppParams.GetBloom();
                            else     v->override.bloom.reset();
                        }
                        if (v->override.bloom)
                        {
                            ImGui::Indent();
                            ImGui::TextDisabled("(no user-facing params yet)");
                            ImGui::Unindent();
                        }
                    }

                    // --- Tonemapping (bloom strength + color grading) ---
                    {
                        bool has = v->override.tonemapping.has_value();
                        if (ImGui::Checkbox("Override Tonemapping", &has))
                        {
                            if (has) v->override.tonemapping = ppParams.GetTonemapping();
                            else     v->override.tonemapping.reset();
                        }
                        if (v->override.tonemapping)
                        {
                            auto& tm = *v->override.tonemapping;
                            ImGui::Indent();
                            ImGui::SliderFloat("Bloom Strength##vol_tm",
                                &tm.bloomStrength, 0.0f, 1.0f, "%.3f");
                            ImGui::Checkbox("Color Grading##vol_tm",
                                &tm.colorGradingEnabled);
                            if (tm.colorGradingEnabled)
                            {
                                auto& g = tm.grading;
                                ImGui::SliderFloat("Exposure##vol_tmg",
                                    &g.exposure, -5.0f, 5.0f, "%.2f EV");
                                ImGui::SliderFloat("Contrast##vol_tmg",
                                    &g.contrast, 0.2f, 3.0f, "%.2f");
                                ImGui::SliderFloat("Saturation##vol_tmg",
                                    &g.saturation, 0.0f, 2.0f, "%.2f");
                                ImGui::SliderFloat("Temperature##vol_tmg",
                                    &g.temperature, -1.0f, 1.0f, "%.2f");
                                ImGui::SliderFloat("Tint##vol_tmg",
                                    &g.tint, -1.0f, 1.0f, "%.2f");
                            }
                            ImGui::Unindent();
                        }
                    }

                    ImGui::TreePop();
                }
                ImGui::PopID();
            }

            if (pendingDelete != PostProcess::kInvalidVolumeHandle)
                vs->Unregister(pendingDelete);
        }
    }

    // ---- Scripted Overrides — transient, time-driven overrides that auto-expire (damage flash, flashbang, etc.). ----
    ImGui::Separator();
    if (ImGui::CollapsingHeader("Scripted Overrides", 0))
    {
        auto& scripted = ppStack->GetScriptedOverrides();

        if (ImGui::Button("Trigger Damage Flash"))
        {
            // Red tint + vignette ~0.6s; tonemapping override snapshots base + tweaks grading for clean blend.
            PostProcess::ScriptedOverrideDesc desc;
            desc.priority = 2000;
            desc.fadeIn   = 0.05f;
            desc.hold     = 0.1f;
            desc.fadeOut  = 0.45f;
            snprintf(desc.label, sizeof(desc.label), "DamageFlash");

            PostProcess::TonemappingParams tm = ppParams.GetTonemapping();
            tm.colorGradingEnabled     = true;
            tm.grading.temperature     = 1.0f;        // warm/red
            tm.grading.saturation      = 1.6f;        // vivid
            tm.grading.contrast        = 1.3f;
            tm.grading.vignetteStrength = 0.6f;        // edge darkening
            desc.override.tonemapping = tm;

            scripted.Push(desc);
        }
        ImGui::SameLine();
        if (ImGui::Button("Trigger Cool Pulse"))
        {
            // Longer, subtler — demos hold phase + stacked priorities.
            PostProcess::ScriptedOverrideDesc desc;
            desc.priority = 1500;
            desc.fadeIn   = 0.3f;
            desc.hold     = 1.5f;
            desc.fadeOut  = 0.8f;
            snprintf(desc.label, sizeof(desc.label), "CoolPulse");

            PostProcess::TonemappingParams tm = ppParams.GetTonemapping();
            tm.colorGradingEnabled = true;
            tm.grading.temperature = -0.7f;  // cool/blue
            tm.grading.saturation  = 0.6f;   // slightly desaturated
            desc.override.tonemapping = tm;

            scripted.Push(desc);
        }
        ImGui::SameLine();
        if (ImGui::Button("Trigger Glass Shatter"))
        {
            // GlassShatterPass: direct trigger (outside PP stack); pass auto-deactivates after duration.
            if (m_renderer) m_renderer->TriggerGlassShatter(0.5f, 0.5f);
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%zu active", scripted.ActiveCount());

        // Live entries — lets user see timings and pop early.
        auto live = scripted.GetLiveEntries();
        if (!live.empty())
        {
            ImGui::Indent();
            for (const auto& e : live)
            {
                const float frac = (e.totalDuration > 0.0f)
                    ? e.elapsedSeconds / e.totalDuration : 0.0f;
                char line[128];
                snprintf(line, sizeof(line),
                    "#%u %s  t=%.2f/%.2fs  p=%d",
                    e.handle, (e.label[0] ? e.label : "(unnamed)"),
                    e.elapsedSeconds, e.totalDuration, e.priority);
                ImGui::ProgressBar(frac, ImVec2(-1, 0), line);
                ImGui::PushID(static_cast<int>(e.handle));
                if (ImGui::SmallButton("Pop##scr"))
                    scripted.Pop(e.handle);
                ImGui::PopID();
            }
            ImGui::Unindent();
        }
    }

    ImGui::End();
}

// Entity-picker comboboxes — shared by inspectors. Handle variant auto-detects stale handles via MakeHandle generation.
namespace
{
    using EntityFilter = std::function<bool(World&, Entity)>;

    // Build the label shown in the combo row. Format: "name (id)".
    inline std::string EntityPickerLabel(const World& world, Entity e)
    {
        return world.GetName(e) + " (" + std::to_string(e) + ")";
    }

    // Raw-Entity variant. Returns true when selection changed this frame.
    bool DrawEntityPicker(const char* label, World* world, Entity* target,
                          const EntityFilter& filter = nullptr)
    {
        if (!world || !target) return false;

        const bool hasValidTarget = (*target != NullEntity) && world->IsAlive(*target);
        const std::string preview = hasValidTarget
            ? EntityPickerLabel(*world, *target)
            : (*target == NullEntity ? std::string("(none)")
                                     : std::string("(dead)"));

        bool changed = false;
        if (ImGui::BeginCombo(label, preview.c_str()))
        {
            if (ImGui::Selectable("(none)", *target == NullEntity))
            {
                if (*target != NullEntity) { *target = NullEntity; changed = true; }
            }

            for (Entity e : world->GetEntities())
            {
                if (!world->IsAlive(e)) continue;
                if (filter && !filter(*world, e)) continue;

                const bool selected = (*target == e);
                const std::string row = EntityPickerLabel(*world, e);
                if (ImGui::Selectable(row.c_str(), selected))
                {
                    if (*target != e) { *target = e; changed = true; }
                }
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }

        if (*target != NullEntity && !world->IsAlive(*target))
            ImGui::TextColored(ImVec4(1.f, 0.4f, 0.4f, 1.f), "  (entity not alive)");

        return changed;
    }

    // EntityHandle variant — pick stamps current generation; IsHandleValid catches recycled slots.
    bool DrawEntityHandlePicker(const char* label, World* world, EntityHandle* handle,
                                const EntityFilter& filter = nullptr)
    {
        if (!world || !handle) return false;

        const bool valid = world->IsHandleValid(*handle);
        const std::string preview = valid
            ? EntityPickerLabel(*world, handle->entity)
            : (handle->entity == NullEntity ? std::string("(none)")
                                            : std::string("(stale)"));

        bool changed = false;
        if (ImGui::BeginCombo(label, preview.c_str()))
        {
            if (ImGui::Selectable("(none)", handle->entity == NullEntity))
            {
                if (handle->entity != NullEntity) { *handle = NullEntityHandle; changed = true; }
            }

            for (Entity e : world->GetEntities())
            {
                if (!world->IsAlive(e)) continue;
                if (filter && !filter(*world, e)) continue;

                const bool selected = (handle->entity == e) && valid;
                const std::string row = EntityPickerLabel(*world, e);
                if (ImGui::Selectable(row.c_str(), selected))
                {
                    EntityHandle h = world->MakeHandle(e);
                    if (*handle != h) { *handle = h; changed = true; }
                }
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }

        if (handle->entity != NullEntity && !valid)
            ImGui::TextColored(ImVec4(1.f, 0.4f, 0.4f, 1.f), "  (stale handle)");

        return changed;
    }

    // Shared TextureMap slot UI (preview + drag-drop + path edit). Returns true if edited (caller SetEditDirty).
    bool DrawTextureSlotWidget(const char* label, MaterialComponent::TextureMap& slot)
    {
        constexpr float kPreviewSize = 64.f;

        ImGui::PushID(&slot);     // address-based unique id

        // UNORM preview handle (sRGB displays correctly in ImGui's linear RTV); fall back to main sampler.
        const uint64_t gpuHandle = slot.previewGpuHandle ? slot.previewGpuHandle
                                                         : slot.gpuHandle;
        if (gpuHandle != 0)
        {
            ImGui::Image(
                ImTextureRef(static_cast<ImTextureID>(gpuHandle)),
                ImVec2(kPreviewSize, kPreviewSize));
        }
        else
        {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.18f, 0.18f, 1.f));
            ImGui::Button("##prev", ImVec2(kPreviewSize, kPreviewSize));
            ImGui::PopStyleColor();
        }

        ImGui::SameLine();

        ImGui::BeginGroup();
        ImGui::TextUnformatted(label);

        char buf[256];
        strncpy_s(buf, sizeof(buf), slot.name.c_str(), _TRUNCATE);
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
        bool changed = false;
        if (ImGui::InputText("##path", buf, sizeof(buf)))
        {
            slot.name = buf;
            slot.bindlessIndex = -1;
            slot.gpuHandle     = 0;
            changed = true;
        }
        ImGui::TextDisabled("UV%u  |  %s", slot.uvset,
            slot.bindlessIndex >= 0 ? "bound"
            : slot.name.empty()     ? "empty"
                                    : "loading");
        ImGui::EndGroup();

        if (ImGui::BeginDragDropTarget())
        {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ITEX_PATH"))
            {
                slot.name = static_cast<const char*>(payload->Data);
                slot.bindlessIndex = -1;
                slot.gpuHandle     = 0;
                changed = true;
            }
            ImGui::EndDragDropTarget();
        }

        ImGui::PopID();
        return changed;
    }
} // namespace

// ---------------------------------------------------------------------------
// DrawScriptVarWidgets — runtime-generated inspector for ONE script slot's
// editor-exposed variables. The schema (types/defaults/ranges) comes from the
// slot's `exposed` table via ScriptSystem; the per-entity values live on that
// slot's `vars` override map and serialize with the scene. Edits in Play mode
// are pushed onto the live Lua instance immediately.
// ---------------------------------------------------------------------------
void EditorLayer::DrawScriptVarWidgets(ScriptComponent& sc, std::size_t slot, World* /*world*/, Entity e)
{
    if (!m_scriptSys || slot >= sc.scripts.size()) return;
    ScriptInstance& si = sc.scripts[slot];
    auto& vars = si.vars;
    if (si.scriptPath.empty()) return;

    const std::vector<ScriptVarDesc>& schema = m_scriptSys->GetExposedSchema(si.scriptPath);

    ImGui::Separator();
    if (schema.empty())
    {
        ImGui::TextDisabled("No exposed variables");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Declare a `T.exposed = { ... }` table in the script to surface tunable values here.");
        return;
    }
    ImGui::TextDisabled("Exposed Variables");

    // Map an asset extension to the drag-drop payload the Resource panel emits.
    auto payloadForExt = [](const std::string& ext) -> const char* {
        if (ext == ".itex" || ext == ".dds" || ext == ".png" || ext == ".tga") return "ITEX_PATH";
        if (ext == ".imat")                       return "IMAT_PATH";
        if (ext == ".ianim")                      return "IANIM_PATH";
        if (ext == ".iskel")                      return "ISKEL_PATH";
        if (ext == ".iscn")                       return "ISCN_PATH";
        if (ext == ".ipfb" || ext == ".prefab")   return "IPFB_PATH";
        if (ext == ".lua")                        return "ILUA_PATH";
        if (ext == ".hlsl" || ext == ".ihlsl")    return "IHLSL_PATH";
        return nullptr;
    };

    const bool playing = (m_playState != ViewportPlayState::Stopped);
    const float btnW   = ImGui::GetFrameHeight();
    bool anyChanged    = false;

    for (const ScriptVarDesc& d : schema)
    {
        ImGui::PushID(d.name.c_str());

        const bool        overridden = (vars.find(d.name) != vars.end());
        const ScriptVarValue cur     = ResolveScriptVar(d, vars);
        const char* label = d.label.empty() ? d.name.c_str() : d.label.c_str();

        ScriptVarValue next    = cur;
        bool           changed = false;

        // Reserve room for the trailing reset button (checkbox ignores width).
        if (d.type != ScriptVarType::Bool)
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - btnW - ImGui::GetStyle().ItemSpacing.x);

        switch (d.type)
        {
        case ScriptVarType::Float:
        {
            float v = cur.data.f;
            if (d.hasMin && d.hasMax)
                changed = ImGui::SliderFloat(label, &v, d.minVal, d.maxVal);
            else
                changed = ImGui::DragFloat(label, &v, d.speed > 0.f ? d.speed : 0.01f,
                                           d.hasMin ? d.minVal : 0.f, d.hasMax ? d.maxVal : 0.f);
            if (changed) next = ScriptVarValue::MakeFloat(v);
        } break;
        case ScriptVarType::Int:
        {
            int v = cur.data.i;
            if (d.hasMin && d.hasMax)
                changed = ImGui::SliderInt(label, &v, (int)d.minVal, (int)d.maxVal);
            else
                changed = ImGui::DragInt(label, &v, d.speed > 0.f ? d.speed : 1.f,
                                         d.hasMin ? (int)d.minVal : 0, d.hasMax ? (int)d.maxVal : 0);
            if (changed) next = ScriptVarValue::MakeInt(v);
        } break;
        case ScriptVarType::Bool:
        {
            bool v = cur.data.b;
            changed = ImGui::Checkbox(label, &v);
            if (changed) next = ScriptVarValue::MakeBool(v);
        } break;
        case ScriptVarType::Float3:
        {
            float v[3] = { cur.data.v3[0], cur.data.v3[1], cur.data.v3[2] };
            changed = ImGui::DragFloat3(label, v, d.speed > 0.f ? d.speed : 0.01f,
                                        d.hasMin ? d.minVal : 0.f, d.hasMax ? d.maxVal : 0.f);
            if (changed) next = ScriptVarValue::MakeFloat3(v[0], v[1], v[2]);
        } break;
        case ScriptVarType::Color:
        {
            float v[3] = { cur.data.v3[0], cur.data.v3[1], cur.data.v3[2] };
            ImGuiColorEditFlags flags = ImGuiColorEditFlags_Float;
            if (d.hdr) flags |= ImGuiColorEditFlags_HDR;
            changed = ImGui::ColorEdit3(label, v, flags);
            if (changed) next = ScriptVarValue::MakeColor(v[0], v[1], v[2]);
        } break;
        case ScriptVarType::String:
        {
            char buf[512];
            strncpy_s(buf, cur.str.c_str(), _TRUNCATE);
            if (ImGui::InputText(label, buf, sizeof(buf)))
            {
                next = ScriptVarValue::MakeString(buf);
                changed = true;
            }
        } break;
        case ScriptVarType::Asset:
        {
            char buf[512];
            strncpy_s(buf, cur.str.c_str(), _TRUNCATE);
            if (ImGui::InputText(label, buf, sizeof(buf)))
            {
                next = ScriptVarValue::MakeAsset(buf);
                changed = true;
            }
            if (const char* payload = payloadForExt(d.assetExt))
            {
                if (ImGui::BeginDragDropTarget())
                {
                    if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(payload))
                    {
                        const char* dropped = static_cast<const char*>(p->Data);
                        if (dropped && *dropped) { next = ScriptVarValue::MakeAsset(dropped); changed = true; }
                    }
                    ImGui::EndDragDropTarget();
                }
            }
        } break;
        case ScriptVarType::Entity:
        {
            int id = (int)cur.data.entity;
            if (ImGui::DragInt(label, &id, 0.2f, 0, 0))
            {
                next = ScriptVarValue::MakeEntity((uint32_t)(id < 0 ? 0 : id));
                changed = true;
            }
            if (ImGui::BeginDragDropTarget())
            {
                if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("ENTITY"))
                {
                    const Entity de = *static_cast<const Entity*>(p->Data);
                    next = ScriptVarValue::MakeEntity((uint32_t)de);
                    changed = true;
                }
                ImGui::EndDragDropTarget();
            }
        } break;
        }

        if (!d.tooltip.empty() && ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", d.tooltip.c_str());

        // Trailing reset-to-default button (enabled only when this entity has
        // an override stored for the variable).
        ImGui::SameLine();
        ImGui::BeginDisabled(!overridden);
        const bool resetClicked = ImGui::Button("R", ImVec2(btnW, 0));
        ImGui::EndDisabled();
        if (overridden && ImGui::IsItemHovered())
            ImGui::SetTooltip("Reset '%s' to default", d.name.c_str());

        ImGui::PopID();

        if (changed)            // widget edit → store / refresh the per-entity override
        {
            vars[d.name] = next;
            anyChanged = true;
        }
        else if (resetClicked)  // drop the override → falls back to schema default
        {
            vars.erase(d.name);
            anyChanged = true;
        }
    }

    if (anyChanged && playing)
        m_scriptSys->ApplyExposedVars(e, slot, vars);
}

// ---------------------------------------------------------------------------
// DrawScriptComponentSlots — full inspector for a ScriptComponent's list of
// attached scripts. One collapsible section per slot (enabled toggle, .lua
// path with drag-drop, and that slot's exposed-variable widgets), plus Remove
// per slot and a trailing "+ Add Script". Mirrors the SocketComponent dynamic-
// list pattern. Slot teardown/spawn on add/remove is handled by ScriptSystem's
// reconcile loop next frame (it diffs scripts.size() against its slot vector).
// ---------------------------------------------------------------------------
void EditorLayer::DrawScriptComponentSlots(ScriptComponent& sc, World* world, Entity e)
{
    int removeIdx = -1;

    for (std::size_t i = 0; i < sc.scripts.size(); ++i)
    {
        ImGui::PushID(static_cast<int>(i));
        ScriptInstance& si = sc.scripts[i];

        // Header shows the index + file stem so reordering stays legible.
        std::string stem = si.scriptPath;
        if (auto p = stem.find_last_of("/\\"); p != std::string::npos)
            stem = stem.substr(p + 1);
        char hdr[192];
        snprintf(hdr, sizeof(hdr), "%zu: %s%s###scriptslot%zu",
                 i, stem.empty() ? "(no script)" : stem.c_str(),
                 si.enabled ? "" : " (disabled)", i);

        // Distinct per-slot header tint so multiple scripts are easy to tell
        // apart at a glance. Hue is spread by the golden ratio (0.618) for good
        // separation between adjacent slots; disabled slots are desaturated/dim.
        float hue = 0.08f + 0.618f * static_cast<float>(i);
        hue -= static_cast<float>(static_cast<int>(hue));           // fractional part → [0,1)
        const float sat = si.enabled ? 0.50f : 0.12f;
        const float val = si.enabled ? 0.42f : 0.26f;
        ImVec4 cBase, cHover, cActive;
        ImGui::ColorConvertHSVtoRGB(hue, sat,         val,         cBase.x,   cBase.y,   cBase.z);   cBase.w   = 1.0f;
        ImGui::ColorConvertHSVtoRGB(hue, sat,         val + 0.13f, cHover.x,  cHover.y,  cHover.z);  cHover.w  = 1.0f;
        ImGui::ColorConvertHSVtoRGB(hue, sat + 0.10f, val + 0.22f, cActive.x, cActive.y, cActive.z); cActive.w = 1.0f;
        ImGui::PushStyleColor(ImGuiCol_Header,        cBase);
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, cHover);
        ImGui::PushStyleColor(ImGuiCol_HeaderActive,  cActive);
        const bool open = ImGui::CollapsingHeader(hdr, ImGuiTreeNodeFlags_DefaultOpen);
        ImGui::PopStyleColor(3);   // tint only the header bar, not the body

        if (open)
        {
            ImGui::Checkbox("Enabled", &si.enabled);

            // .lua path: free text + ILUA_PATH drag-drop from the Resource panel.
            char buf[512];
            strncpy_s(buf, si.scriptPath.c_str(), _TRUNCATE);
            if (ImGui::InputText("Script Path", buf, sizeof(buf)))
                si.scriptPath = buf;
            if (ImGui::BeginDragDropTarget())
            {
                if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("ILUA_PATH"))
                {
                    const char* dropped = static_cast<const char*>(p->Data);
                    if (dropped && *dropped) si.scriptPath = dropped;
                }
                ImGui::EndDragDropTarget();
            }
            ImGui::TextDisabled("Drop a .lua file from the Resource Panel");

            // This slot's editor-exposed variables.
            DrawScriptVarWidgets(sc, i, world, e);

            if (ImGui::Button("Remove Script"))
                removeIdx = static_cast<int>(i);
        }
        ImGui::PopID();
    }

    if (removeIdx >= 0)
        sc.scripts.erase(sc.scripts.begin() + removeIdx);

    ImGui::Separator();
    if (ImGui::Button("+ Add Script"))
        sc.scripts.emplace_back();
}

// ---- RegisterDefaultEditors — all built-in component editor + tag registrations. Call once after SetRenderer(). ----
void EditorLayer::RegisterDefaultEditors()
{
    Renderer* renderer = m_renderer; // captured by lambdas below

    // Pure descriptor — FOV via angle conversion, near/far planes.
    RegisterReflectedComponent<CameraComponent>("Camera", /*priority*/ 5);

    // FPS controller state — yaw/pitch sliders + mouse/move tuning. The
    // camera pose itself is edited through the Local Transform component.
    // followTarget is an AttachmentRef — picker rendered here in postDraw
    // (REFLECT_BEGIN intentionally omits it; see ComponentReflection.h).
    RegisterReflectedComponent<CameraControllerComponent>(
        "Camera Controller", /*priority*/ 6,
        [](CameraControllerComponent& c, World* w, Entity e)
    {
        if (!w) return;
        if (c.mode != CameraControllerComponent::Mode::Free)
            Editor::DrawAttachmentRef("Follow Target", *w, e, c.followTarget);
    });

    // ---- Camera stack pipeline (DesignMd/camera_stack_system.md) ---------
    RegisterReflectedComponent<VirtualCameraComponent>("Virtual Camera (VCam)", /*priority*/ 7);
    RegisterReflectedComponent<CameraPoseComponent>   ("Camera Pose",           /*priority*/ 8);
    RegisterReflectedComponent<VCamPriorityComponent> ("VCam Priority",         /*priority*/ 9);
    RegisterReflectedComponent<VCamBlendComponent>    ("VCam Blend",            /*priority*/ 10);
    RegisterReflectedComponent<FollowCameraComponent>(
        "Follow Camera", /*priority*/ 11,
        [](FollowCameraComponent& fc, World* w, Entity e)
    {
        if (w) Editor::DrawAttachmentRef("Target", *w, e, fc.target);
    });
    RegisterReflectedComponent<AimCameraComponent>(
        "Aim Camera", /*priority*/ 12,
        [](AimCameraComponent& ac, World* w, Entity e)
    {
        if (w) Editor::DrawAttachmentRef("Target", *w, e, ac.target);
    });
    RegisterReflectedComponent<CameraShakeComponent>  ("Camera Shake",          /*priority*/ 13);
    RegisterReflectedComponent<LiveCameraComponent>   ("Live Camera (read-only)", /*priority*/ 14);

    // ---- Identity (DesignMd/entity_persistence_architecture.md) ---------
    // GuidComponent is intentionally NOT registered in the Inspector — it
    // has no user-editable fields (regenerating breaks every existing ref)
    // and is surfaced via the [guid:xxxxxxxx] badge in the Inspector
    // header instead. BindAndStamp auto-adds the component whenever an
    // AttachmentRef is dropped onto, so users never need to add it
    // manually either.
    RegisterComponentTag<PersistentTag>("Persistent (save-game)");

    // LocalTransform — descriptor with custom Euler-degree widget for rotation.
    RegisterReflectedComponent<LocalTransform>("Local Transform", /*priority*/ 0);

    // RigidBody — descriptor for motion/mass/damping; postDraw shows "needs Collider" + BodyID.
    RegisterReflectedComponent<RigidBodyComponent>("Rigid Body",
        /*priority*/ 20,
        [](RigidBodyComponent& rb, World* world, Entity e)
        {
            const bool created     = rb.bodyId != kInvalidPhysicsBodyId;
            const bool hasCollider = world && world->HasComponent<ColliderComponent>(e);
            ImGui::Separator();
            if (!created)
            {
                if (!hasCollider)
                {
                    ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.2f, 1.0f),
                        "Needs a Collider component to activate physics.");
                    if (ImGui::Button("Add Default Box Collider"))
                        world->AddComponent<ColliderComponent>(e, ColliderComponent{});
                }
                else
                {
                    ImGui::TextDisabled("Body will be created on next physics tick.");
                }
            }
            else
            {
                ImGui::TextDisabled("BodyID: 0x%08X (live)", rb.bodyId);
                ImGui::TextDisabled("Remove + re-add component to apply creation-time changes.");
            }
        });

    // Collider — shape-conditional visibility for halfExtents/radius/halfHeight.
    RegisterReflectedComponent<ColliderComponent>("Collider", /*priority*/ 21);
    // AI strategic / tactical / sensing components grouped at the old
    // NavAgent slot (22). Order matters for the inspector list — AIIntent
    // (strategic, top) → NavAgent (tactical, mid) → Perception (sensing).
    RegisterReflectedComponent<AIIntentComponent> ("AI Intent",   /*priority*/ 22);
    RegisterReflectedComponent<NavAgentComponent> ("NavAgent",    /*priority*/ 22);
    RegisterReflectedComponent<PerceptionComponent>("Perception", /*priority*/ 22);

    // CharacterControllerComponent — KCC tuning. postDraw shows live ground
    // state + velocity so authors can verify capsule sits on the floor while
    // tweaking radius/halfHeight from the Inspector.
    RegisterReflectedComponent<CharacterControllerComponent>("Character Controller",
        /*priority*/ 23,
        [](CharacterControllerComponent& cc, World* /*world*/, Entity /*e*/)
        {
            ImGui::Separator();
            ImGui::TextDisabled("Runtime state");
            ImGui::Text("Grounded: %s%s", cc.isGrounded ? "yes" : "no",
                        (cc.isGrounded != cc.wasGrounded) ? " (changed)" : "");
            const float horizSq = cc.velocity.x * cc.velocity.x
                                + cc.velocity.z * cc.velocity.z;
            ImGui::Text("Velocity: %.2f, %.2f, %.2f  (|h|=%.2f)",
                        cc.velocity.x, cc.velocity.y, cc.velocity.z,
                        sqrtf(horizSq));
            ImGui::Text("Time In Air: %.2fs", cc.timeInAir);
            if (cc.groundEntity != NullEntity)
                ImGui::Text("Ground Entity: %u  (normal %.2f, %.2f, %.2f)",
                            cc.groundEntity, cc.groundNormal.x, cc.groundNormal.y, cc.groundNormal.z);
            else
                ImGui::TextDisabled("Airborne — no ground body");
        });

    // PlayerComponent — gameplay-side tuning.
    RegisterReflectedComponent<PlayerComponent>(
        "Player", /*priority*/ 24,
        [](PlayerComponent& pc, World* w, Entity e)
    {
        if (w) Editor::DrawAttachmentRef("Camera Entity (empty = main)",
                                         *w, e, pc.cameraEntity);
    });

    // MeshHandle — primitive picker driven by descriptor enum.
    RegisterReflectedComponent<MeshHandle>("Mesh");

    // VideoComponent — descriptor handles every field; postDraw appends the
    // play / pause / stop / restart buttons + seek slider + read-only
    // debug stats (clock, decoded PTS, frames).
    RegisterReflectedComponent<VideoComponent>("Video",
        /*priority*/ 50,
        [](VideoComponent& vc, World*, Entity)
        {
            ImGui::Separator();
            ImGui::Text("Transport");
            if (ImGui::Button("Play"))    Video::Play(vc);     ImGui::SameLine();
            if (ImGui::Button("Pause"))   Video::Pause(vc);    ImGui::SameLine();
            if (ImGui::Button("Stop"))    Video::Stop(vc);     ImGui::SameLine();
            if (ImGui::Button("Restart")) Video::Restart(vc);

            // Seek slider — uses frameSource duration when known, else a
            // generous default of 300 s so the slider still has a useful
            // range for live / unknown-length streams.
            double duration = -1.0;
            if (vc.frameSource)        duration = vc.frameSource->GetDurationSeconds();
            else if (vc.decodedFrameSource)
                                       duration = vc.decodedFrameSource->GetDurationSeconds();
            float maxT = (duration > 0.0) ? (float)duration : 300.0f;
            float clock = (float)vc.clockSeconds;
            if (ImGui::SliderFloat("Seek (s)", &clock, 0.f, maxT, "%.3f s"))
                Video::Seek(vc, (double)clock);

            ImGui::Separator();
            // Active decode path — derived from which source is set.
            const char* pathLabel = "(no source — author-driven dpb)";
            if      (vc.decodedFrameSource) pathLabel = "Decoded source (FFmpeg / Mp4FrameSource)";
            else if (vc.frameSource)        pathLabel = "Bitstream source (engine D3D12 video decoder)";
            ImGui::TextDisabled("Decode path     : %s", pathLabel);
            ImGui::TextDisabled("Clock           : %.3f s", vc.clockSeconds);
            ImGui::TextDisabled("Last decoded PTS: %.3f s", vc.lastDecodedPts);
            ImGui::TextDisabled("Frames decoded  : %llu",
                                (unsigned long long)vc.framesDecoded);
            ImGui::TextDisabled("Current DPB slot: %u / %zu",
                                vc.currentDpbSlot, vc.dpb.size());
            ImGui::TextDisabled("Decoder handle  : %u",
                                vc.decoder.handle_id);
        });

    // VisibilityComponent — flag-bits widget (Visible / Cast Shadow / Render In
    // Main Pass) + viewMask + read-only inheritedHidden. Pinned above Local
    // Transform (priority -10) and starts collapsed because it's rarely edited
    // per-entity once authored.
    RegisterReflectedComponent<VisibilityComponent>("Visibility", /*priority*/ -10);
    SetComponentDefaultCollapsed<VisibilityComponent>(true);

    // UIRootComponent — one entity per HUD/menu; widget tree built in C++/Lua, not ECS-editable.
    RegisterReflectedComponent<UI::UIRootComponent>("UI Root", /*priority*/ 95);
    RegisterReflectedComponent<UI::UIScreenSpaceComponent>("UI Screen-Space", /*priority*/ 96);
    RegisterReflectedComponent<UI::UIImageComponent>("UI Image", /*priority*/ 98);
    RegisterReflectedComponent<UI::UITextComponent>("UI Text",   /*priority*/ 99);
    RegisterReflectedComponent<UI::UIBarComponent>("UI Bar",     /*priority*/ 100);

    // World-space UI — billboarded 3D quads via WorldSpaceUISystem + WorldUIBillboardPass; pair marker + content component.
    RegisterReflectedComponent<UI::WorldSpaceUIComponent>("World-Space UI",     /*priority*/ 101);
    RegisterReflectedComponent<UI::WorldUIBarComponent>  ("World UI Bar",       /*priority*/ 102);
    RegisterReflectedComponent<UI::WorldUITextComponent> ("World UI Text",      /*priority*/ 103);
    RegisterReflectedComponent<UI::WorldUIImageComponent>("World UI Image",     /*priority*/ 104);
    RegisterReflectedComponent<UI::DamageNumberComponent>("Damage Number",      /*priority*/ 105);

    // Skybox / IBL — descriptor handles paths/knobs; runtime GPU handles in postDraw.
    RegisterReflectedComponent<SkyboxComponent>("Skybox / IBL",
        /*priority*/ 100,
        [](SkyboxComponent& sc, World*, Entity)
        {
            ImGui::TextDisabled("(Atmosphere knobs live on AtmosphereComponent.)");
            ImGui::TextDisabled("Irradiance GPU: %llu", sc.irradianceGpuHandle);
            ImGui::TextDisabled("Radiance GPU:   %llu", sc.radianceGpuHandle);
            ImGui::TextDisabled("Skybox GPU:     %llu", sc.skyboxGpuHandle);
        });

    // Atmosphere — all procedural sky knobs (was Post Processing -> Sky / Atmosphere).
    RegisterReflectedComponent<AtmosphereComponent>("Atmosphere",     /*priority*/ 101);
    // Time-of-Day — singleton: TODConfig drives the cycle, TODOutput shows computed state.
    RegisterReflectedComponent<TODConfigComponent>  ("Time-of-Day",   /*priority*/ 102);
    RegisterReflectedComponent<TODOutputComponent>  ("TOD Output",    /*priority*/ 103);
    // Sun / Moon tags — attach to a directional LightData entity to put it under TOD control.
    RegisterReflectedComponent<SunLightTag>         ("Sun Light Tag", /*priority*/ 104);
    RegisterReflectedComponent<MoonLightTag>        ("Moon Light Tag",/*priority*/ 105);
    // Volumetric clouds — author tunables on the Sky entity; TOD drives sun direction/color.
    RegisterReflectedComponent<CloudComponent>      ("Volumetric Clouds", /*priority*/ 106);

    // Terrain — heightmap + 4 PBR layers; path edits trigger TextureSystem rebind in BuildScene_SyncTerrain.
    // Numeric fields feed per-frame TerrainParamsCB so changes show next frame (no reimport).
    RegisterReflectedComponent<TerrainComponent>("Terrain",
        /*priority*/ 100,
        [tx = m_textureSys, gfx = m_gfx](TerrainComponent& tcRef, World*, Entity)
        {
            auto* tc = &tcRef;

            // Per-instance text input buffers — avoid InputText truncation + per-frame strncpy. Keyed by component addr.
            struct PathBufs
            {
                std::array<char, 512>                       heightmap;
                std::array<char, 512>                       splatmap;
                std::array<std::array<std::array<char, 512>, 4>, 4> layers; // [layer][slot]
            };
            static std::unordered_map<void*, PathBufs> s_bufs;
            auto& bufs = s_bufs[tc];

            auto syncBuf = [](std::array<char, 512>& buf, const std::string& src)
            {
                strncpy_s(buf.data(), buf.size(), src.c_str(), _TRUNCATE);
            };

            // Resolve TextureHandle to SRV gpu handle for ImGui::Image; returns 0 when unbound (placeholder drawn).
            auto resolveSrv = [tx, gfx](Resource::TextureHandle h) -> uint64_t
            {
                if (!tx || !gfx) return 0;
                if (h == Resource::kInvalidTextureHandle) return 0;
                if (!tx->IsReady(h)) return 0;
                const RHI::Texture* tex = tx->GetTexture(h);
                if (!tex || !tex->IsValid()) return 0;
                return gfx->GetTextureSRVGpuHandle(*tex);
            };

            // Thumbnail + path-input row (mirrors decal-material inspector for consistent ITEX drop behaviour).
            auto pathRow = [&resolveSrv](const char* label,
                                         std::array<char, 512>& buf,
                                         std::string& targetPath,
                                         Resource::TextureHandle texHandle,
                                         float previewSize) -> bool
            {
                ImGui::PushID(label);
                bool changed = false;

                // Thumbnail.
                const uint64_t gpu = resolveSrv(texHandle);
                if (gpu != 0)
                {
                    ImGui::Image(ImTextureRef(static_cast<ImTextureID>(gpu)),
                                 ImVec2(previewSize, previewSize));
                }
                else
                {
                    ImGui::PushStyleColor(ImGuiCol_Button,
                                          ImVec4(0.18f, 0.18f, 0.18f, 1.f));
                    ImGui::Button("##prev", ImVec2(previewSize, previewSize));
                    ImGui::PopStyleColor();
                }

                // Drag-drop accepts ITEX onto the thumbnail too.
                if (ImGui::BeginDragDropTarget())
                {
                    if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("ITEX_PATH"))
                    {
                        targetPath = static_cast<const char*>(p->Data);
                        strncpy_s(buf.data(), buf.size(), targetPath.c_str(), _TRUNCATE);
                        changed = true;
                    }
                    ImGui::EndDragDropTarget();
                }

                ImGui::SameLine();
                ImGui::BeginGroup();
                ImGui::TextUnformatted(label);

                ImGui::SetNextItemWidth(-1.0f);
                if (ImGui::InputText("##path", buf.data(), buf.size()))
                {
                    targetPath = buf.data();
                    changed = true;
                }
                if (ImGui::BeginDragDropTarget())
                {
                    if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("ITEX_PATH"))
                    {
                        targetPath = static_cast<const char*>(p->Data);
                        strncpy_s(buf.data(), buf.size(), targetPath.c_str(), _TRUNCATE);
                        changed = true;
                    }
                    ImGui::EndDragDropTarget();
                }
                ImGui::EndGroup();

                ImGui::PopID();
                return changed;
            };

            // ---- Heightmap + splatmap ----
            syncBuf(bufs.heightmap, tc->heightmapPath);
            pathRow("Heightmap", bufs.heightmap, tc->heightmapPath,
                    tc->heightmapHandle, 48.0f);

            syncBuf(bufs.splatmap, tc->splatmapPath);
            pathRow("Splatmap (optional)", bufs.splatmap, tc->splatmapPath,
                    tc->splatmapHandle, 48.0f);

            // ---- Tile placement ----
            ImGui::Separator();
            ImGui::TextUnformatted("Placement");
            ImGui::DragFloat3("World Center",
                reinterpret_cast<float*>(&tc->worldCenter), 1.0f);
            ImGui::DragFloat("World Size", &tc->worldSize, 8.0f, 16.0f, 65536.0f, "%.0f m");
            ImGui::DragFloat("Height Scale", &tc->heightScale, 4.0f, 1.0f, 65536.0f, "%.0f m");

            // tilesPerSide as int slider — clamp to sane range; the
            // mesh-shader dispatch count is N² so the upper bound matters.
            int tps = static_cast<int>(tc->tilesPerSide);
            if (ImGui::SliderInt("Tiles Per Side", &tps, 1, 256,
                                 "%d  (~%.1f m / quad)"))
                tc->tilesPerSide = static_cast<uint32_t>(tps);
            ImGui::TextDisabled("≈ %.2f m per quad", tc->worldSize / float(std::max(tps, 1) * 11));

            // ---- Auto-blend section ----
            ImGui::Separator();
            ImGui::TextUnformatted("Auto-Blend (no splatmap)");
            ImGui::TextDisabled("Each layer is gated by a world-meter height range");
            ImGui::TextDisabled("AND a degree slope range — both must hold.");

            // ---- 4 layers ---- baseY/topY hint the slider range; DragFloat doesn't hard-clamp.
            const float baseY = tc->worldCenter.y;
            const float topY  = tc->worldCenter.y + tc->heightScale;

            for (int i = 0; i < 4; ++i)
            {
                auto& l = tc->layers[i];
                ImGui::PushID(i);
                char hdr[32];
                snprintf(hdr, sizeof(hdr), "Layer %d", i);
                if (ImGui::CollapsingHeader(hdr,
                        i == 0 ? ImGuiTreeNodeFlags_DefaultOpen : 0))
                {
                    constexpr float kLayerPreview = 32.0f;
                    syncBuf(bufs.layers[i][0], l.albedoPath);
                    pathRow("Albedo",               bufs.layers[i][0],
                            l.albedoPath, l.albedoHandle, kLayerPreview);
                    syncBuf(bufs.layers[i][1], l.normalPath);
                    pathRow("Normal",               bufs.layers[i][1],
                            l.normalPath, l.normalHandle, kLayerPreview);
                    syncBuf(bufs.layers[i][2], l.armPath);
                    pathRow("ARM (AO/Rough/Metal)", bufs.layers[i][2],
                            l.armPath,    l.armHandle,    kLayerPreview);
                    syncBuf(bufs.layers[i][3], l.dispPath);
                    pathRow("Disp",                 bufs.layers[i][3],
                            l.dispPath,   l.dispHandle,   kLayerPreview);

                    // Tiling density (rep/m): 0.5 ≈ 2m cycle, 0.05 ≈ 20m cycle.
                    ImGui::SliderFloat("Tiling", &l.tilingScale,
                        0.001f, 2.0f, "%.3f rep/m");
                    if (l.tilingScale > 1e-4f)
                        ImGui::TextDisabled("≈ 1 cycle / %.2f m", 1.0f / l.tilingScale);

                    ImGui::Spacing();
                    ImGui::TextDisabled("Tile Y range: %.0f m … %.0f m", baseY, topY);

                    // Height range (world m); DragFloatRange2 enforces min ≤ max.
                    ImGui::DragFloatRange2("Height Range",
                        &l.minHeight, &l.maxHeight,
                        1.0f, -1e6f, 1e6f,
                        "min %.0f m", "max %.0f m");
                    ImGui::SliderFloat("Height Fade", &l.fadeHeight,
                        0.1f, 200.0f, "%.1f m");

                    // Slope range (degrees, 0=flat, 90=cliff).
                    ImGui::DragFloatRange2("Slope Range",
                        &l.minSlopeDeg, &l.maxSlopeDeg,
                        0.5f, 0.0f, 90.0f,
                        "min %.1f\xc2\xb0", "max %.1f\xc2\xb0");
                    ImGui::SliderFloat("Slope Fade", &l.fadeSlopeDeg,
                        0.1f, 45.0f, "%.1f\xc2\xb0");

                    ImGui::TextDisabled("Bindless: albedo=%d nor=%d arm=%d disp=%d",
                        l.albedoBindlessIdx, l.normalBindlessIdx,
                        l.armBindlessIdx, l.dispBindlessIdx);
                }
                ImGui::PopID();
            }
        });

    // LightData descriptor handles type/dir/radius/cone/color; postDraw shows live dir + atlas-slot hint.
    RegisterReflectedComponent<LightData>("Light",
        /*priority*/ 100,
        [](LightData& ld, World*, Entity)
        {
            if (ld.type == LightType::Directional || ld.type == LightType::Spot)
                ImGui::TextDisabled("dir  %.3f  %.3f  %.3f",
                    ld.direction.x, ld.direction.y, ld.direction.z);
            if (ld.type == LightType::Spot && ld.castsShadow)
                ImGui::TextDisabled("(uses 1 of %u atlas slices)",
                                    SpotShadowPass::kMaxCasters);
        });

    // SkeletonComponent — read-only meta + bone hierarchy via custom postDraw.
    RegisterReflectedComponent<SkeletonComponent>("Skeleton",
        /*priority*/ 20,
        [renderer](SkeletonComponent& scRef, World*, Entity)
        {
            auto* sc = &scRef;
            if (sc->assetIndex == kInvalidSkeletonIndex)
            {
                ImGui::TextDisabled("(no skeleton asset assigned)");
                return;
            }
            ImGui::TextDisabled("Asset index : %u", sc->assetIndex);
            ImGui::TextDisabled("Bone count  : %u", sc->boneCount);

            if (!renderer) return;
            const SkeletonRegistry& reg = renderer->GetSkeletonRegistry();
            if (sc->assetIndex >= reg.Count()) return;
            const SkeletonAsset& skel = reg.Get(sc->assetIndex);

            if (ImGui::TreeNode("Bone Hierarchy"))
            {
                std::vector<std::vector<uint32_t>> children(skel.boneCount);
                std::vector<uint32_t> roots;
                for (uint32_t i = 0; i < skel.boneCount; ++i)
                {
                    if (skel.parentIndex[i] < 0) roots.push_back(i);
                    else children[static_cast<uint32_t>(skel.parentIndex[i])].push_back(i);
                }

                std::function<void(uint32_t)> drawBone = [&](uint32_t b) {
                    ImGuiTreeNodeFlags f = ImGuiTreeNodeFlags_SpanAvailWidth;
                    if (children[b].empty()) f |= ImGuiTreeNodeFlags_Leaf;
                    const char* nm = skel.boneNames[b][0] ? skel.boneNames[b] : "(unnamed)";
                    ImGui::PushID(static_cast<int>(b));
                    if (ImGui::TreeNodeEx(nm, f))
                    {
                        for (uint32_t ch : children[b]) drawBone(ch);
                        ImGui::TreePop();
                    }
                    ImGui::PopID();
                };
                for (uint32_t r : roots) drawBone(r);
                ImGui::TreePop();
            }

            if (!skel.sockets.empty() && ImGui::TreeNode("Sockets"))
            {
                for (const auto& s : skel.sockets)
                    ImGui::TextDisabled("[bone %u] %s", s.boneIndex, s.name[0] ? s.name : "(unnamed)");
                ImGui::TreePop();
            }
        });

    // FootIKComponent — opt-in foot ground snap for PMX-rigged characters.
    // Auto-detects left/right foot IK chains on first tick; designers tweak
    // raycast range + offset from here. Priority adjacent to Skeleton.
    RegisterReflectedComponent<FootIKComponent>("Foot IK", /*priority*/ 31);

    // AnimationComponent — descriptor for paused/looping/speed; clip combos + scrub sliders need ClipLibrary (postDraw).
    RegisterReflectedComponent<AnimationComponent>("Animation",
        /*priority*/ 30,
        [renderer](AnimationComponent& anim, World* world, Entity entity)
        {
            if (!renderer) return;
            const ClipLibrary& clipLib  = renderer->GetClipLibrary();
            const uint32_t     clipCount = clipLib.Count();
            if (clipCount == 0)
            {
                ImGui::TextDisabled("No clips loaded. Drag a .ianim from the Resource panel.");
                return;
            }
            uint32_t boneFilter = 0;
            if (world)
            {
                const SkeletonComponent* sc = world->GetComponent<SkeletonComponent>(entity);
                if (sc && sc->assetIndex != kInvalidSkeletonIndex)
                    boneFilter = sc->boneCount;
            }
            auto clipLabel = [&](uint32_t idx) -> std::string {
                if (idx == kInvalidClipIndex) return "(none)";
                if (idx >= clipCount)         return "(invalid)";
                const ClipAsset& c = clipLib.Get(idx);
                char buf[64];
                snprintf(buf, sizeof(buf), "Clip #%u  |  %.2f s  |  %u bones",
                         idx, c.duration, c.boneCount);
                return buf;
            };
            auto clipCombo = [&](const char* tag, uint32_t& clipIdx, float& timeFld)
            {
                ImGui::Separator();
                ImGui::TextDisabled("%s", tag);
                ImGui::PushID(tag);
                const std::string cur = clipLabel(clipIdx);
                if (ImGui::BeginCombo("##c", cur.c_str()))
                {
                    if (ImGui::Selectable("(none)", clipIdx == kInvalidClipIndex))
                        clipIdx = kInvalidClipIndex;
                    for (uint32_t i = 0; i < clipCount; ++i)
                    {
                        const ClipAsset& c = clipLib.Get(i);
                        if (boneFilter && c.boneCount != boneFilter) continue;
                        const bool sel = (clipIdx == i);
                        if (ImGui::Selectable(clipLabel(i).c_str(), sel))
                        { clipIdx = i; timeFld = 0.f; }
                        if (sel) ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
                ImGui::PopID();
                if (clipIdx != kInvalidClipIndex && clipIdx < clipCount)
                {
                    const float dur = clipLib.Get(clipIdx).duration;
                    char id[16]; snprintf(id, sizeof(id), "Time##%s", tag);
                    ImGui::SliderFloat(id, &timeFld, 0.f,
                                       dur > 0.f ? dur : 1.f, "%.3f s");
                }
            };
            clipCombo("Primary Clip",          anim.primaryClip,   anim.primaryTime);
            clipCombo("Cross-fade (Secondary)", anim.secondaryClip, anim.secondaryTime);
            if (anim.secondaryClip != kInvalidClipIndex)
                ImGui::SliderFloat("Blend##blend", &anim.blendWeight, 0.f, 1.f,
                                   "%.2f  (0=primary, 1=secondary)");
        },
        /*requires*/ [](World& w, Entity e) -> bool {
            return w.HasComponent<SkeletonComponent>(e);
        });

    // MorphComponent — descriptor for paused/looping; clip combo + weight inspector in postDraw.
    // Manual mode (no clip bound) lets the user drag mesh-target weights directly to
    // author multi-morph blends without needing an animation.
    RegisterReflectedComponent<MorphComponent>("Morph",
        /*priority*/ 35,
        [renderer](MorphComponent& morph, World* world, Entity entity)
        {
            if (!renderer || !world) return;
            const MorphClipLibrary& morphLib  = renderer->GetMorphClipLibrary();
            const uint32_t          clipCount = morphLib.Count();

            // Resolve the skeleton asset for this entity (MorphComponent lives on
            // the skeleton root, which also carries SkeletonComponent).
            const SkeletonAsset* skelAsset = nullptr;
            {
                auto* sc = world->GetComponent<SkeletonComponent>(entity);
                if (sc && sc->assetIndex < renderer->GetSkeletonRegistry().Count())
                    skelAsset = &renderer->GetSkeletonRegistry().Get(sc->assetIndex);
            }

            auto clipLabel = [&](uint32_t idx) -> std::string {
                if (idx == kInvalidClipIndex) return "(none - manual)";
                if (idx >= clipCount)         return "(invalid)";
                const MorphClipAsset& c = morphLib.Get(idx);
                char buf[64];
                snprintf(buf, sizeof(buf), "MorphClip #%u  |  %.2f s  |  %u channels",
                         idx, c.duration, c.morphCount);
                return buf;
            };
            ImGui::TextDisabled("Morph Clip");
            {
                const std::string cur = clipLabel(morph.primaryMorphClip);
                if (ImGui::BeginCombo("##morph_clip", cur.c_str()))
                {
                    if (ImGui::Selectable("(none - manual)",
                                          morph.primaryMorphClip == kInvalidClipIndex))
                    {
                        morph.primaryMorphClip = kInvalidClipIndex;
                        // Re-key weights to mesh-target indexing for manual mode;
                        // clip-indexed values left over from a previous clip would
                        // land on the wrong morph targets, so reset them.
                        if (skelAsset)
                        {
                            morph.count = std::min<uint32_t>(skelAsset->morphTargetCount,
                                                              MorphComponent::MAX);
                            for (uint32_t i = 0; i < morph.count; ++i)
                                morph.weights[i] = 0.f;
                        }
                    }
                    for (uint32_t i = 0; i < clipCount; ++i)
                    {
                        const bool sel = (morph.primaryMorphClip == i);
                        if (ImGui::Selectable(clipLabel(i).c_str(), sel))
                        { morph.primaryMorphClip = i; morph.time = 0.f; }
                        if (sel) ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
            }

            const bool hasClip = (morph.primaryMorphClip != kInvalidClipIndex
                               && morph.primaryMorphClip < clipCount);
            const bool hasAnim = world->HasComponent<AnimationComponent>(entity);

            if (hasClip && !hasAnim)
            {
                const float dur = morphLib.Get(morph.primaryMorphClip).duration;
                ImGui::SliderFloat("Time##mt", &morph.time, 0.f,
                                   dur > 0.f ? dur : 1.f, "%.3f s");
            }
            else if (hasClip && hasAnim)
            {
                ImGui::TextDisabled("Time synced to AnimationComponent");
            }

            ImGui::Separator();

            // ---- Manual mode: drag any mesh morph target ----------------------
            if (!hasClip)
            {
                if (!skelAsset || skelAsset->morphTargetCount == 0)
                {
                    ImGui::TextDisabled("(skeleton has no morph targets)");
                    return;
                }
                const uint32_t n = std::min<uint32_t>(skelAsset->morphTargetCount,
                                                      MorphComponent::MAX);
                // Ensure count is sized so the skinning subsystem picks weights up.
                if (morph.count < n) morph.count = n;

                ImGui::TextDisabled("Mesh Morph Targets (%u) - drag to blend:", n);
                ImGui::SameLine();
                if (ImGui::SmallButton("Reset All##morph_reset"))
                {
                    for (uint32_t i = 0; i < n; ++i) morph.weights[i] = 0.f;
                }

                static ImGuiTextFilter morphFilter;
                morphFilter.Draw("Filter##morph_filter", 180.f);

                for (uint32_t i = 0; i < n; ++i)
                {
                    const char* nm = skelAsset->morphTargetNames[i][0]
                                     ? skelAsset->morphTargetNames[i] : "(unnamed)";
                    if (!morphFilter.PassFilter(nm)) continue;

                    char lbl[96];
                    snprintf(lbl, sizeof(lbl), "%s##mm%u", nm, i);
                    ImGui::SliderFloat(lbl, &morph.weights[i], 0.f, 1.f);
                }
                return;
            }

            // ---- Clip mode: show sampled weights (editable only when paused) --
            if (morph.count == 0)
            {
                ImGui::TextDisabled("(weights not yet sampled - play to see values)");
                return;
            }
            ImGui::TextDisabled("Morph Weights (%u channels):", morph.count);
            const MorphClipAsset& asset  = morphLib.Get(morph.primaryMorphClip);
            const bool            canEdit = morph.paused && !hasAnim;
            for (uint32_t i = 0; i < morph.count; ++i)
            {
                const char* nm = (i < asset.morphCount && asset.morphNames[i][0])
                                 ? asset.morphNames[i] : "(unnamed)";
                if (canEdit)
                {
                    char lbl[96];
                    snprintf(lbl, sizeof(lbl), "[%u] %s##cw%u", i, nm, i);
                    ImGui::SliderFloat(lbl, &morph.weights[i], 0.f, 1.f);
                }
                else
                {
                    char ov[96];
                    snprintf(ov, sizeof(ov), "[%u] %s  %.3f", i, nm, morph.weights[i]);
                    ImGui::ProgressBar(morph.weights[i], ImVec2(-1.f, 0.f), ov);
                }
            }
            if (!canEdit)
                ImGui::TextDisabled("(pause + no AnimComp to edit, or switch clip to (none) for manual)");
        },
        /*requires*/ [](World& w, Entity e) -> bool {
            return w.HasComponent<SkeletonComponent>(e);
        });

    // ChainPhysicsComponent — REFLECT_COLLAPSE for Hair/Skirt/Spring Root/Spring Child sub-sections.
    RegisterReflectedComponent<ChainPhysicsComponent>("Chain Physics",
        /*priority*/ 40, /*postDraw*/ nullptr,
        /*requires*/ [](World& w, Entity e) -> bool {
            return w.HasComponent<SkeletonComponent>(e);
        });

    // SkeletonRef — read-only display only. Empty descriptor + postDraw.
    RegisterReflectedComponent<SkeletonRef>("Skeleton Ref",
        /*priority*/ 70,
        [](SkeletonRef& ref, World* world, Entity)
        {
            if (ref.entity == NullEntity)
            {
                ImGui::TextDisabled("(not linked)");
                return;
            }
            ImGui::TextDisabled("Root entity : %u", ref.entity);
            if (world)
            {
                const std::string& nm = world->GetName(ref.entity);
                ImGui::TextDisabled("Root name   : %s", nm.c_str());
                const bool hasAnim  = world->HasComponent<AnimationComponent>(ref.entity);
                const bool hasSkel  = world->HasComponent<SkeletonComponent>(ref.entity);
                const bool hasMorph = world->HasComponent<MorphComponent>(ref.entity);
                ImGui::TextDisabled("Root has    :%s%s%s",
                    hasSkel  ? " Skeleton"   : "",
                    hasAnim  ? " Animation"  : "",
                    hasMorph ? " Morph"      : "");
            }
            ImGui::TextDisabled("(select root entity to edit animation)");
        });

    // CapsuleColliderComponent — bone combos need Renderer::GetSkeletonRegistry; custom postDraw.
    RegisterReflectedComponent<CapsuleColliderComponent>("Capsule Colliders",
        /*priority*/ 45,
        [renderer](CapsuleColliderComponent& ccRef, World* world, Entity entity)
        {
            auto* cc = &ccRef;

            // Show bone name list from skeleton if available.
            const SkeletonAsset* skel = nullptr;
            if (renderer && world)
            {
                const SkeletonComponent* sc = world->GetComponent<SkeletonComponent>(entity);
                if (sc && sc->assetIndex != kInvalidSkeletonIndex
                    && sc->assetIndex < renderer->GetSkeletonRegistry().Count())
                    skel = &renderer->GetSkeletonRegistry().Get(sc->assetIndex);
            }

            auto boneName = [&](uint32_t idx) -> const char* {
                if (skel && idx < skel->boneCount && skel->boneNames[idx][0])
                    return skel->boneNames[idx];
                return "(?)";
            };

            // Bone combo helper — shows bone name list from skeleton.
            auto boneCombo = [&](const char* label, uint32_t& boneIdx)
            {
                const char* cur = boneName(boneIdx);
                if (ImGui::BeginCombo(label, cur))
                {
                    uint32_t count = skel ? skel->boneCount : 0;
                    for (uint32_t bi = 0; bi < count; ++bi)
                    {
                        const bool sel = (boneIdx == bi);
                        char entry[80];
                        snprintf(entry, sizeof(entry), "[%u] %s", bi, boneName(bi));
                        if (ImGui::Selectable(entry, sel))
                            boneIdx = bi;
                        if (sel) ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
            };

            for (int i = 0; i < cc->count; ++i)
            {
                ImGui::PushID(i);
                auto& cap = cc->capsules[i];

                char label[32]; snprintf(label, sizeof(label), "Capsule %d", i);
                if (ImGui::CollapsingHeader(label, 0))
                {
                    ImGui::Checkbox("Enabled", &cap.enabled);
                    boneCombo("Bone A", cap.boneA);
                    ImGui::DragFloat3("Offset A", &cap.offsetA.x, 0.005f, -2.f, 2.f, "%.3f");
                    boneCombo("Bone B", cap.boneB);
                    ImGui::DragFloat3("Offset B", &cap.offsetB.x, 0.005f, -2.f, 2.f, "%.3f");
                    ImGui::DragFloat("Radius", &cap.radius, 0.005f, 0.01f, 1.0f, "%.3f");

                    if (ImGui::Button("Remove"))
                    {
                        for (int j = i; j < cc->count - 1; ++j)
                            cc->capsules[j] = cc->capsules[j + 1];
                        cc->count--;
                        ImGui::PopID();
                        break;
                    }
                }
                ImGui::PopID();
            }

            if (cc->count < CapsuleColliderComponent::MAX_CAPSULES)
            {
                if (ImGui::Button("+ Add Capsule"))
                {
                    cc->capsules[cc->count] = {};
                    cc->count++;
                }
            }
        },
        /*requires*/ [](World& w, Entity e) -> bool {
            return w.HasComponent<ChainPhysicsComponent>(e);
        });

    // ScriptComponent — a dynamic list of attached Lua scripts. The reflected
    // descriptor is empty (a static field list can't express a vector); the
    // whole inspector is hand-drawn here: per-slot enabled + path (ILUA_PATH
    // drag-drop) + exposed-variable UI, with add/remove controls.
    RegisterReflectedComponent<ScriptComponent>("Script", /*priority*/ 50,
        [this](ScriptComponent& sc, World* w, Entity e) {
            DrawScriptComponentSlots(sc, w, e);
        });

    // SocketComponent — bone combos need live skeleton lookup; full custom postDraw.
    RegisterReflectedComponent<SocketComponent>("Sockets",
        /*priority*/ 48,
        [renderer](SocketComponent& scRef, World* world, Entity entity)
        {
            auto* sc = &scRef;

            const SkeletonAsset* skel = nullptr;
            if (renderer && world)
            {
                const SkeletonComponent* skc = world->GetComponent<SkeletonComponent>(entity);
                if (skc && skc->assetIndex != kInvalidSkeletonIndex
                    && skc->assetIndex < renderer->GetSkeletonRegistry().Count())
                    skel = &renderer->GetSkeletonRegistry().Get(skc->assetIndex);
            }

            if (!skel)
            {
                ImGui::TextDisabled("(entity has no SkeletonComponent — add one first)");
                return;
            }

            auto boneNameAt = [&](uint32_t idx) -> const char* {
                if (idx < skel->boneCount && skel->boneNames[idx][0])
                    return skel->boneNames[idx];
                return "(?)";
            };

            for (uint32_t i = 0; i < sc->count; ++i)
            {
                ImGui::PushID(static_cast<int>(i));
                auto& s = sc->sockets[i];

                // ### (triple hash) — ImGui ignores visible label for ID, so name typing doesn't reset expanded state.
                char hdr[96];
                snprintf(hdr, sizeof(hdr), "%u: %s###sock%u",
                         i, s.name[0] ? s.name : "(unnamed)", i);

                if (ImGui::CollapsingHeader(hdr, ImGuiTreeNodeFlags_DefaultOpen))
                {
                    ImGui::InputText("Name", s.name, sizeof(s.name));

                    const char* curBone = boneNameAt(s.boneIndex);
                    if (ImGui::BeginCombo("Bone", curBone))
                    {
                        for (uint32_t bi = 0; bi < skel->boneCount; ++bi)
                        {
                            const bool sel = (s.boneIndex == bi);
                            char row[96];
                            snprintf(row, sizeof(row), "[%u] %s", bi, boneNameAt(bi));
                            if (ImGui::Selectable(row, sel))
                                s.boneIndex = bi;
                            if (sel) ImGui::SetItemDefaultFocus();
                        }
                        ImGui::EndCombo();
                    }

                    DirectX::XMFLOAT3 offPos{
                        s.localOffset._41, s.localOffset._42, s.localOffset._43 };
                    bool offChanged = false;
                    offChanged |= ImGui::DragFloat3("Offset", &offPos.x, 0.01f, -10.f, 10.f, "%.3f");
                    offChanged |= ImGui::DragFloat3("Rotation (deg)", &s.rotationEulerDeg.x,
                                                    0.5f, -360.f, 360.f, "%.2f");
                    if (offChanged)
                    {
                        using namespace DirectX;
                        const XMVECTOR q = XMQuaternionRotationRollPitchYaw(
                            XMConvertToRadians(s.rotationEulerDeg.x),
                            XMConvertToRadians(s.rotationEulerDeg.y),
                            XMConvertToRadians(s.rotationEulerDeg.z));
                        XMMATRIX M = XMMatrixRotationQuaternion(q);
                        M.r[3] = XMVectorSet(offPos.x, offPos.y, offPos.z, 1.0f);
                        XMStoreFloat4x4(&s.localOffset, M);
                    }

                    if (ImGui::Button("Remove"))
                    {
                        for (uint32_t j = i; j + 1 < sc->count; ++j)
                            sc->sockets[j] = sc->sockets[j + 1];
                        sc->count--;
                        ImGui::PopID();
                        break;
                    }
                }
                ImGui::PopID();
            }

            if (sc->count < SocketComponent::MAX)
            {
                if (ImGui::Button("+ Add Socket"))
                {
                    sc->sockets[sc->count] = SocketComponent::Socket{};
                    sc->count++;
                }
            }
            else
                ImGui::TextDisabled("(max %u sockets)", SocketComponent::MAX);
        },
        /*requires*/ [](World& w, Entity e) -> bool {
            return w.HasComponent<SkeletonComponent>(e);
        });

    // FollowEntity — entity-handle picker; needs World context, custom postDraw.
    RegisterReflectedComponent<FollowEntityComponent>("Follow Entity",
        /*priority*/ 49,
        [](FollowEntityComponent& feRef, World* world, Entity self)
        {
            auto* fe = &feRef;

            // Target must differ from self and carry GlobalTransform.
            DrawEntityHandlePicker("Target", world, &fe->target,
                [self](World& w, Entity e) {
                    return e != self && w.HasComponent<GlobalTransform>(e);
                });

            DirectX::XMFLOAT3 offPos{
                fe->localOffset._41, fe->localOffset._42, fe->localOffset._43 };
            bool offChanged = false;
            offChanged |= ImGui::DragFloat3("Offset", &offPos.x, 0.01f, -100.f, 100.f, "%.3f");
            offChanged |= ImGui::DragFloat3("Rotation (deg)", &fe->rotationEulerDeg.x,
                                            0.5f, -360.f, 360.f, "%.2f");
            if (offChanged)
            {
                using namespace DirectX;
                const XMVECTOR q = XMQuaternionRotationRollPitchYaw(
                    XMConvertToRadians(fe->rotationEulerDeg.x),
                    XMConvertToRadians(fe->rotationEulerDeg.y),
                    XMConvertToRadians(fe->rotationEulerDeg.z));
                XMMATRIX M = XMMatrixRotationQuaternion(q);
                M.r[3] = XMVectorSet(offPos.x, offPos.y, offPos.z, 1.0f);
                XMStoreFloat4x4(&fe->localOffset, M);
            }
        });

    // FollowSocket — entity picker + socket combo, both World-context heavy.
    RegisterReflectedComponent<FollowSocketComponent>("Follow Socket",
        /*priority*/ 50,
        [](FollowSocketComponent& fsRef, World* world, Entity self)
        {
            auto* fs = &fsRef;

            // Target must publish SocketComponent; self filtered (trivial cycle).
            DrawEntityHandlePicker("Target", world, &fs->target,
                [self](World& w, Entity e) {
                    return e != self && w.HasComponent<SocketComponent>(e);
                });

            const SocketComponent* sc = nullptr;
            if (world && world->IsHandleValid(fs->target))
                sc = world->GetComponent<SocketComponent>(fs->target.entity);

            if (!sc || sc->count == 0)
            {
                ImGui::TextDisabled("(target has no sockets — add SocketComponent entries first)");
            }
            else
            {
                auto socketLabel = [sc](uint32_t i) -> const char* {
                    if (i >= sc->count) return "(out of range)";
                    return sc->sockets[i].name[0] ? sc->sockets[i].name : "(unnamed)";
                };

                const char* curName = socketLabel(fs->socketIndex);
                if (ImGui::BeginCombo("Socket", curName))
                {
                    for (uint32_t i = 0; i < sc->count; ++i)
                    {
                        const bool sel = (fs->socketIndex == i);
                        char row[96];
                        snprintf(row, sizeof(row), "%u: %s", i, socketLabel(i));
                        if (ImGui::Selectable(row, sel))
                            fs->socketIndex = i;
                        if (sel) ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
            }

            DirectX::XMFLOAT3 offPos{
                fs->localOffset._41, fs->localOffset._42, fs->localOffset._43 };
            bool offChanged = false;
            offChanged |= ImGui::DragFloat3("Offset", &offPos.x, 0.01f, -100.f, 100.f, "%.3f");
            offChanged |= ImGui::DragFloat3("Rotation (deg)", &fs->rotationEulerDeg.x,
                                            0.5f, -360.f, 360.f, "%.2f");
            if (offChanged)
            {
                using namespace DirectX;
                const XMVECTOR q = XMQuaternionRotationRollPitchYaw(
                    XMConvertToRadians(fs->rotationEulerDeg.x),
                    XMConvertToRadians(fs->rotationEulerDeg.y),
                    XMConvertToRadians(fs->rotationEulerDeg.z));
                XMMATRIX M = XMMatrixRotationQuaternion(q);
                M.r[3] = XMVectorSet(offPos.x, offPos.y, offPos.z, 1.0f);
                XMStoreFloat4x4(&fs->localOffset, M);
            }
        });

    // ---- Tag-only registrations -------
    RegisterComponentTag<GlobalTransform>      ("Global Transform");
    RegisterComponentTag<Parent>               ("Parent");
    RegisterComponentTag<Children>             ("Children");
    RegisterComponentTag<SceneNodeTag>         ("Scene Node");
    RegisterComponentTag<RenderLayer>          ("Render Layer");
    RegisterComponentTag<WorldAabb>            ("World AABB");
    RegisterComponentTag<MeshLibRef>           ("Mesh Ref");
    RegisterComponentTag<MeshSourcePath>       ("Mesh Source Path");
    RegisterComponentTag<MaterialComponent>    ("Material");
    RegisterComponentTag<MeshSkinnedComponent>    ("Skinned Mesh");
    RegisterComponentTag<SkinningOutputComponent> ("Skinning Output");

    // Volumetric Light — opt-in marker for Spot/Point lights to contribute fog froxels; needs LightData.
    RegisterReflectedComponent<VolumetricLightComponent>("Volumetric Light",
        /*priority*/ 60,
        /*postDraw*/ nullptr,
        [](World& w, Entity e) -> bool
        {
            return w.GetComponent<LightData>(e) != nullptr;
        });

    // Reflection Probe — descriptor for extents/realtime/interval; postDraw adds Bake button (needs Renderer access).
    RegisterReflectedComponent<ReflectionProbeComponent>("Reflection Probe",
        /*priority*/ 55,
        [this](ReflectionProbeComponent& probe, World*, Entity)
        {
            ImGui::Separator();
            if (probe.cubemapSlice == ReflectionProbeComponent::kInvalidSlice)
                ImGui::Text("Cubemap Slice: <unassigned>");
            else
                ImGui::Text("Cubemap Slice: %u", probe.cubemapSlice);

            const bool baked = probe.IsBaked();
            ImGui::Text("Status: %s", baked ? "Baked" : "Not Baked");

            const bool canBake = m_renderer
                              && probe.cubemapSlice != ReflectionProbeComponent::kInvalidSlice;
            if (!canBake) ImGui::BeginDisabled();
            if (ImGui::Button("Bake This Probe"))
            {
                probe.RequestRebake();
                m_renderer->BakeProbe(probe.cubemapSlice);
            }
            if (!canBake) ImGui::EndDisabled();
            if (probe.NeedsRebake())
            {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f), "(pending)");
            }
            if (probe.realtime)
            {
                ImGui::TextDisabled("Auto-rebake every N frames (60 = 1s @ 60fps).");
                ImGui::TextDisabled("Each tick costs one full bake (~6 face draws + prefilter).");
            }
        },
        /*requires*/ [](World& w, Entity e) -> bool
        {
            return w.GetComponent<GlobalTransform>(e) != nullptr;
        });

    // DDGI Volume — DDGI probe ray-trace volume; needs DXR. postDraw shows readiness + probe-density warning.
    RegisterReflectedComponent<DDGIVolumeComponent>("DDGI Volume",
        /*priority*/ 56,
        [this](DDGIVolumeComponent& v, World*, Entity)
        {
            ImGui::Separator();
            if (m_renderer)
            {
                ImGui::TextDisabled(m_renderer->IsDDGIReady()
                    ? "DXR ready — volume active."
                    : "DXR unavailable — Sky IBL fallback only.");
            }
            const uint32_t totalProbes = v.probeCountsX * v.probeCountsY * v.probeCountsZ;
            ImGui::TextDisabled("Total probes: %u", totalProbes);
            if (totalProbes > 4096)
                ImGui::TextColored(ImVec4(1, 0.7f, 0.2f, 1),
                                   "(>4096 probes - atlas memory ~%u KB)",
                                   (totalProbes * 8 * 8 * 4) / 1024);
        },
        /*requires*/ [](World& w, Entity e) -> bool
        {
            return w.GetComponent<GlobalTransform>(e) != nullptr;
        });

    // IndirectLightingSettings — scene-singleton DDGI/IBL fallback knobs (first matching entity wins).
    RegisterReflectedComponent<IndirectLightingSettingsComponent>(
        "Indirect Lighting Settings", /*priority*/ 57);

    // Decal — clustered compute decal; references shared DecalMaterialAsset (managed via Tools → Decal Materials).
    RegisterReflectedComponent<DecalComponent>("Decal",
        /*priority*/ 58,
        [this](DecalComponent& dcRef, World*, Entity)
        {
            auto* dc = &dcRef;
            if (!m_renderer)
            {
                ImGui::TextDisabled("(Renderer not wired — cannot resolve material library.)");
                return;
            }
            auto& lib = m_renderer->GetDecalMaterialLibrary();
            const auto& assets = lib.GetAll();

            // ---- Material picker — current asset name + combo to swap/clear. ----
            const char* currentName = dc->material
                ? dc->material->name.c_str()
                : "(none)";

            if (ImGui::BeginCombo("Material", currentName))
            {
                // "None" clears the assignment — entity stops contributing a decal.
                if (ImGui::Selectable("(none)", !dc->material))
                    dc->material.reset();

                ImGui::Separator();

                // List every asset. Sort for stable ordering.
                std::vector<const std::string*> names;
                names.reserve(assets.size());
                for (const auto& [name, _] : assets) names.push_back(&name);
                std::sort(names.begin(), names.end(),
                          [](const std::string* a, const std::string* b){ return *a < *b; });

                for (const std::string* namePtr : names)
                {
                    const bool selected =
                        dc->material && dc->material->name == *namePtr;
                    if (ImGui::Selectable(namePtr->c_str(), selected))
                        dc->material = lib.Find(*namePtr);
                }
                if (assets.empty())
                    ImGui::TextDisabled("(library empty — open Tools → Decal Materials)");
                ImGui::EndCombo();
            }

            // Shortcut button: open the Decal Materials window for editing
            // the selected asset.
            ImGui::SameLine();
            if (ImGui::SmallButton("Edit..."))
            {
                m_showDecalMaterials = true;
                if (dc->material) m_selectedDecalMatName = dc->material->name;
            }

            if (!dc->material)
            {
                ImGui::TextDisabled("No material assigned — decal is inactive.");
                return;
            }

            ImGui::Separator();

            // ---- Per-instance state -------------------------------------------
            ImGui::SliderFloat("Fade Alpha", &dc->fadeAlpha, 0.0f, 1.0f,
                               "%.2f  (multiplies asset opacity)");

            // Lifetime: -1=static, ≥0=dynamic (drag for remaining seconds + fade + destroy-on-expire).
            bool isDynamic = (dc->lifetime >= 0.0f);
            if (ImGui::Checkbox("Dynamic", &isDynamic))
                dc->lifetime = isDynamic ? 5.0f : -1.0f;
            if (isDynamic)
            {
                ImGui::SameLine();
                ImGui::SetNextItemWidth(-1.f);
                ImGui::DragFloat("##lifetime", &dc->lifetime, 0.1f, 0.0f, 600.0f,
                                 "Lifetime: %.1f s remaining");
                ImGui::DragFloat("Fade-out", &dc->fadeOutDuration, 0.05f, 0.0f, 60.0f,
                                 "%.2f s  (0 = instant pop)");
                ImGui::Checkbox("Destroy Entity on Expire", &dc->destroyEntityOnExpire);
                if (!dc->destroyEntityOnExpire)
                    ImGui::TextDisabled("Only the DecalComponent is removed; entity stays alive.");
            }
            else
            {
                ImGui::SameLine();
                ImGui::TextDisabled("(static — never expires)");
            }

            // Tint override: w>0 means use override; UI splits into toggle + color picker.
            ImGui::Separator();
            bool overrideOn = (dc->tintOverride.w > 0.0f);
            if (ImGui::Checkbox("Override Tint", &overrideOn))
            {
                if (overrideOn)
                {
                    // Seed with asset tint so the editor doesn't show pure black.
                    dc->tintOverride = dc->material->baseColorTint;
                    if (dc->tintOverride.w <= 0.0f) dc->tintOverride.w = 1.0f;
                }
                else
                {
                    dc->tintOverride.w = 0.0f;
                }
            }
            if (overrideOn)
            {
                ImGui::ColorEdit4("Override Color", &dc->tintOverride.x,
                                  ImGuiColorEditFlags_AlphaBar);
            }

            // ---- Bound-texture status (read-only) ------------------------------
            ImGui::Separator();
            const auto& a = *dc->material;
            const bool fullyResolved = a.IsFullyResolved();
            ImGui::TextColored(
                fullyResolved ? ImVec4(0.4f, 0.9f, 0.4f, 1.f)
                              : ImVec4(0.9f, 0.8f, 0.3f, 1.f),
                fullyResolved ? "Material resolved."
                              : "Material textures loading...");
            // Per-slot status: "-"=unused, "loading"=path set but not GPU-resident, "bound"=ready.
            for (uint32_t s = 0; s < Resource::DecalMaterialAsset::SLOT_COUNT; ++s)
            {
                const auto slot = static_cast<Resource::DecalMaterialAsset::TextureSlot>(s);
                const char* st = a.texBindless[s] >= 0    ? "bound"
                               : a.texPaths[s].empty()   ? "-"
                                                          : "loading";
                ImGui::TextDisabled("  %-13s: %s",
                    Resource::DecalMaterialAsset::SlotLabel(slot), st);
            }
        },
        /*requires*/ [](World& w, Entity e) -> bool {
            return w.GetComponent<GlobalTransform>(e) != nullptr;
        });

    // Post-Process Volume — center follows GlobalTransform; 4 override sections in postDraw need Stack base params.
    RegisterReflectedComponent<ECS::VolumeComponent>("Post-Process Volume",
        /*priority*/ 55,
        [renderer](ECS::VolumeComponent& vc, World*, Entity)
        {
            PostProcess::Volume& v = vc.volume;
            PostProcess::ParameterStore* baseParams = nullptr;
            if (renderer)
            {
                if (auto* stack = renderer->GetPostProcessStack())
                    baseParams = &stack->GetParameters();
            }
            ImGui::InputText("Label", v.label, sizeof(v.label));
            ImGui::Separator();
            ImGui::TextDisabled("Overrides");

            // --- CAS ---
            {
                bool has = v.override.cas.has_value();
                if (ImGui::Checkbox("Override CAS", &has))
                {
                    if (has)
                        v.override.cas = baseParams ? baseParams->GetCAS()
                                                    : PostProcess::CASParams{};
                    else v.override.cas.reset();
                }
                if (v.override.cas)
                {
                    auto& cas = *v.override.cas;
                    ImGui::Indent();
                    ImGui::Checkbox("Enabled##vc_cas", &cas.enabled);
                    ImGui::SliderFloat("Sharpness##vc_cas",
                        &cas.sharpness, 0.0f, 1.0f, "%.2f");
                    ImGui::Unindent();
                }
            }

            // --- AutoExposure ---
            {
                bool has = v.override.autoExposure.has_value();
                if (ImGui::Checkbox("Override AutoExposure", &has))
                {
                    if (has)
                        v.override.autoExposure = baseParams
                            ? baseParams->GetAutoExposure()
                            : PostProcess::AutoExposureParams{};
                    else v.override.autoExposure.reset();
                }
                if (v.override.autoExposure)
                {
                    auto& ae = *v.override.autoExposure;
                    ImGui::Indent();
                    ImGui::Checkbox("Enabled##vc_ae", &ae.enabled);
                    ImGui::SliderFloat("Manual Exposure##vc_ae",
                        &ae.manualExposure, 0.05f, 16.0f, "%.3f",
                        ImGuiSliderFlags_Logarithmic);
                    ImGui::SliderFloat("EV Bias##vc_ae",
                        &ae.evBias, -3.0f, 3.0f, "%.2f EV");
                    ImGui::SliderFloat("Key##vc_ae",
                        &ae.keyValue, 0.05f, 0.5f, "%.3f");
                    ImGui::Unindent();
                }
            }

            // --- Bloom (placeholder) ---
            {
                bool has = v.override.bloom.has_value();
                if (ImGui::Checkbox("Override Bloom", &has))
                {
                    if (has)
                        v.override.bloom = baseParams ? baseParams->GetBloom()
                                                      : PostProcess::BloomParams{};
                    else v.override.bloom.reset();
                }
                if (v.override.bloom)
                {
                    ImGui::Indent();
                    ImGui::TextDisabled("(no user-facing params yet)");
                    ImGui::Unindent();
                }
            }

            // --- Tonemapping ---
            {
                bool has = v.override.tonemapping.has_value();
                if (ImGui::Checkbox("Override Tonemapping", &has))
                {
                    if (has)
                        v.override.tonemapping = baseParams
                            ? baseParams->GetTonemapping()
                            : PostProcess::TonemappingParams{};
                    else v.override.tonemapping.reset();
                }
                if (v.override.tonemapping)
                {
                    auto& tm = *v.override.tonemapping;
                    ImGui::Indent();
                    ImGui::SliderFloat("Bloom Strength##vc_tm",
                        &tm.bloomStrength, 0.0f, 1.0f, "%.3f");
                    ImGui::Checkbox("Color Grading##vc_tm",
                        &tm.colorGradingEnabled);
                    if (tm.colorGradingEnabled)
                    {
                        auto& g = tm.grading;
                        ImGui::SliderFloat("Exposure##vc_tmg",
                            &g.exposure, -5.0f, 5.0f, "%.2f EV");
                        ImGui::SliderFloat("Contrast##vc_tmg",
                            &g.contrast, 0.2f, 3.0f, "%.2f");
                        ImGui::SliderFloat("Saturation##vc_tmg",
                            &g.saturation, 0.0f, 2.0f, "%.2f");
                        ImGui::SliderFloat("Temperature##vc_tmg",
                            &g.temperature, -1.0f, 1.0f, "%.2f");
                        ImGui::SliderFloat("Tint##vc_tmg",
                            &g.tint, -1.0f, 1.0f, "%.2f");
                    }
                    ImGui::Unindent();
                }
            }
        });

    // ParticleEmitterComponent — descriptor for editable fields; postDraw adds texture badge + mesh-source name.
    RegisterReflectedComponent<ParticleEmitterComponent>("Particle Emitter",
        /*priority*/ 61,
        [](ParticleEmitterComponent& pe, World* w, Entity)
        {
            ImGui::TextDisabled("Texture: %s  idx=%d",
                pe.texturePath.empty() ? "(no texture)" : "bound",
                pe.textureBindlessIdx);
            if (pe.shape == ParticleShape::Mesh
                && w && pe.meshSourceEntity != NullEntity)
            {
                const std::string& name = w->GetName(pe.meshSourceEntity);
                ImGui::TextDisabled("Mesh Source: %s", name.c_str());
            }
        });

    // Trail — descriptor for visual/sampling; trailSlot is runtime-assigned (postDraw info line).
    RegisterReflectedComponent<TrailComponent>("Trail",
        /*priority*/ 62,
        [](TrailComponent& tc, World*, Entity)
        {
            ImGui::TextDisabled("Slot: %u  (auto-assigned on first sample)",
                                tc.trailSlot);
        });

    // Timeline — AnimNotify tracks. Uses the raw RegisterComponentEditor
    // path (no Reflect::Descriptor) because the nested vector<Track> with
    // PropertyBag values doesn't map cleanly onto REFLECT_FLOAT / REFLECT_
    // STRING etc. The full track/notify UI lives in the Timeline Editor
    // panel; this row just gives a one-line summary + a button to open it.
    RegisterComponentEditor<TimelineComponent>("Timeline",
        [this](void* ptr, World*, Entity) {
            auto& tl = *static_cast<TimelineComponent*>(ptr);
            size_t numNotifies = 0, numStates = 0;
            for (const auto& tr : tl.tracks) {
                numNotifies += tr.notifies.size();
                numStates   += tr.states.size();
            }
            ImGui::Text("%zu tracks, %zu notifies, %zu states",
                        tl.tracks.size(), numNotifies, numStates);
            ImGui::Text("Duration: %.2fs", tl.clipDuration);
            ImGui::TextDisabled("Last observed time: %.3fs", tl.lastObservedTime);
            if (ImGui::Button("Open Timeline Editor"))
                m_showTimeline = true;
        },
        /*priority*/ 63);

    // BeamComponent — descriptor for globals + control points; postDraw seeds default + shader hint.
    RegisterReflectedComponent<BeamComponent>("Beam (Procedural Tube)",
        /*priority*/ 63,
        [](BeamComponent& bc, World*, Entity)
        {
            if (bc.controlPoints.size() < 2)
            {
                BeamControlPoint p0, p1;
                p0.position = { 0.0f, 0.0f, 0.0f };
                p1.position = { 5.0f, 0.0f, 0.0f };
                p0.radius   = p1.radius   = 0.3f;
                p0.colorTint = p1.colorTint = { 1.0f, 0.5f, 0.2f, 4.0f };
                bc.controlPoints = { p0, p1 };
            }
            ImGui::Separator();
            ImGui::TextDisabled("Slot: %s  (auto-acquired on first frame)",
                                bc.beamSlot == 0xFFFFFFFFu
                                    ? "(unassigned)"
                                    : std::to_string(bc.beamSlot).c_str());
            ImGui::TextWrapped(
                "Hint: control points are interpreted as world-space when the "
                "entity transform is identity. Add a MaterialComponent with "
                "useCustomShader=true and shader path "
                "'shaders/BeamTube_InnerCore.ps.hlsl' (Opaque) for the inner "
                "core, or 'BeamTube_OuterGlow.ps.hlsl' (Additive) for the "
                "outer glow shell.");
        });

    // AIComponent — descriptor for enabled/tickInterval; postDraw walks BTInstance trace via BTAsset lookup.
    RegisterReflectedComponent<AIComponent>("AI (Behavior Tree)",
        /*priority*/ 65,
        [aiSys = &m_aiSys](AIComponent& ai, World*, Entity)
        {
            // BT slot — drag a .bt.lua file from the Resource panel onto
            // this button to attach. Shows the current path (or a hint when
            // empty); right-click clears. Loading goes through
            // AISystem::AcquireTree so the tree is cached by path.
            const std::string& path = ai.treePath;
            const char* label = path.empty()
                ? "Drop .bt.lua here..."
                : path.c_str();

            ImGui::PushID("##aiTreeSlot");
            // Use a wide button so the drop zone is obvious.
            const ImVec2 slotSize(-FLT_MIN, 0);
            ImGui::Button(label, slotSize);

            if (ImGui::BeginDragDropTarget())
            {
                if (const ImGuiPayload* p =
                        ImGui::AcceptDragDropPayload("ILUA_PATH"))
                {
                    const char* dropped = static_cast<const char*>(p->Data);
                    if (*aiSys && dropped && *dropped)
                    {
                        if (auto tree = (*aiSys)->AcquireTree(dropped))
                        {
                            ai.tree     = tree;
                            ai.treePath = dropped;
                            if (!ai.instance)
                                ai.instance = std::make_unique<AI::BTInstance>();
                            LOG_INFO("AI: attached '%s' to entity", dropped);
                        }
                        else
                        {
                            LOG_ERROR("AI: AcquireTree failed for '%s' "
                                      "(not a valid .bt.lua?)", dropped);
                        }
                    }
                }
                ImGui::EndDragDropTarget();
            }

            // Right-click → detach.
            if (ImGui::BeginPopupContextItem("##aiTreeCtx"))
            {
                if (ImGui::MenuItem("Detach", nullptr, false, !path.empty()))
                {
                    ai.tree.reset();
                    ai.instance.reset();
                    ai.treePath.clear();
                }
                ImGui::EndPopup();
            }
            ImGui::PopID();

            // Status line — what's currently attached + a Reload button so
            // edits to the .bt.lua hot-reload without re-drag.
            if (!path.empty())
            {
                ImGui::SameLine(0, 4);
                ImGui::BeginDisabled(!(*aiSys));
                if (ImGui::SmallButton("Reload"))
                {
                    // ReloadTree forces a re-parse and updates the path-cache,
                    // so subsequent AcquireTree calls (other entities, scene
                    // load) get the fresh tree. Existing aliased shared_ptrs
                    // keep their stale copy until they re-acquire.
                    if (auto tree = (*aiSys)->ReloadTree(path))
                    {
                        ai.tree = tree;
                        if (!ai.instance)
                            ai.instance = std::make_unique<AI::BTInstance>();
                        LOG_INFO("AI: reloaded '%s'", path.c_str());
                    }
                }
                ImGui::EndDisabled();
            }

            if (!ai.tree || !ai.instance || ai.instance->trace.empty()) return;
            ImGui::Separator();
            ImGui::TextDisabled("Last Tick Trace (%zu nodes)",
                                ai.instance->trace.size());
            for (const auto& te : ai.instance->trace)
            {
                const AI::BTNode* n = ai.tree->FindNode(te.id);
                const char* type = n ? n->TypeName() : "?";
                const char* lbl  = (n && !n->name.empty()) ? n->name.c_str() : "";
                ImVec4 col(0.7f, 0.7f, 0.7f, 1.f);
                const char* st = "?";
                switch (te.status)
                {
                case AI::NodeStatus::Success: col = {0.4f, 0.9f, 0.4f, 1.f}; st = "Success"; break;
                case AI::NodeStatus::Failure: col = {0.9f, 0.4f, 0.4f, 1.f}; st = "Failure"; break;
                case AI::NodeStatus::Running: col = {1.0f, 0.85f, 0.3f, 1.f}; st = "Running"; break;
                default: break;
                }
                ImGui::TextColored(col, "[%u] %s %s - %s", te.id, type, lbl, st);
            }
        });

    // Blackboard — variant table not expressible as descriptor; postDraw renders live key/type/value rows.
    RegisterReflectedComponent<BlackboardComponent>("Blackboard",
        /*priority*/ 66,
        [](BlackboardComponent& bbRef, World*, Entity)
        {
            auto* bb = &bbRef;
            if (bb->values.empty())
            {
                ImGui::TextDisabled("(empty)");
                return;
            }
            if (ImGui::BeginTable("##bb", 3,
                ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
            {
                ImGui::TableSetupColumn("Key");
                ImGui::TableSetupColumn("Type");
                ImGui::TableSetupColumn("Value");
                ImGui::TableHeadersRow();
                for (auto& kv : bb->values)
                {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(kv.first.c_str());
                    ImGui::TableNextColumn();
                    std::visit([](const auto& v) {
                        using T = std::decay_t<decltype(v)>;
                        if      constexpr (std::is_same_v<T, bool>)        ImGui::TextUnformatted("bool");
                        else if constexpr (std::is_same_v<T, int>)         ImGui::TextUnformatted("int");
                        else if constexpr (std::is_same_v<T, float>)       ImGui::TextUnformatted("float");
                        else if constexpr (std::is_same_v<T, std::string>) ImGui::TextUnformatted("string");
                        else if constexpr (std::is_same_v<T, Entity>)      ImGui::TextUnformatted("Entity");
                        else if constexpr (std::is_same_v<T, std::vector<DirectX::XMFLOAT3>>)
                            ImGui::TextUnformatted("vec3[]");
                        else if constexpr (std::is_same_v<T, std::vector<std::string>>)
                            ImGui::TextUnformatted("string[]");
                        else                                               ImGui::TextUnformatted("vec3");
                    }, kv.second);
                    ImGui::TableNextColumn();
                    ImGui::PushID(kv.first.c_str());
                    std::visit([](auto& v) {
                        using T = std::decay_t<decltype(v)>;
                        if constexpr (std::is_same_v<T, bool>)
                            ImGui::Checkbox("##v", &v);
                        else if constexpr (std::is_same_v<T, int>)
                            ImGui::DragInt("##v", &v);
                        else if constexpr (std::is_same_v<T, float>)
                            ImGui::DragFloat("##v", &v, 0.01f);
                        else if constexpr (std::is_same_v<T, std::string>)
                            ImGui::TextUnformatted(v.c_str());
                        else if constexpr (std::is_same_v<T, Entity>)
                        {
                            int e = static_cast<int>(v);
                            if (ImGui::DragInt("##v", &e, 1.f, 0, INT_MAX))
                                v = static_cast<Entity>(e);
                        }
                        else if constexpr (std::is_same_v<T, std::vector<DirectX::XMFLOAT3>>)
                        {
                            // Per-element drag would be enormous; show a
                            // count summary + a "View" disclosure (not yet
                            // implemented — arrays are typically authored
                            // in the BT file, not the inspector).
                            char buf[48];
                            snprintf(buf, sizeof(buf), "[%zu pts]", v.size());
                            ImGui::TextUnformatted(buf);
                        }
                        else if constexpr (std::is_same_v<T, std::vector<std::string>>)
                        {
                            char buf[64];
                            snprintf(buf, sizeof(buf), "[%zu strs]", v.size());
                            ImGui::TextUnformatted(buf);
                        }
                        else
                        {
                            ImGui::DragFloat3("##v", &v.x, 0.01f);
                        }
                    }, kv.second);
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
        });

    // ---- Category assignments — popup groups submenus; unset ones default to
    //      "Misc". Submenu order is driven by kCategoryOrder in the Add Component
    //      popup. Every addable component should appear here exactly once so none
    //      silently fall into "Misc". (Tags registered via RegisterComponentTag
    //      have no Add entry and are intentionally absent.) ----

    // -- Transform --
    SetComponentCategory<LocalTransform>                  ("Transform");

    // -- Rendering -- (renderable surfaces / visibility)
    SetComponentCategory<MeshHandle>                      ("Rendering");
    SetComponentCategory<VisibilityComponent>             ("Rendering");
    SetComponentCategory<VideoComponent>                  ("Rendering");

    // -- Lighting -- (light sources + global illumination)
    SetComponentCategory<LightData>                       ("Lighting");
    SetComponentCategory<VolumetricLightComponent>        ("Lighting");
    SetComponentCategory<ReflectionProbeComponent>        ("Lighting");
    SetComponentCategory<DDGIVolumeComponent>             ("Lighting");
    SetComponentCategory<IndirectLightingSettingsComponent>("Lighting");

    // -- Environment -- (sky / atmosphere / time-of-day / terrain)
    SetComponentCategory<SkyboxComponent>                 ("Environment");
    SetComponentCategory<AtmosphereComponent>             ("Environment");
    SetComponentCategory<TODConfigComponent>              ("Environment");
    SetComponentCategory<TODOutputComponent>              ("Environment");
    SetComponentCategory<SunLightTag>                     ("Environment");
    SetComponentCategory<MoonLightTag>                    ("Environment");
    SetComponentCategory<CloudComponent>                  ("Environment");
    SetComponentCategory<TerrainComponent>                ("Environment");

    // -- Camera -- (lens + controllers + VCam stack)
    SetComponentCategory<CameraComponent>                 ("Camera");
    SetComponentCategory<CameraControllerComponent>       ("Camera");
    SetComponentCategory<VirtualCameraComponent>          ("Camera");
    SetComponentCategory<CameraPoseComponent>             ("Camera");
    SetComponentCategory<VCamPriorityComponent>           ("Camera");
    SetComponentCategory<VCamBlendComponent>              ("Camera");
    SetComponentCategory<FollowCameraComponent>           ("Camera");
    SetComponentCategory<AimCameraComponent>              ("Camera");
    SetComponentCategory<CameraShakeComponent>            ("Camera");
    SetComponentCategory<LiveCameraComponent>             ("Camera");

    // -- Animation -- (skeletons / clips / morphs / sockets / IK / timeline)
    SetComponentCategory<SkeletonComponent>               ("Animation");
    SetComponentCategory<SkeletonRef>                     ("Animation");
    SetComponentCategory<AnimationComponent>              ("Animation");
    SetComponentCategory<MorphComponent>                  ("Animation");
    SetComponentCategory<SocketComponent>                 ("Animation");
    SetComponentCategory<FootIKComponent>                 ("Animation");
    SetComponentCategory<TimelineComponent>               ("Animation");

    // -- Physics -- (rigid bodies / colliders / chains)
    SetComponentCategory<RigidBodyComponent>              ("Physics");
    SetComponentCategory<ColliderComponent>               ("Physics");
    SetComponentCategory<CapsuleColliderComponent>        ("Physics");
    SetComponentCategory<ChainPhysicsComponent>           ("Physics");

    // -- Gameplay -- (character motor + player state)
    SetComponentCategory<CharacterControllerComponent>    ("Gameplay");
    SetComponentCategory<PlayerComponent>                 ("Gameplay");

    // -- VFX -- (decals / particles / trails / beams)
    SetComponentCategory<DecalComponent>                  ("VFX");
    SetComponentCategory<ParticleEmitterComponent>        ("VFX");
    SetComponentCategory<TrailComponent>                  ("VFX");
    SetComponentCategory<BeamComponent>                   ("VFX");

    // -- Post-Process --
    SetComponentCategory<ECS::VolumeComponent>            ("Post-Process");

    // -- UI -- (screen-space + world-space widgets)
    SetComponentCategory<UI::UIRootComponent>             ("UI");
    SetComponentCategory<UI::UIScreenSpaceComponent>      ("UI");
    SetComponentCategory<UI::UIImageComponent>            ("UI");
    SetComponentCategory<UI::UITextComponent>             ("UI");
    SetComponentCategory<UI::UIBarComponent>              ("UI");
    SetComponentCategory<UI::WorldSpaceUIComponent>       ("UI");
    SetComponentCategory<UI::WorldUIBarComponent>         ("UI");
    SetComponentCategory<UI::WorldUITextComponent>        ("UI");
    SetComponentCategory<UI::WorldUIImageComponent>       ("UI");
    SetComponentCategory<UI::DamageNumberComponent>       ("UI");

    // -- AI / Script -- (strategic/tactical/sensing AI + behavior tree + Lua)
    SetComponentCategory<AIIntentComponent>               ("AI / Script");
    SetComponentCategory<NavAgentComponent>               ("AI / Script");
    SetComponentCategory<PerceptionComponent>             ("AI / Script");
    SetComponentCategory<AIComponent>                     ("AI / Script");
    SetComponentCategory<BlackboardComponent>             ("AI / Script");
    SetComponentCategory<ScriptComponent>                 ("AI / Script");

    // -- Attachment -- (follow entity / socket)
    SetComponentCategory<FollowEntityComponent>           ("Attachment");
    SetComponentCategory<FollowSocketComponent>           ("Attachment");
}

// ---- RenderTimelinePanel — Animation timeline editor (dockable) ----
// Two tabs:
//   * "Clip Asset (.ianim)" — Unreal AnimSequence-style: edit AnimNotify
//     tracks that live ON the animation clip. Save round-trips to disk.
//   * "Entity Timeline"     — legacy per-entity TimelineComponent editing.
// The shared NotifyTrackEditor drives both; only its target differs.
void EditorLayer::RenderTimelinePanel()
{
    ImGui::SetNextWindowSize(ImVec2(960, 380), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Animation Timeline", &m_showTimeline)) { ImGui::End(); return; }

    // Drive the curve editor's read-only playback off our own clock; never let
    // it self-advance (the notify editor owns the preview clock).
    m_curveEditor.SetAnimationClipSystem(m_animClipSys);

    if (ImGui::BeginTabBar("##animTimelineTabs"))
    {
        if (ImGui::BeginTabItem("Clip Asset (.ianim)"))
        {
            RenderClipAssetTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Entity Timeline"))
        {
            RenderEntityTimelineTab();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    ImGui::End();
}

// ---- Clip-asset tab: edit notify tracks stored ON the .ianim clip ----------
void EditorLayer::RenderClipAssetTab()
{
    // ---- Poll a pending async load ----
    if (m_animEditHandle.packed != 0 && !m_animEditReady && m_animClipSys
        && m_animClipSys->IsReady(m_animEditHandle))
    {
        m_animEditReady = true;
        m_animEditClipIdx = 0;
        m_animEditClipMirrored = -1;     // force a scratch refresh below
        if (const Resource::AnimationResource* res = m_animClipSys->GetResource(m_animEditHandle))
            m_curveEditor.LoadFromAnimationResource(*res);
    }

    // ---- Toolbar: path + load + save ----
    ImGui::SetNextItemWidth(360.f);
    ImGui::InputTextWithHint("##animpath", "asset/anim/clip.ianim",
                             m_animPathInput, sizeof(m_animPathInput));
    ImGui::SameLine();
    if (ImGui::Button("Load") && m_animPathInput[0])
        LoadAnimForEdit(m_animPathInput);
    ImGui::SameLine();
    if (ImGui::Button("Save"))
    {
        if (SaveAnimEdit()) m_animSaveStatus = "Saved " + m_animEditPath;
        else                m_animSaveStatus = "Save failed";
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Re-serialize the loaded .ianim (bones/morphs untouched, notify tracks updated)");
    ImGui::SameLine();
    ImGui::TextDisabled("(drag a .ianim from the asset browser here)");

    // Window-wide drag-drop target for .ianim files.
    if (ImGui::BeginDragDropTarget())
    {
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("IANIM_PATH"))
        {
            const char* path = static_cast<const char*>(p->Data);
            if (path && path[0]) LoadAnimForEdit(path);
        }
        ImGui::EndDragDropTarget();
    }

    if (!m_animSaveStatus.empty())
    {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.6f, 1.f, 0.6f, 1.f), "%s", m_animSaveStatus.c_str());
    }

    if (m_animEditHandle.packed == 0)
    {
        ImGui::Separator();
        ImGui::TextDisabled("No clip loaded. Drag a .ianim here or type a path and press Load.");
        return;
    }
    if (!m_animEditReady)
    {
        ImGui::Separator();
        ImGui::TextColored(ImVec4(1.f, 1.f, 0.3f, 1.f), "Loading %s ...", m_animEditPath.c_str());
        return;
    }

    // Borrowed pointer, re-fetched every frame (never stored across frames, so
    // the "do NOT store" contract holds). Safe to hold through this function:
    // the editor runs single-threaded in the Render phase, after the frame's
    // AnimationClipSystem::Tick, and nothing here loads/unloads a resource that
    // could reallocate ResourceManager storage mid-call.
    Resource::AnimationResource* res =
        m_animClipSys ? m_animClipSys->GetResourceMutable(m_animEditHandle) : nullptr;
    if (!res || res->clips.empty())
    {
        ImGui::Separator();
        ImGui::TextDisabled("Loaded resource has no bone clips.");
        return;
    }

    // ---- Clip selector (resources can hold multiple clips) ----
    m_animEditClipIdx = std::clamp(m_animEditClipIdx, 0, (int)res->clips.size() - 1);
    if (res->clips.size() > 1)
    {
        if (ImGui::BeginCombo("Clip", res->clips[m_animEditClipIdx].name))
        {
            for (int i = 0; i < (int)res->clips.size(); ++i)
            {
                const bool sel = (i == m_animEditClipIdx);
                if (ImGui::Selectable(res->clips[i].name[0] ? res->clips[i].name : "(unnamed)", sel))
                {
                    // Don't reset m_animEditClipMirrored here — the sync block
                    // below detects the index change, flushes the OLD clip's
                    // edits back first, then re-points the scratch at the new
                    // clip. Resetting to -1 would lose the old-clip identity
                    // and silently discard its unsaved edits.
                    m_animEditClipIdx = i;
                }
            }
            ImGui::EndCombo();
        }
    }
    else
    {
        ImGui::Text("Clip: %s", res->clips[0].name[0] ? res->clips[0].name : "(unnamed)");
    }

    Resource::AnimClipData& clip = res->clips[m_animEditClipIdx];

    // ---- Sync scratch <-> clip ----
    // On (re)select, pull the clip's notify tracks into the scratch
    // TimelineComponent the NotifyTrackEditor edits. Re-pointing the editor's
    // target resets its undo/selection state, which is what we want per clip.
    if (m_animEditClipMirrored != m_animEditClipIdx)
    {
        // Flush the PREVIOUSLY-edited clip's scratch edits back first, or
        // switching clips would silently discard unsaved edits to the old one.
        if (m_animEditClipMirrored >= 0 &&
            m_animEditClipMirrored < (int)res->clips.size())
        {
            res->clips[m_animEditClipMirrored].notifyTracks = m_clipScratch.tracks;
            res->clips[m_animEditClipMirrored].nextNotifyId = m_clipScratch.nextNotifyId;
        }

        m_clipScratch.tracks       = clip.notifyTracks;
        m_clipScratch.clipDuration = clip.duration;
        m_clipScratch.nextNotifyId = clip.nextNotifyId;
        m_clipScratch.lastObservedTime = -1.f;
        m_animEditClipMirrored = m_animEditClipIdx;
        m_notifyTrackEditor.SetTarget(nullptr);             // force a clean re-arm
        m_notifyTrackEditor.SetTarget(&m_clipScratch);
    }

    // ---- Preview options ----
    ImGui::Checkbox("Preview on selected entity", &m_animPreviewOnEntity);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("While Play is active, drive the selected entity's animation\n"
                          "time and fire this clip's notifies through the real runtime path.");
    ImGui::SameLine();
    ImGui::Checkbox("Show bone curves", &m_animShowCurves);

    // Editing context: only bind to an entity when the user opted in. Resolve
    // the skeleton-owning entity (mesh children reference it via SkeletonRef);
    // AnimationComponent + SkeletonComponent live on that root.
    Entity previewEnt = NullEntity;
    if (m_animPreviewOnEntity && m_world && m_selectedEntity != NullEntity)
    {
        previewEnt = m_selectedEntity;
        if (auto* ref = m_world->GetComponent<SkeletonRef>(previewEnt))
            if (ref->entity != NullEntity) previewEnt = ref->entity;
    }
    // Re-arm the editor target every frame: the Entity-timeline tab may have
    // pointed the shared NotifyTrackEditor at a different TimelineComponent, so
    // this isn't redundant — it reclaims the editor for the clip scratch.
    m_notifyTrackEditor.SetTarget(&m_clipScratch);
    m_notifyTrackEditor.SetEditingEntity(previewEnt, m_world);

    // ---- The notify track editor (body only — we own the window) ----
    m_notifyTrackEditor.Render();

    // ---- Write scratch edits back into the clip asset (in memory) ----
    clip.notifyTracks = m_clipScratch.tracks;
    clip.nextNotifyId = m_clipScratch.nextNotifyId;

    // ---- Live preview: make the entity actually PLAY the edited clip --------
    // Bind this resource to the entity's skeleton (cached after the first call)
    // to get the contiguous ClipLibrary range, point primaryClip at the clip we
    // are editing (firstLib + local index), and stamp the edited notify tracks
    // onto that library clip. This guarantees previewed clip == edited clip even
    // for multi-clip resources, and TimelineSystem's clip-authoritative path
    // then fires exactly what the editor shows. Notifies are skeleton-
    // independent, so the stamp is a straight copy.
    if (previewEnt != NullEntity && m_renderer && m_animClipSys)
    {
        auto* anim = m_world->GetComponent<AnimationComponent>(previewEnt);
        auto* skel = m_world->GetComponent<SkeletonComponent>(previewEnt);
        SkeletonRegistry& skelReg = m_renderer->GetSkeletonRegistry();
        if (anim && skel && skel->assetIndex != kInvalidAnimHandle
            && skel->assetIndex < skelReg.Count())
        {
            const SkeletonAsset& sk = skelReg.Get(skel->assetIndex);
            ClipLibrary& lib = m_renderer->GetClipLibrary();
            const uint32_t firstLib =
                m_animClipSys->BindToSkeleton(m_animEditHandle, sk, lib);
            if (firstLib != kInvalidClipIndex)
            {
                const uint32_t libIdx = firstLib + static_cast<uint32_t>(m_animEditClipIdx);
                if (libIdx < lib.Count())
                {
                    anim->primaryClip = libIdx;
                    lib.GetMutable(libIdx).notifyTracks = clip.notifyTracks;
                }
            }
        }
    }

    // ---- Optional read-only bone-curve overlay (Unreal-style combined view) ----
    if (m_animShowCurves)
    {
        ImGui::Separator();
        if (ImGui::CollapsingHeader("Bone Curves (read-only)", ImGuiTreeNodeFlags_DefaultOpen))
        {
            // Mirror the notify editor's preview time onto the curve scrubber so
            // the two views stay aligned; never let the curve editor self-play.
            m_curveEditor.GetClip().isPlaying   = false;
            m_curveEditor.GetClip().currentTime = m_notifyTrackEditor.GetPreviewTime();
            ImGui::BeginChild("##curveOverlay", ImVec2(0, 240.f), true);
            m_curveEditor.Draw(0.f);
            ImGui::EndChild();
        }
    }
}

// ---- Entity-timeline tab: legacy per-entity TimelineComponent editing ------
void EditorLayer::RenderEntityTimelineTab()
{
    TimelineComponent* tl = nullptr;
    if (m_world && m_selectedEntity != NullEntity) {
        tl = m_world->GetComponent<TimelineComponent>(m_selectedEntity);
        if (!tl && m_world->GetComponent<AnimationComponent>(m_selectedEntity)) {
            TimelineComponent fresh;
            fresh.clipDuration = 2.f;
            m_world->AddComponent<TimelineComponent>(m_selectedEntity, std::move(fresh));
            tl = m_world->GetComponent<TimelineComponent>(m_selectedEntity);
        }
    }

    ImGui::TextDisabled("Per-entity override tracks (additive to clip-authored notifies).");
    m_notifyTrackEditor.SetTarget(tl);
    m_notifyTrackEditor.SetEditingEntity(m_selectedEntity, m_world);
    m_notifyTrackEditor.Render();
}

// ---- LoadAnimForEdit — begin async acquire of a .ianim for editing ---------
void EditorLayer::LoadAnimForEdit(const std::string& path)
{
    if (!m_animClipSys) return;

    // Release any previously-held edit handle so its refcount drops.
    if (m_animEditHandle.packed != 0)
        m_animClipSys->ReleaseClip(m_animEditHandle);

    m_animEditPath    = path;
    m_animEditHandle  = m_animClipSys->AcquireClip(path);
    m_animEditReady   = false;
    m_animEditClipIdx = 0;
    m_animEditClipMirrored = -1;
    m_animSaveStatus.clear();
    // Keep the manual-path box in sync with drag-drop loads.
    std::snprintf(m_animPathInput, sizeof(m_animPathInput), "%s", path.c_str());

    // Ready immediately if cached.
    if (m_animClipSys->IsReady(m_animEditHandle))
    {
        m_animEditReady = true;
        if (const Resource::AnimationResource* res = m_animClipSys->GetResource(m_animEditHandle))
            m_curveEditor.LoadFromAnimationResource(*res);
    }
}

// ---- SaveAnimEdit — re-serialize the edited resource back to .ianim --------
bool EditorLayer::SaveAnimEdit()
{
    if (!m_animClipSys || m_animEditHandle.packed == 0 || m_animEditPath.empty())
        return false;
    Resource::AnimationResource* res = m_animClipSys->GetResourceMutable(m_animEditHandle);
    if (!res) return false;

    const std::vector<uint8_t> blob = Resource::SerializeAnimation(*res);
    if (blob.empty()) return false;

    std::ofstream out(m_animEditPath, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(reinterpret_cast<const char*>(blob.data()),
              static_cast<std::streamsize>(blob.size()));
    return out.good();
}

// ---- RenderProfilerPanel — GPU/CPU timing with smoothing & history graph ----
//
// GPU costs are reported per-queue (Graphics vs Compute) and "Effective Frame"
// is the critical path = max(graphics, compute), since the two queues execute
// in parallel between cross-queue fences. The legacy `Total = sum of all
// regions` is still displayed below the effective number for reference.
void EditorLayer::RenderProfilerPanel()
{
    // ---- Update smoothed values & ring buffer history ----
    const float alpha = kProfilerEmaAlpha;

    // CPU EMA
    if (m_cpuSmoothed < 0.001f) m_cpuSmoothed = m_cpuFrameMs; // first frame init
    m_cpuSmoothed += alpha * (m_cpuFrameMs - m_cpuSmoothed);

    // GPU EMA — track total, per-queue, and effective frame separately.
    float rawTotalMs     = 0.f;
    float rawGraphicsMs  = 0.f;
    float rawComputeMs   = 0.f;
    float rawEffectiveMs = 0.f;
    if (m_gpuProfiler && m_gpuProfiler->hasResults)
    {
        rawTotalMs     = m_gpuProfiler->totalGpuMs;
        rawGraphicsMs  = m_gpuProfiler->perQueueTotalMs[GPUProfiler::kQueueGraphics];
        rawComputeMs   = m_gpuProfiler->perQueueTotalMs[GPUProfiler::kQueueCompute];
        rawEffectiveMs = m_gpuProfiler->effectiveFrameMs;
    }
    auto ema = [&](float& smoothed, float raw) {
        if (smoothed < 0.001f) smoothed = raw;
        smoothed += alpha * (raw - smoothed);
    };
    ema(m_gpuSmoothed,          rawTotalMs);
    ema(m_gpuEffectiveSmoothed, rawEffectiveMs);
    ema(m_gpuGraphicsSmoothed,  rawGraphicsMs);
    ema(m_gpuComputeSmoothed,   rawComputeMs);

    // Push into ring buffer — gpu history tracks the effective frame so the
    // graph shows actual wall-clock GPU work rather than sum-of-overlap.
    m_cpuHistory[m_historyOffset] = m_cpuFrameMs;
    m_gpuHistory[m_historyOffset] = rawEffectiveMs;
    m_historyOffset = (m_historyOffset + 1) % kProfilerHistoryLen;
    if (m_statFrameCount < kProfilerHistoryLen) ++m_statFrameCount;

    // Per-pass EMA — also stash the queue type so we can section by queue.
    if (m_gpuProfiler && m_gpuProfiler->hasResults)
    {
        for (uint32_t i = 0; i < m_gpuProfiler->resultCount; ++i)
        {
            const auto& r = m_gpuProfiler->results[i];
            int slot = -1;
            for (int s = 0; s < m_passStatsCount; ++s)
            {
                if (m_passStats[s].name == r.name) { slot = s; break; }
            }
            if (slot < 0 && m_passStatsCount < kMaxProfilerPasses)
            {
                slot = m_passStatsCount++;
                m_passStats[slot].name       = r.name;
                m_passStats[slot].smoothedMs = r.gpuMs;
                m_passStats[slot].queueType  = r.queueType;
            }
            if (slot >= 0)
            {
                m_passStats[slot].smoothedMs += alpha * (r.gpuMs - m_passStats[slot].smoothedMs);
                m_passStats[slot].queueType   = r.queueType; // refresh in case a pass migrated queues
            }
        }
    }

    // Compute min/avg/max over filled history
    {
        float cpuSum = 0.f, gpuSum = 0.f;
        float cpuMn = FLT_MAX, cpuMx = 0.f;
        float gpuMn = FLT_MAX, gpuMx = 0.f;
        for (int i = 0; i < m_statFrameCount; ++i)
        {
            float c = m_cpuHistory[i], g = m_gpuHistory[i];
            cpuSum += c; gpuSum += g;
            if (c < cpuMn) cpuMn = c; if (c > cpuMx) cpuMx = c;
            if (g < gpuMn) gpuMn = g; if (g > gpuMx) gpuMx = g;
        }
        float n = static_cast<float>(m_statFrameCount);
        m_cpuAvg = cpuSum / n; m_cpuMin = cpuMn; m_cpuMax = cpuMx;
        m_gpuAvg = gpuSum / n; m_gpuMin = gpuMn; m_gpuMax = gpuMx;
    }

    // ---- ImGui window ----
    ImGui::Begin("Profiler", &m_showProfiler);

    // FPS from smoothed CPU time
    float fps = (m_cpuSmoothed > 0.001f) ? 1000.0f / m_cpuSmoothed : 0.0f;

    // ---- CPU section ----
    if (ImGui::CollapsingHeader("CPU", 0))
    {
        ImGui::Text("Frame:  %.2f ms  (%.0f FPS)", m_cpuSmoothed, fps);
        ImGui::Text("Avg: %.2f ms   Min: %.2f ms   Max: %.2f ms",
                    m_cpuAvg, m_cpuMin, m_cpuMax);

        // PlotLines — reorder ring buffer into linear array for display
        float plotBuf[kProfilerHistoryLen];
        for (int i = 0; i < kProfilerHistoryLen; ++i)
            plotBuf[i] = m_cpuHistory[(m_historyOffset + i) % kProfilerHistoryLen];

        char overlay[32];
        snprintf(overlay, sizeof(overlay), "%.1f ms", m_cpuSmoothed);
        ImGui::PlotLines("##cpu_graph", plotBuf, kProfilerHistoryLen,
                         0, overlay, 0.f, m_cpuMax * 1.2f, ImVec2(-1, 60));
    }

    // ---- GPU section ----
    if (ImGui::CollapsingHeader("GPU", ImGuiTreeNodeFlags_DefaultOpen))
    {
        // Headline: effective frame (critical path) + per-queue breakdown.
        ImGui::Text("Effective Frame: %.2f ms   (= max(Gfx, Compute))",
                    m_gpuEffectiveSmoothed);
        ImGui::Text("  Graphics Queue:  %.2f ms", m_gpuGraphicsSmoothed);
        ImGui::Text("  Compute Queue:   %.2f ms", m_gpuComputeSmoothed);
        ImGui::TextDisabled("Sum (overlapping):  %.2f ms", m_gpuSmoothed);
        ImGui::Text("Effective Avg: %.2f   Min: %.2f   Max: %.2f ms",
                    m_gpuAvg, m_gpuMin, m_gpuMax);

        // PlotLines on effective frame
        float plotBuf[kProfilerHistoryLen];
        for (int i = 0; i < kProfilerHistoryLen; ++i)
            plotBuf[i] = m_gpuHistory[(m_historyOffset + i) % kProfilerHistoryLen];

        char overlay[32];
        snprintf(overlay, sizeof(overlay), "%.1f ms", m_gpuEffectiveSmoothed);
        ImGui::PlotLines("##gpu_graph", plotBuf, kProfilerHistoryLen,
                         0, overlay, 0.f, m_gpuMax * 1.2f, ImVec2(-1, 60));

        ImGui::Separator();

        if (m_gpuProfiler && m_gpuProfiler->hasResults)
        {
            // Section per queue. Bars normalised against the queue's own
            // total so a small compute-queue cost isn't visually swamped by
            // a large graphics-queue frame.
            auto renderQueueSection = [&](const char* header,
                                          uint8_t queueType,
                                          float queueTotalMs)
            {
                bool open = ImGui::TreeNodeEx(header,
                    ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_Framed,
                    "%s — %.2f ms", header, queueTotalMs);
                if (!open) return;

                const float barMax = (queueTotalMs > 0.01f) ? queueTotalMs : 1.0f;
                for (int s = 0; s < m_passStatsCount; ++s)
                {
                    const auto& ps = m_passStats[s];
                    if (!ps.name) continue;
                    if (ps.queueType != queueType) continue;

                    float frac = ps.smoothedMs / barMax;
                    if (frac > 1.f) frac = 1.f;

                    char label[80];
                    snprintf(label, sizeof(label), "%s: %.2f ms",
                             ps.name, ps.smoothedMs);
                    ImGui::ProgressBar(frac, ImVec2(-1, 0), label);
                }
                ImGui::TreePop();
            };

            renderQueueSection("Graphics Queue",
                               GPUProfiler::kQueueGraphics,
                               m_gpuGraphicsSmoothed);
            renderQueueSection("Compute Queue",
                               GPUProfiler::kQueueCompute,
                               m_gpuComputeSmoothed);
        }
        else
        {
            ImGui::TextDisabled("Waiting for data...");
        }
    }

    ImGui::End();

    // Sync enabled state when window is closed via X button.
    if (!m_showProfiler && m_gpuProfiler)
        m_gpuProfiler->enabled = false;
}
