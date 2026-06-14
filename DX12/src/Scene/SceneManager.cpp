#include "Scene/SceneManager.h"
#include "Scene/DataScene.h"
#include "Scene/SceneDefaults.h"

#include "Resource/SceneSerializer.h"
#include "Resource/AssetManager.h"
#include "Resource/AssetFS.h"
#include "Graphics/Renderer.h"
#include "Graphics/IGraphicsDevice.h"
#include "Nav/NavMeshSystem.h"
#include "Scripting/ScriptSystem.h"
#include "Physics/PhysicsSystem.h"
#include "System/Log.h"

#include <nlohmann/json.hpp>
#include <fstream>

namespace
{
    // A scene id is a registry NAME or a raw .iscene PATH — this checks the
    // path case (pak first, then loose disk), mirroring GameScene's old helper.
    bool SceneFileExists(const std::string& path)
    {
        if (path.empty()) return false;
        if (::Resource::AssetFS::Get().HasInPak(path)) return true;
        std::ifstream f(path);
        return f.good();
    }

    // basename without directory or extension — used to name an unregistered
    // scene loaded straight from a path.
    std::string StemOf(const std::string& path)
    {
        const size_t slash = path.find_last_of("/\\");
        const size_t start = (slash == std::string::npos) ? 0 : slash + 1;
        const size_t dot   = path.find_last_of('.');
        const size_t end   = (dot == std::string::npos || dot < start) ? path.size() : dot;
        return path.substr(start, end - start);
    }
}

void SceneManager::Configure(IGraphicsDevice* gfx, Renderer* renderer, World* world,
                             Resource::AssetManager* assetMgr,
                             Resource::AnimationClipSystem* animClipSys,
                             Nav::NavMeshSystem* navSys, ScriptSystem* scriptSys,
                             DX12Physics::PhysicsSystem* physics)
{
    m_gfx         = gfx;
    m_renderer    = renderer;
    m_world       = world;
    m_assetMgr    = assetMgr;
    m_animClipSys = animClipSys;
    m_navSys      = navSys;
    m_scriptSys   = scriptSys;
    m_physics     = physics;
}

void SceneManager::SetTransitionHooks(ReplaceModeFn begin, ReplaceModeFn replace)
{
    m_beginTransition = std::move(begin);
    m_requestReplace  = std::move(replace);
}

bool SceneManager::LoadManifest(const std::string& gameJsonPath)
{
    m_registry.clear();
    m_startupScene.clear();

    std::string src;
    if (!::Resource::AssetFS::Get().ReadFileText(gameJsonPath, src))
    {
        LOG_WARNING("SceneManager: no manifest '%s' — scene registry empty",
                    gameJsonPath.c_str());
        return false;
    }

    try
    {
        nlohmann::json j = nlohmann::json::parse(src);

        if (j.contains("startup_scene") && j["startup_scene"].is_string())
            m_startupScene = j["startup_scene"].get<std::string>();

        // "scenes" : { "name" : "path.iscene" }            (path-only form)
        //          or { "name" : { "path":.., "script":.. } } (full form)
        if (j.contains("scenes") && j["scenes"].is_object())
        {
            for (auto& kv : j["scenes"].items())
            {
                const std::string& name = kv.key();
                const nlohmann::json& val = kv.value();
                SceneEntry e;
                if (val.is_string())
                {
                    e.path = val.get<std::string>();
                }
                else if (val.is_object())
                {
                    if (val.contains("path")   && val["path"].is_string())
                        e.path = val["path"].get<std::string>();
                    if (val.contains("script") && val["script"].is_string())
                        e.script = val["script"].get<std::string>();
                }
                if (!e.path.empty())
                    m_registry[name] = std::move(e);
                else
                    LOG_WARNING("SceneManager: scene '%s' has no path — skipped", name.c_str());
            }
        }
    }
    catch (const std::exception& ex)
    {
        LOG_ERROR("SceneManager: failed to parse '%s': %s", gameJsonPath.c_str(), ex.what());
        return false;
    }

    LOG_INFO("SceneManager: manifest loaded — %zu registered scene(s), startup='%s'",
             m_registry.size(), m_startupScene.c_str());
    return true;
}

bool SceneManager::ResolveScene(const std::string& nameOrPath,
                                std::string& outPath, std::string& outScript,
                                std::string& outName) const
{
    auto it = m_registry.find(nameOrPath);
    if (it != m_registry.end())
    {
        outPath   = it->second.path;
        outScript = it->second.script;
        outName   = it->first;
        return true;
    }
    // Not a registered name — accept it as a literal .iscene path if it exists.
    if (SceneFileExists(nameOrPath))
    {
        outPath = nameOrPath;
        outScript.clear();
        outName = StemOf(nameOrPath);
        return true;
    }
    return false;
}

std::vector<std::string> SceneManager::SceneNames() const
{
    std::vector<std::string> names;
    names.reserve(m_registry.size());
    for (auto& kv : m_registry) names.push_back(kv.first);
    return names;
}

bool SceneManager::ActivateScene(const std::string& nameOrPath)
{
    if (!m_world)
    {
        LOG_ERROR("SceneManager: ActivateScene('%s') with no World wired", nameOrPath.c_str());
        return false;
    }

    // Remember the exact request so Reload() can round-trip it (a registry
    // name resolves via the registry; a raw path loads directly — m_currentName
    // is only a lossy stem for path-loaded scenes).
    m_currentRequest = nameOrPath;

    std::string path, scriptFallback, name;
    if (!ResolveScene(nameOrPath, path, scriptFallback, name))
    {
        // Unresolved — try the id as a literal path; if it doesn't exist the
        // load fails into the default-world fallback below.
        path = nameOrPath;
        name = StemOf(nameOrPath);
        scriptFallback.clear();
    }

    LOG_INFO("SceneManager: activating scene '%s' (path='%s')", name.c_str(), path.c_str());

    // Safe world-reload order (mirrors EditorLayer::ProcessPendingActions):
    //   1. hard-sync every queue + drain deferred releases
    //   2. let the Renderer stash its caches for DEFERRED release
    //   3. LoadScene (which clears the World internally)
    // Reordering 1<->2 risks a delayed GPU TDR (bindless SRV slots reaching
    // just-freed memory); see the EditorLayer note.
    if (m_gfx)      m_gfx->WaitIdleAndReleaseDeferred();
    if (m_renderer) m_renderer->OnWorldClear();
    // Destroy the outgoing scene's Jolt bodies BEFORE the World is cleared —
    // World::Clear recycles entity IDs without firing destroy listeners, so the
    // per-entity reaper can't catch bodies on IDs reused-with-RigidBody next
    // scene; they would leak as ghost colliders + body-pool slots.
    if (m_physics)  m_physics->OnWorldClear();

    std::string loadedName, ppcPath, navPath, sceneScript;
    const bool ok = m_assetMgr
        ? ::Resource::LoadScene(path, *m_world, *m_assetMgr, m_renderer, m_animClipSys,
                                &loadedName, &ppcPath, &navPath, &sceneScript)
        : false;

    if (!ok)
    {
        LOG_ERROR("SceneManager: LoadScene('%s') failed — spawning default world", path.c_str());
        m_world->Clear();
        Scene::SpawnDefaultWorld(*m_world);
    }
    else if (!navPath.empty() && m_navSys)
    {
        // Companion navmesh — load so runtime FindPath / NavAgent are live.
        if (!m_navSys->Load(navPath))
            LOG_ERROR("SceneManager: navmesh '%s' failed to load — AI path-following off",
                      navPath.c_str());
    }

    // Behaviour: the .iscene's own declared sceneScript wins; otherwise fall
    // back to the manifest's script for this scene. Empty path = no script
    // (the previously-active scene script still exits cleanly).
    const std::string finalScript = !sceneScript.empty() ? sceneScript : scriptFallback;
    if (m_scriptSys)
        m_scriptSys->SetActiveSceneScript(finalScript, name);

    m_currentName = name;
    m_currentPath = path;
    return ok;
}

void SceneManager::RequestLoad(const std::string& nameOrPath, bool fade)
{
    // Validate for a friendlier log (the load still falls back to a default
    // world if the file turns out to be missing).
    std::string path, script, name;
    if (!ResolveScene(nameOrPath, path, script, name))
        LOG_WARNING("SceneManager: Scene.Load('%s') — not a registered name or existing "
                    "path; attempting as a literal path", nameOrPath.c_str());

    auto mode = std::make_unique<DataScene>(nameOrPath);
    if (fade && m_beginTransition)
        m_beginTransition(std::move(mode));
    else if (m_requestReplace)
        m_requestReplace(std::move(mode));
    else
        LOG_ERROR("SceneManager: no transition hooks wired — cannot load '%s'",
                  nameOrPath.c_str());
}

void SceneManager::Reload(bool fade)
{
    // Reuse the original request id (name or path) — NOT m_currentName, which
    // is a lossy stem for path-loaded scenes and would fail to re-resolve
    // (re-loading would then drop into the default-world fallback).
    if (m_currentRequest.empty())
    {
        LOG_WARNING("SceneManager: Reload with no current scene");
        return;
    }
    RequestLoad(m_currentRequest, fade);
}
