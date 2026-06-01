#pragma once

// Forward declaration for the registration entry point. Defined in
// src/Editor/NotifyEditorCategoryDrawers.cpp and invoked once from
// EditorLayer setup (call site is idempotent — registering a category
// twice just overwrites the previous drawer).

void RegisterDefaultNotifyEditors();
