#pragma once

// ReflectionEditor — generic ImGui drawer for Reflect::TypeDescriptor.
//
// Walks a TypeDescriptor's fields and emits one ImGui control per field
// (Drag/Slider/Color/Combo/Checkbox/InputText) using each FieldDescriptor's
// FieldKind + Widget hint. Returns true if any field changed this frame.
//
// EditorLayer's RegisterReflectedComponent<T>() helper wraps DrawObject<T>
// so most pure-data components need zero hand-written ImGui code.

#include "Reflection/Reflect.h"

namespace Reflect
{

// Draw every field in `desc` for the object pointed to by `obj`.
// Returns true if any field was modified this frame (use to mark dirty).
bool DrawObject(void* obj, const TypeDescriptor& desc);

// Draw a single field (exposed so callers can interleave reflected fields
// with hand-written widgets if needed).
bool DrawField(void* obj, const FieldDescriptor& f);

template <typename T>
inline bool DrawObject(T* obj)
{
    return DrawObject(static_cast<void*>(obj), Describe<T>());
}

} // namespace Reflect
