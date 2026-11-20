#pragma once
// Small math for the examples. Matrices are column-major (four float4 columns), the same memory the shaders read as `Mat4`
// (nox_gpu.slang), which multiplies with explicit columns -- no matrix-layout setting can change what they mean on either API.
//
// Conventions shared by every example on both APIs:
//   right-handed world, the camera looks down -Z;
//   reverse-Z: depth 1 at the near plane, 0 at infinity (clear depth to 0, compare GREATER) -- as the Nox engine does;
//   NDC +Y is up (NoxRHI flips the Vulkan viewport to match D3D12's).

#include <cmath>
#include <cstdint>

namespace nox
{
    struct Float3
    {
        float x = 0, y = 0, z = 0;
    };

    inline Float3 operator+(Float3 a, Float3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
    inline Float3 operator-(Float3 a, Float3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
    inline Float3 operator*(Float3 a, float s) { return { a.x * s, a.y * s, a.z * s }; }
    inline float Dot(Float3 a, Float3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
    inline Float3 Cross(Float3 a, Float3 b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
    inline Float3 Normalize(Float3 a)
    {
        const float length = std::sqrt(Dot(a, a));
        return length > 0.0f ? a * (1.0f / length) : a;
    }

    struct Float4
    {
        float x = 0, y = 0, z = 0, w = 0;
    };

    // Column-major 4 x 4: c[column] holds that column (the shader's Mat4).
    struct Mat4
    {
        Float4 c[4]{};

        static Mat4 Identity()
        {
            Mat4 m;
            m.c[0].x = m.c[1].y = m.c[2].z = m.c[3].w = 1.0f;
            return m;
        }

        float& At(int row, int column) { return (&c[column].x)[row]; }
        float At(int row, int column) const { return (&c[column].x)[row]; }
    };

    inline Mat4 operator*(const Mat4& a, const Mat4& b)
    {
        Mat4 r;
        for (int column = 0; column < 4; ++column)
            for (int row = 0; row < 4; ++row)
            {
                float sum = 0.0f;
                for (int k = 0; k < 4; ++k)
                    sum += a.At(row, k) * b.At(k, column);
                r.At(row, column) = sum;
            }
        return r;
    }

    inline Mat4 Translate(Float3 t)
    {
        Mat4 m = Mat4::Identity();
        m.c[3] = { t.x, t.y, t.z, 1.0f };
        return m;
    }

    inline Mat4 Scale(Float3 s)
    {
        Mat4 m = Mat4::Identity();
        m.c[0].x = s.x;
        m.c[1].y = s.y;
        m.c[2].z = s.z;
        return m;
    }

    inline Mat4 RotateY(float radians)
    {
        Mat4 m = Mat4::Identity();
        const float c = std::cos(radians), s = std::sin(radians);
        m.c[0] = { c, 0, -s, 0 };
        m.c[2] = { s, 0, c, 0 };
        return m;
    }

    inline Mat4 RotateX(float radians)
    {
        Mat4 m = Mat4::Identity();
        const float c = std::cos(radians), s = std::sin(radians);
        m.c[1] = { 0, c, s, 0 };
        m.c[2] = { 0, -s, c, 0 };
        return m;
    }

    // Right-handed view matrix.
    inline Mat4 LookAt(Float3 eye, Float3 target, Float3 up)
    {
        const Float3 f = Normalize(target - eye), s = Normalize(Cross(f, up)), u = Cross(s, f);
        Mat4 m = Mat4::Identity();
        m.c[0] = { s.x, u.x, -f.x, 0 };
        m.c[1] = { s.y, u.y, -f.y, 0 };
        m.c[2] = { s.z, u.z, -f.z, 0 };
        m.c[3] = { -Dot(s, eye), -Dot(u, eye), Dot(f, eye), 1 };
        return m;
    }

    // Reverse-Z perspective with an infinite far plane: depth = near / distance (1 at the near plane, 0 at infinity).
    inline Mat4 PerspectiveReverseZ(float fovYRadians, float aspect, float nearPlane)
    {
        const float f = 1.0f / std::tan(fovYRadians * 0.5f);
        Mat4 m;
        m.c[0] = { f / aspect, 0, 0, 0 };
        m.c[1] = { 0, f, 0, 0 };
        m.c[2] = { 0, 0, 0, -1 };
        m.c[3] = { 0, 0, nearPlane, 0 };
        return m;
    }

    // The six planes (inward normals, xyz, and distance in w) of a view-projection's frustum, for culling in a shader; the far plane
    // is left out (infinite).
    inline void FrustumPlanes(const Mat4& viewProj, Float4 planes[5])
    {
        const auto row = [&](int r) { return Float4{ viewProj.At(r, 0), viewProj.At(r, 1), viewProj.At(r, 2), viewProj.At(r, 3) }; };
        const Float4 r0 = row(0), r1 = row(1), r2 = row(2), r3 = row(3);
        const auto add = [](Float4 a, Float4 b, float s) { return Float4{ a.x + s * b.x, a.y + s * b.y, a.z + s * b.z, a.w + s * b.w }; };
        planes[0] = add(r3, r0, 1.0f);  // left
        planes[1] = add(r3, r0, -1.0f); // right
        planes[2] = add(r3, r1, 1.0f);  // bottom
        planes[3] = add(r3, r1, -1.0f); // top
        planes[4] = add(r3, r2, -1.0f); // near (reverse-Z: z <= w)
        for (int i = 0; i < 5; ++i)
        {
            const float length = std::sqrt(planes[i].x * planes[i].x + planes[i].y * planes[i].y + planes[i].z * planes[i].z);
            planes[i] = { planes[i].x / length, planes[i].y / length, planes[i].z / length, planes[i].w / length };
        }
    }
}
