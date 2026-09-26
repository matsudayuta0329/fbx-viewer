#include "../src/Model.h"
#include "../src/Shaders.h"
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <assimp/Exporter.hpp>
#include <assimp/scene.h>
#include <cmath>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>

using namespace viewer;
void require(bool condition, char const *message) {
    if (!condition)
        throw std::runtime_error(message);
}
bool approximately(float a, float b) {
    return std::abs(a - b) < 0.001f;
}
template <class F> void rejects(F f) {
    bool failed = false;
    try {
        f();
    } catch (std::runtime_error const &) {
        failed = true;
    }
    require(failed, "Invalid input was accepted");
}
void fixture(std::filesystem::path const &path, bool attributes, char const *format = "fbxa",
             bool mirror = false) {
    aiScene scene;
    scene.mRootNode = new aiNode("Root");
    scene.mNumMaterials = 1;
    scene.mMaterials = new aiMaterial *[1]{new aiMaterial};
    scene.mNumMeshes = 1;
    scene.mMeshes = new aiMesh *[1]{new aiMesh};
    auto mesh = scene.mMeshes[0];
    mesh->mName = aiString("Triangle");
    mesh->mPrimitiveTypes = aiPrimitiveType_TRIANGLE;
    mesh->mNumVertices = 3;
    mesh->mVertices = new aiVector3D[3]{{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
    mesh->mNumFaces = 1;
    mesh->mFaces = new aiFace[1];
    mesh->mFaces[0].mNumIndices = 3;
    mesh->mFaces[0].mIndices = new unsigned[3]{0, 1, 2};
    if (attributes) {
        mesh->mNormals = new aiVector3D[3]{{0, 0, 1}, {0, 0, 1}, {0, 0, 1}};
        mesh->mTextureCoords[0] = new aiVector3D[3]{{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
        mesh->mTextureCoords[1] = new aiVector3D[3]{{0.2f, 0.2f, 0}, {0.8f, 0.2f, 0}, {0.2f, 0.8f, 0}};
        mesh->mNumUVComponents[0] = mesh->mNumUVComponents[1] = 2;
        mesh->mColors[0] = new aiColor4D[3]{{1, 0, 0, 1}, {0, 1, 0, 1}, {0, 0, 1, 1}};
    }
    scene.mRootNode->mNumChildren = 1;
    scene.mRootNode->mChildren = new aiNode *[1]{new aiNode("Translated")};
    auto node = scene.mRootNode->mChildren[0];
    node->mParent = scene.mRootNode;
    node->mTransformation.a4 = 2;
    node->mTransformation.b4 = 3;
    node->mTransformation.c4 = 4;
    if (mirror)
        node->mTransformation.a1 = -2;
    node->mNumMeshes = 1;
    node->mMeshes = new unsigned[1]{0};
    Assimp::Exporter exporter;
    auto blob = exporter.ExportToBlob(&scene, format);
    require(blob != nullptr, exporter.GetErrorString());
    std::ofstream file(path, std::ios::binary);
    file.write(static_cast<char const *>(blob->data), static_cast<std::streamsize>(blob->size));
    require(file.good(), "Cannot write fixture");
}
int main() {
    try {
        for (auto stage : {std::pair{"VS", "vs_5_0"}, std::pair{"PS", "ps_5_0"}, std::pair{"GS", "gs_5_0"}}) {
            Microsoft::WRL::ComPtr<ID3DBlob> code, errors;
            auto hr = D3DCompile(shader, sizeof(shader) - 1, nullptr, nullptr, nullptr, stage.first,
                                 stage.second, D3DCOMPILE_ENABLE_STRICTNESS, 0, &code, &errors);
            if (FAILED(hr))
                throw std::runtime_error(errors ? static_cast<char const *>(errors->GetBufferPointer())
                                                : "Shader compilation failed");
        }
        std::filesystem::path directory = L"build/test-data";
        std::filesystem::create_directories(directory);
        auto path = directory / L"日本語モデル.fbx";
        fixture(path, true);
        auto model = loadModel(path);
        require(model.vertices.size() == 3 && model.indices.size() == 3, "Triangle topology mismatch");
        require(approximately(model.minimum.x, 2) && approximately(model.minimum.y, 3) &&
                    approximately(model.minimum.z, 4),
                "Node transform not applied");
        require(model.uvMask == 3 && model.colorMask == 1, "Attribute channels lost");
        require(approximately(model.vertices[0].normal.z, 1), "Normal transform incorrect");
        require(describeVertex(model.vertices[0], 0, model).find(L"TEXCOORD1") != std::wstring::npos,
                "Hover data missing UV1");
        Settings settings;
        settings.attribute = Attribute::Normal;
        auto color = vertexColor(model.vertices[0], 0, model, settings);
        require(approximately(color.r, 0.5f) && approximately(color.b, 1), "Normal color mapping incorrect");
        settings.attribute = Attribute::Position;
        color = vertexColor(model.vertices[0], 0, model, settings);
        require(approximately(color.r, 0) && approximately(color.b, 0.5f),
                "Position range mapping incorrect");
        settings.attribute = Attribute::Index;
        color = vertexColor(model.vertices[2], 2, model, settings);
        require(approximately(color.r, 1), "Index mapping incorrect");
        settings.attribute = Attribute::UV;
        settings.attributeChannel = 7;
        color = vertexColor(model.vertices[0], 0, model, settings);
        require(approximately(color.r, 1) && approximately(color.g, 0) && approximately(color.b, 1),
                "Missing attribute marker incorrect");
        settings.attribute = Attribute::Color;
        settings.attributeChannel = 0;
        settings.grayscale = true;
        color = vertexColor(model.vertices[0], 0, model, settings);
        require(approximately(color.r, color.g) && approximately(color.g, color.b),
                "Grayscale mapping incorrect");
        auto missing = directory / L"no-attributes.fbx";
        fixture(missing, false);
        auto bare = loadModel(missing);
        require(bare.uvMask == 0 && bare.colorMask == 0, "Invented missing channels");
        require(approximately(bare.vertices[0].normal.z, 1), "Generated normal incorrect");
        auto binary = directory / L"binary.fbx";
        fixture(binary, true, "fbx");
        auto binaryModel = loadModel(binary);
        require(binaryModel.indices.size() == 3 && binaryModel.uvMask == 3, "Binary FBX failed");
        auto mirrored = directory / L"mirrored.fbx";
        fixture(mirrored, true, "fbxa", true);
        auto flipped = loadModel(mirrored);
        require(approximately(flipped.minimum.x, 0) && approximately(flipped.maximum.x, 2),
                "Negative scale bounds incorrect");
        auto a = flipped.vertices[flipped.indices[0]], b = flipped.vertices[flipped.indices[1]],
             c = flipped.vertices[flipped.indices[2]];
        require(dot(normalized(cross(b.position - a.position, c.position - a.position)), a.normal) > 0.99f,
                "Mirrored winding does not match normals");
        auto broken = directory / L"broken.fbx";
        std::ofstream(broken) << "not an FBX";
        rejects([&] { loadModel(broken); });
        auto empty = directory / L"empty.fbx";
        std::ofstream{empty};
        rejects([&] { loadModel(empty); });
        rejects([&] { loadModel(directory / L"missing.fbx"); });
        rejects([&] { loadModel(directory / L"wrong.obj"); });
        std::cout << "PASS: ASCII/binary FBX, Unicode path, hierarchy transform, UV channels, color, "
                     "generated normals, attribute mapping, and invalid input\n";
        return 0;
    } catch (std::exception const &e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
