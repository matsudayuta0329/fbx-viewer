#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace viewer {
struct Vec2 { float x{}, y{}; };
struct Vec3 { float x{}, y{}, z{}; };
struct Color { float r{1}, g{1}, b{1}, a{1}; };
Vec3 operator+(Vec3 a, Vec3 b);
Vec3 operator-(Vec3 a, Vec3 b);
Vec3 operator*(Vec3 a, float s);
float dot(Vec3 a, Vec3 b);
Vec3 cross(Vec3 a, Vec3 b);
Vec3 normalized(Vec3 a);
struct Vertex {
    Vec3 position, normal, tangent, bitangent;
    std::array<Vec3, 8> uv{};
    std::array<Color, 8> colors{};
    uint32_t uvMask{}, colorMask{}, mesh{}, localIndex{};
    bool hasTangents{}, generatedNormal{};
};
struct Model {
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<std::string> meshes;
    Vec3 center{}, minimum{}, maximum{};
    float radius{1};
    uint32_t uvMask{}, colorMask{};
};
enum class Mode { Surface, UVScroll, Attribute };
enum class Attribute { Normal, Position, UV, Index, Tangent, Bitangent, Color };
struct Settings {
    Mode mode{Mode::Surface};
    Attribute attribute{Attribute::Normal};
    int attributeChannel{}, uvChannel{};
    bool grayscale{}, showVertices{}, transparent{};
    float pointSize{4}, speed{0.2f}, grid{10};
    Vec2 direction{1, 0};
    Color pointColor{};
};
Model loadModel(std::filesystem::path const& path);
Color vertexColor(Vertex const& v, size_t index, Model const& model, Settings const& settings);
std::wstring describeVertex(Vertex const& v, size_t index, Model const& model);
}
