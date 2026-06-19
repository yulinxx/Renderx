#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <type_traits>
#include <vector>

namespace Render
{
    template <typename T, size_t N>
    struct Vec
    {
        T data[N]{};

        Vec() = default;

        template <size_t M = N, typename = std::enable_if_t<M == 2>>
        Vec(T x, T y)
        {
            data[0] = x;
            data[1] = y;
        }

        template <size_t M = N, typename = std::enable_if_t<M == 3>>
        Vec(T x, T y, T z)
        {
            data[0] = x;
            data[1] = y;
            data[2] = z;
        }

        T& x() { return data[0]; }
        const T& x() const { return data[0]; }

        template <size_t M = N, typename = std::enable_if_t<(M >= 2)>>
        T& y() { return data[1]; }
        template <size_t M = N, typename = std::enable_if_t<(M >= 2)>>
        const T& y() const { return data[1]; }

        template <size_t M = N, typename = std::enable_if_t<(M >= 3)>>
        T& z() { return data[2]; }
        template <size_t M = N, typename = std::enable_if_t<(M >= 3)>>
        const T& z() const { return data[2]; }

        T& operator[](size_t index) { return data[index]; }
        const T& operator[](size_t index) const { return data[index]; }

        static Vec min(const Vec& a, const Vec& b)
        {
            Vec result;
            for (size_t i = 0; i < N; ++i)
                result.data[i] = std::min(a.data[i], b.data[i]);
            return result;
        }

        static Vec max(const Vec& a, const Vec& b)
        {
            Vec result;
            for (size_t i = 0; i < N; ++i)
                result.data[i] = std::max(a.data[i], b.data[i]);
            return result;
        }
    };

    using Vec2f = Vec<float, 2>;
    using Vec2d = Vec<double, 2>;
    using Vec3f = Vec<float, 3>;

    struct Mat3f
    {
        // Column-major layout, matching OpenGL glUniformMatrix3fv.
        float data[9]{};

        Mat3f()
        {
            data[0] = 1.0f;
            data[4] = 1.0f;
            data[8] = 1.0f;
        }

        explicit Mat3f(float diagonal)
        {
            data[0] = diagonal;
            data[4] = diagonal;
            data[8] = diagonal;
        }

        static Mat3f identity() { return Mat3f(1.0f); }
        static Mat3f zero() { return Mat3f(0.0f); }

        static Mat3f ortho2D(float left, float right, float bottom, float top)
        {
            Mat3f result = identity();
            if (right == left || top == bottom)
                return result;

            result.at(0, 0) = 2.0f / (right - left);
            result.at(1, 1) = 2.0f / (top - bottom);
            result.at(0, 2) = -(right + left) / (right - left);
            result.at(1, 2) = -(top + bottom) / (top - bottom);
            return result;
        }

        float& at(size_t row, size_t col) { return data[col * 3 + row]; }
        const float& at(size_t row, size_t col) const { return data[col * 3 + row]; }
    };

    struct BBox2d
    {
        Vec2d minPt;
        Vec2d maxPt;

        BBox2d()
        {
            reset();
        }

        BBox2d(double minX, double minY, double maxX, double maxY)
            : minPt(std::min(minX, maxX), std::min(minY, maxY))
            , maxPt(std::max(minX, maxX), std::max(minY, maxY))
        {
        }

        BBox2d(const Vec2d& minPoint, const Vec2d& maxPoint)
            : minPt(Vec2d::min(minPoint, maxPoint))
            , maxPt(Vec2d::max(minPoint, maxPoint))
        {
        }

        void reset()
        {
            constexpr double maxVal = std::numeric_limits<double>::max();
            constexpr double minVal = std::numeric_limits<double>::lowest();
            minPt = Vec2d(maxVal, maxVal);
            maxPt = Vec2d(minVal, minVal);
        }

        bool isValid() const
        {
            return minPt.x() <= maxPt.x() && minPt.y() <= maxPt.y();
        }
    };

    struct Color
    {
        float data[4]{ 0.0f, 0.0f, 0.0f, 1.0f };

        Color() = default;
        Color(float red, float green, float blue, float alpha = 1.0f)
            : data{ red, green, blue, alpha }
        {
        }

        static Color fromRGB255(int red, int green, int blue, int alpha = 255)
        {
            auto clampByte = [](int value) {
                return std::max(0, std::min(value, 255));
            };
            return Color(
                static_cast<float>(clampByte(red)) / 255.0f,
                static_cast<float>(clampByte(green)) / 255.0f,
                static_cast<float>(clampByte(blue)) / 255.0f,
                static_cast<float>(clampByte(alpha)) / 255.0f);
        }

        float r() const { return data[0]; }
        float g() const { return data[1]; }
        float b() const { return data[2]; }
        float a() const { return data[3]; }
    };

    enum class RenderPrimitiveType : uint8_t
    {
        Points,
        Lines,
        LineStrip,
        LineLoop,
        Triangles,
        TriangleFan,
    };

    struct RenderVertex
    {
        Vec3f position;
        Vec3f color;
    };

    struct RenderCommand
    {
        RenderPrimitiveType primitiveType = RenderPrimitiveType::Lines;
        std::vector<RenderVertex> vertices;
        bool useVertexColors = false;
        float lineWidth = 1.0f;
        float pointSize = 1.0f;

        size_t vertexCount() const { return vertices.size(); }
        bool isEmpty() const { return vertices.empty(); }
    };

    struct RenderCommandList
    {
        std::vector<RenderCommand> commands;

        bool empty() const { return commands.empty(); }
        size_t size() const { return commands.size(); }
    };

    enum class UiTextCoordMode : uint8_t
    {
        PixelCoords = 0,
        WorldPos_PixelSize = 1,
    };

    enum class UiTextHAlign : uint8_t
    {
        Left = 0,
        Center = 1,
        Right = 2,
    };

    enum class UiTextVAlign : uint8_t
    {
        Top = 0,
        Middle = 1,
        Bottom = 2,
    };

    struct UiTextItem
    {
        std::string text;
        float x = 0.0f;
        float y = 0.0f;
        UiTextCoordMode coordMode = UiTextCoordMode::PixelCoords;
        UiTextHAlign hAlign = UiTextHAlign::Left;
        UiTextVAlign vAlign = UiTextVAlign::Top;
        int fontSize = 10;
        Color color;
        float rotationDeg = 0.0f;
        float zOrder = 0.0f;

        bool hasBackground = false;
        Color bgColor;
        float bgPaddingX = 2.0f;
        float bgPaddingY = 1.0f;
        float bgRadius = 0.0f;

        bool hasLeaderLine = false;
        Color leaderLineColor;
        float leaderLineWidth = 1.0f;
    };

    using UiTextItemList = std::vector<UiTextItem>;

    struct SceneEnvLayer
    {
        std::vector<Vec2f> vertices;
        Color color;
        float zDepth = 0.0f;
        float lineWidth = 1.0f;
        bool asTriangles = false;
        bool usePixelCoords = false;
    };

    struct SceneEnvGeometry
    {
        std::vector<SceneEnvLayer> layers;
        UiTextItemList texts;

        bool empty() const
        {
            return layers.empty() && texts.empty();
        }

        void clear()
        {
            layers.clear();
            texts.clear();
        }
    };

    /// 3D 包围盒（用于视锥裁剪等）
    struct BBox3f
    {
        Vec3f minPt;
        Vec3f maxPt;

        BBox3f()
            : minPt(std::numeric_limits<float>::max(),
                     std::numeric_limits<float>::max(),
                     std::numeric_limits<float>::max())
            , maxPt(std::numeric_limits<float>::lowest(),
                    std::numeric_limits<float>::lowest(),
                    std::numeric_limits<float>::lowest())
        {
        }

        BBox3f(const Vec3f& minPoint, const Vec3f& maxPoint)
            : minPt(minPoint), maxPt(maxPoint)
        {
        }

        bool isValid() const
        {
            return minPt.x() <= maxPt.x() && minPt.y() <= maxPt.y() && minPt.z() <= maxPt.z();
        }
    };
} // namespace Render
