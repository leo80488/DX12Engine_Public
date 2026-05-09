#include "Scene/SceneManager.h"
#include "System/Log.h"

void SceneManager::PushScene(std::unique_ptr<IScene> scene, SceneContext& ctx)
{
    LOG_INFO("SceneManager: pushing scene '%s'", scene->GetName());
    scene->Init(&ctx);
    m_stack.push_back(std::move(scene));
}

void SceneManager::PopScene()
{
    if (m_stack.empty()) return;
    LOG_INFO("SceneManager: popping scene '%s'", m_stack.back()->GetName());
    m_stack.back()->Shutdown();
    m_stack.pop_back();
}

void SceneManager::Update(float dt)
{
    if (!m_stack.empty())
        m_stack.back()->Update( dt);
}

void SceneManager::OnUIRender(SceneContext& ctx)
{
    if (!m_stack.empty())
        m_stack.back()->OnUIRender(ctx);
}

IScene* SceneManager::GetActiveScene() const
{
    return m_stack.empty() ? nullptr : m_stack.back().get();
}
