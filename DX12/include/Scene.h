#pragma once

class Scene
{
public:
	Scene() = default;
	virtual ~Scene() = default;
	virtual void Update(float deltaTime) = 0;
	virtual void Render() = 0;
	virtual void HandleInput() = 0;
};