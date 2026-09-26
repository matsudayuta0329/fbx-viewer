#include "Model.h"
#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>
#include <algorithm>
#include <cmath>
#include <cwctype>
#include <fstream>
#include <functional>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace viewer {
Vec3 operator+(Vec3 a, Vec3 b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}
Vec3 operator-(Vec3 a, Vec3 b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}
Vec3 operator*(Vec3 a, float s) {
    return {a.x * s, a.y * s, a.z * s};
}
float dot(Vec3 a, Vec3 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
Vec3 cross(Vec3 a, Vec3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
Vec3 normalized(Vec3 a) {
    float l = std::sqrt(dot(a, a));
    return l > 1e-12f ? a * (1 / l) : Vec3{0, 1, 0};
}
static Vec3 convert(aiVector3D a) {
    return {a.x, a.y, a.z};
}
Model loadModel(std::filesystem::path const &path) {
    auto ext = path.extension().wstring();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    if (ext != L".fbx")
        throw std::runtime_error("Only .fbx files are supported.");
    // Read through a wide filesystem path so Japanese filenames work on Windows.
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file)
        throw std::runtime_error("Cannot open the file.");
    auto length = file.tellg();
    if (length <= 0 || length > 512LL * 1024 * 1024)
        throw std::runtime_error("File is empty or exceeds the 512 MiB limit.");
    std::vector<char> bytes(static_cast<size_t>(length));
    file.seekg(0);
    file.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!file)
        throw std::runtime_error("Could not read the complete file.");
    Assimp::Importer importer;
    auto scene = importer.ReadFileFromMemory(bytes.data(), bytes.size(),
                                             aiProcess_Triangulate | aiProcess_ValidateDataStructure, "fbx");
    if (!scene || !scene->mRootNode)
        throw std::runtime_error(importer.GetErrorString());
    Model result;
    std::function<void(aiNode const *, aiMatrix4x4)> visit;
    visit = [&](aiNode const *node, aiMatrix4x4 parent) {
        auto transform = parent * node->mTransformation;
        aiMatrix3x3 linear(transform), normalMatrix(linear);
        normalMatrix.Inverse().Transpose();
        bool mirrored = linear.Determinant() < 0;
        for (unsigned n = 0; n < node->mNumMeshes; ++n) {
            auto mesh = scene->mMeshes[node->mMeshes[n]];
            if (!mesh->HasPositions() || !mesh->HasFaces())
                continue;
            if (result.vertices.size() + mesh->mNumVertices > 5000000)
                throw std::runtime_error("Model exceeds the 5 million vertex limit.");
            uint32_t base = static_cast<uint32_t>(result.vertices.size());
            uint32_t meshIndex = static_cast<uint32_t>(result.meshes.size());
            result.meshes.emplace_back(mesh->mName.C_Str());
            for (unsigned i = 0; i < mesh->mNumVertices; ++i) {
                Vertex v;
                v.position = convert(transform * mesh->mVertices[i]);
                v.mesh = meshIndex;
                v.localIndex = i;
                if (!std::isfinite(v.position.x) || !std::isfinite(v.position.y) ||
                    !std::isfinite(v.position.z))
                    throw std::runtime_error("Model contains non-finite positions.");
                v.generatedNormal = !mesh->HasNormals();
                if (mesh->HasNormals())
                    v.normal = normalized(convert(normalMatrix * mesh->mNormals[i]));
                v.hasTangents = mesh->HasTangentsAndBitangents();
                if (v.hasTangents) {
                    v.tangent = normalized(convert(linear * mesh->mTangents[i]));
                    v.bitangent = normalized(convert(linear * mesh->mBitangents[i]));
                }
                for (unsigned c = 0; c < 8; ++c) {
                    if (mesh->HasTextureCoords(c)) {
                        v.uvMask |= 1u << c;
                        v.uv[c] = convert(mesh->mTextureCoords[c][i]);
                    }
                    if (mesh->HasVertexColors(c)) {
                        v.colorMask |= 1u << c;
                        auto a = mesh->mColors[c][i];
                        v.colors[c] = {a.r, a.g, a.b, a.a};
                    }
                }
                result.uvMask |= v.uvMask;
                result.colorMask |= v.colorMask;
                result.vertices.push_back(v);
            }
            for (unsigned f = 0; f < mesh->mNumFaces; ++f) {
                auto const &face = mesh->mFaces[f];
                if (face.mNumIndices != 3)
                    continue;
                uint32_t a = base + face.mIndices[0], b = base + face.mIndices[mirrored ? 2 : 1],
                         c = base + face.mIndices[mirrored ? 1 : 2];
                result.indices.insert(result.indices.end(), {a, b, c});
                if (!mesh->HasNormals()) {
                    auto normal = cross(result.vertices[b].position - result.vertices[a].position,
                                        result.vertices[c].position - result.vertices[a].position);
                    for (auto index : {a, b, c})
                        result.vertices[index].normal = result.vertices[index].normal + normal;
                }
            }
            if (!mesh->HasNormals())
                for (size_t i = base; i < result.vertices.size(); ++i)
                    result.vertices[i].normal = normalized(result.vertices[i].normal);
        }
        for (unsigned i = 0; i < node->mNumChildren; ++i)
            visit(node->mChildren[i], transform);
    };
    visit(scene->mRootNode, aiMatrix4x4{});
    if (result.indices.empty())
        throw std::runtime_error("No triangle meshes were found in the FBX.");
    result.minimum = result.maximum = result.vertices.front().position;
    for (auto const &v : result.vertices) {
        result.minimum = {std::min(result.minimum.x, v.position.x), std::min(result.minimum.y, v.position.y),
                          std::min(result.minimum.z, v.position.z)};
        result.maximum = {std::max(result.maximum.x, v.position.x), std::max(result.maximum.y, v.position.y),
                          std::max(result.maximum.z, v.position.z)};
    }
    result.center = result.minimum + (result.maximum - result.minimum) * 0.5f;
    result.radius = 0.001f;
    for (auto const &v : result.vertices)
        result.radius =
            std::max(result.radius, std::sqrt(dot(v.position - result.center, v.position - result.center)));
    if (!std::isfinite(result.radius))
        throw std::runtime_error("Model bounds exceed supported numeric range.");
    return result;
}
Color vertexColor(Vertex const &v, size_t index, Model const &model, Settings const &s) {
    Vec3 value{};
    switch (s.attribute) {
    case Attribute::Normal:
        value = v.normal * 0.5f + Vec3{0.5f, 0.5f, 0.5f};
        break;
    case Attribute::Position: {
        auto extent = model.maximum - model.minimum;
        auto p = v.position - model.minimum;
        value = {extent.x > 0 ? p.x / extent.x : 0.5f, extent.y > 0 ? p.y / extent.y : 0.5f,
                 extent.z > 0 ? p.z / extent.z : 0.5f};
        break;
    }
    case Attribute::UV:
        if (!(v.uvMask & (1u << s.attributeChannel)))
            return {1, 0, 1, 1};
        value = v.uv[s.attributeChannel];
        break;
    case Attribute::Index: {
        float t =
            static_cast<float>(index) / static_cast<float>(std::max(size_t{1}, model.vertices.size() - 1));
        value = {t, t, t};
        break;
    }
    case Attribute::Tangent:
    case Attribute::Bitangent:
        if (!v.hasTangents)
            return {1, 0, 1, 1};
        value = (s.attribute == Attribute::Tangent ? v.tangent : v.bitangent) * 0.5f + Vec3{0.5f, 0.5f, 0.5f};
        break;
    case Attribute::Color:
        if (!(v.colorMask & (1u << s.attributeChannel)))
            return {1, 0, 1, 1};
        {
            auto c = v.colors[s.attributeChannel];
            value = {c.r, c.g, c.b};
        }
        break;
    }
    value = {std::clamp(value.x, 0.f, 1.f), std::clamp(value.y, 0.f, 1.f), std::clamp(value.z, 0.f, 1.f)};
    if (s.grayscale) {
        float l = dot(value, {0.2126f, 0.7152f, 0.0722f});
        return {l, l, l, 1};
    }
    return {value.x, value.y, value.z, 1};
}
std::wstring describeVertex(Vertex const &v, size_t index, Model const &model) {
    std::wostringstream out;
    out << std::fixed << std::setprecision(5);
    out << L"Vertex " << index << L" / mesh " << v.mesh << L" / local " << v.localIndex;
    auto vec = [&](wchar_t const *name, Vec3 a) {
        out << L'\n' << name << L": " << a.x << L", " << a.y << L", " << a.z;
    };
    vec(L"Position (world)", v.position);
    vec(v.generatedNormal ? L"Normal (generated)" : L"Normal", v.normal);
    if (v.hasTangents) {
        vec(L"Tangent", v.tangent);
        vec(L"Bitangent", v.bitangent);
    } else
        out << L"\nTangent / Bitangent: N/A";
    for (unsigned c = 0; c < 8; ++c)
        if (v.uvMask & (1u << c)) {
            auto name = L"TEXCOORD" + std::to_wstring(c);
            vec(name.c_str(), v.uv[c]);
        }
    if (!v.uvMask)
        out << L"\nTEXCOORD: N/A";
    for (unsigned c = 0; c < 8; ++c)
        if (v.colorMask & (1u << c)) {
            auto a = v.colors[c];
            out << L"\nCOLOR" << c << L": " << a.r << L", " << a.g << L", " << a.b << L", " << a.a;
        }
    if (!v.colorMask)
        out << L"\nCOLOR: N/A";
    (void)model;
    return out.str();
}
} // namespace viewer
