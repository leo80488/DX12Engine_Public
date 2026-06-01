#include "Scene/GameModeStack.h"
#include "System/Log.h"

void GameModeStack::PushMode(std::unique_ptr<IGameMode> mode, GameModeContext& ctx)
{
    LOG_INFO("GameModeStack: pushing mode '%s'", mode->GetName());
    mode->Init(&ctx);
    m_stack.push_back(std::move(mode));
}

void GameModeStack::PopMode()
{
    if (m_stack.empty()) return;
    LOG_INFO("GameModeStack: popping mode '%s'", m_stack.back()->GetName());
    m_stack.back()->Shutdown();
    m_stack.pop_back();
}

void GameModeStack::Update(float dt)
{
    if (!m_stack.empty())
        m_stack.back()->Update(dt);
}

void GameModeStack::OnUIRender(GameModeContext& ctx)
{
    if (!m_stack.empty())
        m_stack.back()->OnUIRender(ctx);
}

IGameMode* GameModeStack::GetActive() const
{
    return m_stack.empty() ? nullptr : m_stack.back().get();
}
