#include "Scene/DataScene.h"
#include "Scene/SceneManager.h"
#include "System/Log.h"

void DataScene::Init(GameModeContext* ctx)
{
    m_ctx = ctx;
    if (!m_ctx || !m_ctx->sceneManager)
    {
        LOG_ERROR("DataScene('%s'): Init received no SceneManager — cannot load",
                  m_sceneId.c_str());
        return;
    }
    // Blocking load: clears the World, deserialises the .iscene, loads the
    // companion navmesh/post-process, and activates the scene's Lua scene
    // script. All the per-scene specifics live in DATA now.
    m_ctx->sceneManager->ActivateScene(m_sceneId);
}

void DataScene::Update(float /*dt*/)
{
    // Nothing — the active scene's OnSceneUpdate ticks inside ScriptSystem, and
    // engine systems tick in App on the shared World.
}

void DataScene::Shutdown()
{
    // Nothing to undo here: the next scene's activation drives the scene-script
    // OnSceneExit→OnSceneEnter swap; app-quit OnSceneExit is fired by
    // ScriptSystem teardown. World content is replaced by the next load.
    LOG_INFO("DataScene('%s'): Shutdown", m_sceneId.c_str());
}
