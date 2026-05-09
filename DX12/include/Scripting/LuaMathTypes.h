#pragma once

// LuaMathTypes — lightweight math wrappers for Lua binding.
// Same memory layout as XMFLOAT3/XMFLOAT4 — reinterpret_cast safe.

struct LuaVec3
{
    float x = 0, y = 0, z = 0;
    LuaVec3() = default;
    LuaVec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}

    LuaVec3 operator+(const LuaVec3& o) const { return { x+o.x, y+o.y, z+o.z }; }
    LuaVec3 operator-(const LuaVec3& o) const { return { x-o.x, y-o.y, z-o.z }; }
    LuaVec3 operator*(float s)          const { return { x*s,   y*s,   z*s   }; }
    float   Length()                    const { return std::sqrtf(x*x + y*y + z*z); }
    LuaVec3 Normalized() const {
        float l = Length();
        return l > 1e-6f ? LuaVec3{x/l, y/l, z/l} : LuaVec3{};
    }
};

struct LuaQuat
{
    float x = 0, y = 0, z = 0, w = 1;
    LuaQuat() = default;
    LuaQuat(float x_, float y_, float z_, float w_) : x(x_), y(y_), z(z_), w(w_) {}
};
