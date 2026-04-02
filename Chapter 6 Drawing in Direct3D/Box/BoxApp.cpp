//***************************************************************************************
// BoxApp.cpp
// Deferred rendering with Sponza + shot point lights (Space = shoot from camera)
//***************************************************************************************

#include "Common/d3dApp.h"
#include "Common/MathHelper.h"
#include "Common/UploadBuffer.h"
#include "Common/GeometryGenerator.h"
#include "Common/DDSTextureLoader.h"
#include "Common/d3dx12.h"

#define TINYOBJLOADER_IMPLEMENTATION
#include "tiny_obj_loader.h"
#include "RenderingSystem.h"

using Microsoft::WRL::ComPtr;
using namespace DirectX;
using namespace DirectX::PackedVector;

struct Vertex
{
    XMFLOAT3 Pos;
    XMFLOAT3 Normal;
    XMFLOAT2 TexC;
};

struct MyTexture
{
    std::string Name;
    std::wstring Filename;
    ComPtr<ID3D12Resource> Resource = nullptr;
    ComPtr<ID3D12Resource> UploadHeap = nullptr;
};

struct RenderItem
{
    std::string SubmeshName;
    int         TexSrvIndex;
    bool        IsStar = false;
};

// -------------------------------------------------------
// Möller–Trumbore ray-triangle intersection (CPU)
// Возвращает true и записывает t (параметр луча), если есть пересечение.
// -------------------------------------------------------
static bool RayTriangleIntersect(
    FXMVECTOR orig, FXMVECTOR dir,
    FXMVECTOR v0, GXMVECTOR v1, HXMVECTOR v2,
    float& t)
{
    const float EPS = 1e-7f;
    XMVECTOR edge1 = v1 - v0;
    XMVECTOR edge2 = v2 - v0;
    XMVECTOR h = XMVector3Cross(dir, edge2);
    float    a = XMVectorGetX(XMVector3Dot(edge1, h));
    if (a > -EPS && a < EPS) return false;
    float    f = 1.0f / a;
    XMVECTOR s = orig - v0;
    float    u = f * XMVectorGetX(XMVector3Dot(s, h));
    if (u < 0.0f || u > 1.0f) return false;
    XMVECTOR q = XMVector3Cross(s, edge1);
    float    v = f * XMVectorGetX(XMVector3Dot(dir, q));
    if (v < 0.0f || u + v > 1.0f) return false;
    t = f * XMVectorGetX(XMVector3Dot(edge2, q));
    return t > EPS;
}

class BoxApp : public D3DApp
{
public:
    BoxApp(HINSTANCE hInstance);
    ~BoxApp();
    virtual bool Initialize() override;

private:
    virtual void OnResize()   override;
    virtual void Update(const GameTimer& gt) override;
    virtual void Draw(const GameTimer& gt)   override;
    virtual void OnMouseDown(WPARAM btnState, int x, int y) override;
    virtual void OnMouseUp(WPARAM btnState, int x, int y) override;
    virtual void OnMouseMove(WPARAM btnState, int x, int y) override;
    // Обработка клавиш
    virtual LRESULT MsgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) override;

    void LoadTextures();
    void BuildDescriptorHeaps();
    void BuildModelGeometry();

    // Создаём SRV на depth buffer в mSrvHeap[slot]
    void BuildDepthSRV();

    // Луч из камеры — CPU пересечение с мешем → AddPointLight
    void ShootLightFromCamera();

private:
    RenderingSystem mRenderingSystem;

    ComPtr<ID3D12DescriptorHeap> mGbufferRtvHeap;
    // mSrvHeap теперь содержит: [0..2] = GBuffer SRVs, [3] = Depth SRV
    ComPtr<ID3D12DescriptorHeap> mSrvHeap;
    ComPtr<ID3D12DescriptorHeap> mObjectSrvHeap;

    // GPU-хэндл depth SRV (слот 3 в mSrvHeap)
    D3D12_GPU_DESCRIPTOR_HANDLE mDepthSrvGpuHandle = {};

    std::vector<RenderItem> mRenderItems;
    XMFLOAT3 mEyePosW = { 0.0f, 0.0f, 0.0f };

    static const UINT mGbufferRtvOffset = 0;
    static const UINT mGbufferSrvOffset = 0;
    // Depth SRV идёт сразу после трёх GBuffer SRVs
    static const UINT mDepthSrvOffset = GBuffer::NumRTs; // = 3

    std::vector<std::unique_ptr<MyTexture>> mAllTextures;
    std::unique_ptr<MeshGeometry> mModelGeo = nullptr;

    // CPU-копия вершин и индексов Sponza для ray casting
    std::vector<XMFLOAT3>    mCpuVertices;
    std::vector<uint32_t>    mCpuIndices;
    // Мировая матрица Sponza (для трансформации вершин при ray cast)
    XMFLOAT4X4 mSponzaWorld = MathHelper::Identity4x4();

    XMFLOAT4X4 mWorld = MathHelper::Identity4x4();
    XMFLOAT4X4 mView = MathHelper::Identity4x4();
    XMFLOAT4X4 mProj = MathHelper::Identity4x4();

    float mTheta = 1.5f * XM_PI;
    float mPhi = XM_PIDIV4;
    float mRadius = 7.0f;
    float mStarRotation = 0.0f;
    POINT mLastMousePos;

    // Выстрелянные источники (хранятся отдельно, чтобы переоткрывать каждый кадр)
    struct ShotLight
    {
        XMFLOAT3 Position;
        XMFLOAT3 Color;
        float    Range;
    };
    std::vector<ShotLight> mShotLights;

    // Счётчик для случайных цветов
    int mShotCount = 0;
};

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE prevInstance, PSTR cmdLine, int showCmd)
{
#if defined(DEBUG) | defined(_DEBUG)
    _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
#endif
    try {
        BoxApp theApp(hInstance);
        if (!theApp.Initialize()) return 0;
        return theApp.Run();
    }
    catch (DxException& e) {
        MessageBox(nullptr, e.ToString().c_str(), L"HR Failed", MB_OK);
        return 0;
    }
}

BoxApp::BoxApp(HINSTANCE hInstance) : D3DApp(hInstance)
{
    mLastMousePos.x = 0;
    mLastMousePos.y = 0;
}

BoxApp::~BoxApp() {}

bool BoxApp::Initialize()
{

    if (!D3DApp::Initialize()) return false;
    ThrowIfFailed(mCommandList->Reset(mDirectCmdListAlloc.Get(), nullptr));

    BuildDescriptorHeaps();
    BuildModelGeometry();

    ThrowIfFailed(mCommandList->Close());
    ID3D12CommandList* cmdsLists[] = { mCommandList.Get() };
    mCommandQueue->ExecuteCommandLists(_countof(cmdsLists), cmdsLists);
    FlushCommandQueue();

    // После инициализации создаём depth SRV (DepthStencilBuffer уже существует)
    BuildDepthSRV();

    return true;
}

// -------------------------------------------------------
// Depth SRV: создаём SRV на mDepthStencilBuffer в слоте mDepthSrvOffset
// Depth format DXGI_FORMAT_D24_UNORM_S8_UINT читается как R24_UNORM_X8_TYPELESS
// -------------------------------------------------------
void BoxApp::BuildDepthSRV()
{
    UINT srvSize = md3dDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    CD3DX12_CPU_DESCRIPTOR_HANDLE cpuHandle(mSrvHeap->GetCPUDescriptorHandleForHeapStart());
    cpuHandle.Offset(mDepthSrvOffset, srvSize);

    CD3DX12_GPU_DESCRIPTOR_HANDLE gpuHandle(mSrvHeap->GetGPUDescriptorHandleForHeapStart());
    gpuHandle.Offset(mDepthSrvOffset, srvSize);
    mDepthSrvGpuHandle = gpuHandle;

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    // D24_UNORM_S8_UINT нельзя использовать напрямую как SRV — используем типизированный псевдоним
    srvDesc.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;
    srvDesc.Texture2D.PlaneSlice = 0;

    md3dDevice->CreateShaderResourceView(mDepthStencilBuffer.Get(), &srvDesc, cpuHandle);
}

void BoxApp::LoadTextures()
{
    tinyobj::ObjReader       reader;
    tinyobj::ObjReaderConfig config;
    config.triangulate = false;

    if (!reader.ParseFromFile("Sponza-master/sponza.obj", config))
        return;

    auto& materials = reader.GetMaterials();
    std::wstring texDir = L"Sponza-master/textures/";

    auto addTex = [&](const std::string& name) -> bool
        {
            if (name.empty()) return false;

            std::string baseName = name;
            size_t dotPos = baseName.rfind('.');
            if (dotPos != std::string::npos)
                baseName = baseName.substr(0, dotPos);

            size_t slashPos = baseName.rfind('/');
            if (slashPos != std::string::npos)
                baseName = baseName.substr(slashPos + 1);
            slashPos = baseName.rfind('\\');
            if (slashPos != std::string::npos)
                baseName = baseName.substr(slashPos + 1);

            for (auto& t : mAllTextures)
                if (t->Name == baseName) return true;

            std::wstring wName(baseName.begin(), baseName.end());
            std::wstring fullPath = texDir + wName + L".dds";

            auto tex = std::make_unique<MyTexture>();
            tex->Name = baseName;
            tex->Filename = fullPath;

            HRESULT hr = DirectX::CreateDDSTextureFromFile12(
                md3dDevice.Get(), mCommandList.Get(),
                tex->Filename.c_str(),
                tex->Resource, tex->UploadHeap);

            if (FAILED(hr)) return false;

            mAllTextures.push_back(std::move(tex));
            return true;
        };

    for (const auto& mat : materials)
        addTex(mat.diffuse_texname);

    if (mAllTextures.empty())
        addTex("default");

    auto addTexDDS = [&](std::wstring path, std::string name) {
        auto tex = std::make_unique<MyTexture>();
        tex->Name = name;
        tex->Filename = path;
        ThrowIfFailed(DirectX::CreateDDSTextureFromFile12(
            md3dDevice.Get(), mCommandList.Get(),
            path.c_str(), tex->Resource, tex->UploadHeap));
        mAllTextures.push_back(std::move(tex));
        };

    addTexDDS(L"models/source/725b3a4da0ef_Tiny_green_starw__3_texture_kd.dds", "star_diffuse");
    addTexDDS(L"models/source/725b3a4da0ef_Tiny_green_starw__3_roughness.dds", "star_roughness");
    addTexDDS(L"models/source/725b3a4da0ef_Tiny_green_starw__3_metallic.dds", "star_metallic");
}

void BoxApp::BuildDescriptorHeaps()
{
    LoadTextures();

    // RTV куча: 3 слота для GBuffer
    D3D12_DESCRIPTOR_HEAP_DESC rtvDesc = {};
    rtvDesc.NumDescriptors = GBuffer::NumRTs;
    rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    ThrowIfFailed(md3dDevice->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&mGbufferRtvHeap)));

    // SRV куча для lighting pass: 3 GBuffer + 1 Depth = 4 слота
    D3D12_DESCRIPTOR_HEAP_DESC srvDesc = {};
    srvDesc.NumDescriptors = GBuffer::NumRTs + 1; // +1 для depth SRV
    srvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ThrowIfFailed(md3dDevice->CreateDescriptorHeap(&srvDesc, IID_PPV_ARGS(&mSrvHeap)));

    // SRV куча для текстур объектов
    D3D12_DESCRIPTOR_HEAP_DESC objSrvDesc = {};
    objSrvDesc.NumDescriptors = 64;
    objSrvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    objSrvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ThrowIfFailed(md3dDevice->CreateDescriptorHeap(&objSrvDesc, IID_PPV_ARGS(&mObjectSrvHeap)));

    mRenderingSystem.Init(
        md3dDevice.Get(), mCommandList.Get(),
        mClientWidth, mClientHeight,
        mBackBufferFormat, mDepthStencilFormat,
        mGbufferRtvHeap.Get(), mSrvHeap.Get(),
        mGbufferRtvOffset, mGbufferSrvOffset
    );

    // Текстуры объектов в mObjectSrvHeap
    UINT srvSize = md3dDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    CD3DX12_CPU_DESCRIPTOR_HANDLE hDesc(mObjectSrvHeap->GetCPUDescriptorHandleForHeapStart());

    for (auto& tex : mAllTextures) {
        D3D12_SHADER_RESOURCE_VIEW_DESC srvD = {};
        srvD.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvD.Format = tex->Resource->GetDesc().Format;
        srvD.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvD.Texture2D.MipLevels = tex->Resource->GetDesc().MipLevels;
        md3dDevice->CreateShaderResourceView(tex->Resource.Get(), &srvD, hDesc);
        hDesc.Offset(1, srvSize);
    }

    // Источники света по умолчанию
    mRenderingSystem.AddDirectionalLight(
        { 0.3f, -1.0f, 0.5f }, { 1.0f, 0.95f, 0.8f }, 1.0f);
    mRenderingSystem.AddPointLight(
        { 0.0f, 2.0f,  0.0f }, { 1.0f, 0.2f, 0.1f }, 3.0f, 8.0f);
    mRenderingSystem.AddPointLight(
        { 5.0f, 2.0f, -3.0f }, { 0.1f, 0.5f, 1.0f }, 2.0f, 6.0f);
    mRenderingSystem.AddSpotLight(
        { 0.0f, 5.0f, 0.0f }, { 0.0f, -1.0f, 0.0f },
        { 1.0f, 1.0f, 0.8f }, 5.0f, 10.0f, 30.0f);
}

void BoxApp::BuildModelGeometry()
{
    tinyobj::ObjReader       reader;
    tinyobj::ObjReaderConfig config;
    config.triangulate = true;

    if (!reader.ParseFromFile("Sponza-master/sponza.obj", config))
    {
        MessageBoxA(nullptr, reader.Error().c_str(), "OBJ Load Error", MB_OK);
        return;
    }

    auto& attrib = reader.GetAttrib();
    auto& shapes = reader.GetShapes();
    auto& materials = reader.GetMaterials();

    std::vector<Vertex>        allVertices;
    std::vector<std::uint32_t> allIndices;

    mModelGeo = std::make_unique<MeshGeometry>();
    mModelGeo->Name = "sponzaGeo";

    for (const auto& shape : shapes)
    {
        UINT indexOffset = (UINT)allIndices.size();
        UINT indexCount = 0;

        int matId = -1;
        if (!shape.mesh.material_ids.empty())
            matId = shape.mesh.material_ids[0];

        for (const auto& index : shape.mesh.indices)
        {
            Vertex v = {};
            v.Pos = {
                attrib.vertices[3 * index.vertex_index + 0],
                attrib.vertices[3 * index.vertex_index + 1],
                attrib.vertices[3 * index.vertex_index + 2]
            };
            if (index.normal_index >= 0)
                v.Normal = {
                    attrib.normals[3 * index.normal_index + 0],
                    attrib.normals[3 * index.normal_index + 1],
                    attrib.normals[3 * index.normal_index + 2]
            };
            if (index.texcoord_index >= 0)
                v.TexC = {
                    attrib.texcoords[2 * index.texcoord_index + 0],
                    1.0f - attrib.texcoords[2 * index.texcoord_index + 1]
            };
            allVertices.push_back(v);
            allIndices.push_back((std::uint32_t)(allVertices.size() - 1));
            ++indexCount;
        }

        SubmeshGeometry submesh;
        submesh.IndexCount = indexCount;
        submesh.StartIndexLocation = indexOffset;
        submesh.BaseVertexLocation = 0;
        mModelGeo->DrawArgs[shape.name] = submesh;

        int texIndex = 0;
        if (matId >= 0 && matId < (int)materials.size())
        {
            const std::string& texName = materials[matId].diffuse_texname;
            for (int i = 0; i < (int)mAllTextures.size(); ++i)
            {
                std::string loaded = mAllTextures[i]->Name;
                if (loaded.find(texName) != std::string::npos ||
                    texName.find(loaded) != std::string::npos)
                {
                    texIndex = i;
                    break;
                }
            }
        }

        RenderItem ri;
        ri.SubmeshName = shape.name;
        ri.TexSrvIndex = texIndex;
        ri.IsStar = false;
        mRenderItems.push_back(ri);
    }

    // Сохраняем CPU-копию только Sponza вершин для ray casting
    // (до добавления звезды)
    mCpuVertices.reserve(allVertices.size());
    for (const auto& v : allVertices)
        mCpuVertices.push_back(v.Pos);
    mCpuIndices = allIndices;

    // --- Загрузка звезды ---
    {
        tinyobj::ObjReader reader2;
        tinyobj::ObjReaderConfig config2;
        config2.triangulate = true;
        reader2.ParseFromFile("models/source/725b3a4da0ef_Tiny_green_starw__3.obj", config2);

        auto& attrib2 = reader2.GetAttrib();
        auto& shapes2 = reader2.GetShapes();

        UINT indexOffset = (UINT)allIndices.size();
        UINT indexCount = 0;

        for (const auto& shape : shapes2)
        {
            for (const auto& index : shape.mesh.indices)
            {
                Vertex v = {};
                v.Pos = { attrib2.vertices[3 * index.vertex_index + 0],
                          attrib2.vertices[3 * index.vertex_index + 1],
                          attrib2.vertices[3 * index.vertex_index + 2] };
                if (index.normal_index >= 0)
                    v.Normal = { attrib2.normals[3 * index.normal_index + 0],
                                 attrib2.normals[3 * index.normal_index + 1],
                                 attrib2.normals[3 * index.normal_index + 2] };
                if (index.texcoord_index >= 0)
                    v.TexC = { attrib2.texcoords[2 * index.texcoord_index + 0],
                               1.0f - attrib2.texcoords[2 * index.texcoord_index + 1] };
                allVertices.push_back(v);
                allIndices.push_back((UINT)(allVertices.size() - 1));
                ++indexCount;
            }
        }

        SubmeshGeometry submesh;
        submesh.IndexCount = indexCount;
        submesh.StartIndexLocation = indexOffset;
        submesh.BaseVertexLocation = 0;
        mModelGeo->DrawArgs["star"] = submesh;

        int texIndex = 0;
        for (int i = 0; i < (int)mAllTextures.size(); ++i)
        {
            if (mAllTextures[i]->Name == "star_diffuse")
            {
                texIndex = i; break;
            }
        }

        RenderItem ri;
        ri.SubmeshName = "star";
        ri.TexSrvIndex = texIndex;
        ri.IsStar = true;
        mRenderItems.push_back(ri);
    }

    // Загрузка в GPU
    const UINT vbSize = (UINT)allVertices.size() * sizeof(Vertex);
    const UINT ibSize = (UINT)allIndices.size() * sizeof(std::uint32_t);

    ThrowIfFailed(D3DCreateBlob(vbSize, &mModelGeo->VertexBufferCPU));
    CopyMemory(mModelGeo->VertexBufferCPU->GetBufferPointer(), allVertices.data(), vbSize);
    ThrowIfFailed(D3DCreateBlob(ibSize, &mModelGeo->IndexBufferCPU));
    CopyMemory(mModelGeo->IndexBufferCPU->GetBufferPointer(), allIndices.data(), ibSize);

    mModelGeo->VertexBufferGPU = d3dUtil::CreateDefaultBuffer(
        md3dDevice.Get(), mCommandList.Get(),
        allVertices.data(), vbSize, mModelGeo->VertexBufferUploader);
    mModelGeo->IndexBufferGPU = d3dUtil::CreateDefaultBuffer(
        md3dDevice.Get(), mCommandList.Get(),
        allIndices.data(), ibSize, mModelGeo->IndexBufferUploader);

    mModelGeo->VertexByteStride = sizeof(Vertex);
    mModelGeo->VertexBufferByteSize = vbSize;
    mModelGeo->IndexFormat = DXGI_FORMAT_R32_UINT;
    mModelGeo->IndexBufferByteSize = ibSize;
}

// -------------------------------------------------------
// Möller–Trumbore ray cast через все треугольники Sponza.
// Вершины трансформируем мировой матрицей Sponza.
// Нажатие SPACE → добавить point light в точку попадания.
// -------------------------------------------------------
void BoxApp::ShootLightFromCamera()
{
    // Берём направление камеры из матрицы вида
    XMMATRIX view = XMLoadFloat4x4(&mView);
    XMMATRIX invView = XMMatrixInverse(nullptr, view);

    XMVECTOR eye = invView.r[3];                        // позиция камеры
    XMVECTOR dir = XMVector3Normalize(invView.r[2]);    // вектор вперёд


    XMMATRIX world = XMLoadFloat4x4(&mSponzaWorld);

    float tMin = FLT_MAX;
    bool  hit = false;

    uint32_t triCount = (uint32_t)mCpuIndices.size() / 3;
    for (uint32_t i = 0; i < triCount; ++i)
    {
        uint32_t i0 = mCpuIndices[3 * i + 0];
        uint32_t i1 = mCpuIndices[3 * i + 1];
        uint32_t i2 = mCpuIndices[3 * i + 2];

        if (i0 >= mCpuVertices.size() || i1 >= mCpuVertices.size() || i2 >= mCpuVertices.size())
            continue;

        XMVECTOR v0 = XMVector3Transform(XMLoadFloat3(&mCpuVertices[i0]), world);
        XMVECTOR v1 = XMVector3Transform(XMLoadFloat3(&mCpuVertices[i1]), world);
        XMVECTOR v2 = XMVector3Transform(XMLoadFloat3(&mCpuVertices[i2]), world);

        // Вычисляем нормаль треугольника
        XMVECTOR edge1 = v1 - v0;
        XMVECTOR edge2 = v2 - v0;
        XMVECTOR normal = XMVector3Normalize(XMVector3Cross(edge1, edge2));

        // Пропускаем треугольники которые смотрят в том же направлении что и луч
        // (back-face culling для ray cast)
        float NdotD = XMVectorGetX(XMVector3Dot(normal, dir));
        if (NdotD >= 0.0f) continue; // треугольник повёрнут от луча

        float t = 0.0f;
        if (RayTriangleIntersect(eye, dir, v0, v1, v2, t))
        {
            if (t < tMin) { tMin = t; hit = true; }
        }
    }

    if (!hit) return;

    XMVECTOR hitPos = eye + dir * tMin;
    XMFLOAT3 pos;
    XMStoreFloat3(&pos, hitPos);

    // Отладка — убери после проверки
    XMFLOAT3 eyeF, dirF;
    XMStoreFloat3(&eyeF, eye);
    XMStoreFloat3(&dirF, dir);
    std::string dbg =
        "Eye: " + std::to_string(eyeF.x) + ", " + std::to_string(eyeF.y) + ", " + std::to_string(eyeF.z) +
        "\nDir: " + std::to_string(dirF.x) + ", " + std::to_string(dirF.y) + ", " + std::to_string(dirF.z) +
        "\ntMin: " + std::to_string(tMin) +
        "\nHit: " + std::to_string(pos.x) + ", " + std::to_string(pos.y) + ", " + std::to_string(pos.z);
    MessageBoxA(nullptr, dbg.c_str(), "Debug", MB_OK);

    static const XMFLOAT3 palette[] = {
        { 1.0f, 0.4f, 0.1f },
        { 0.2f, 0.6f, 1.0f },
        { 0.4f, 1.0f, 0.4f },
        { 1.0f, 0.2f, 0.8f },
        { 1.0f, 1.0f, 0.3f },
        { 0.5f, 0.2f, 1.0f },
    };
    XMFLOAT3 color = palette[mShotCount % _countof(palette)];
    ++mShotCount;

    ShotLight sl;
    sl.Position = pos;
    sl.Color = color;
    sl.Range = 1.0f;
    mShotLights.push_back(sl);
}

LRESULT BoxApp::MsgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_KEYDOWN)
    {
        if (wParam == VK_SPACE)
            ShootLightFromCamera();
        if (wParam == 'R')
        {
            mShotLights.clear();
            mShotCount = 0;
        }
    }
    return D3DApp::MsgProc(hwnd, msg, wParam, lParam);


}



void BoxApp::Update(const GameTimer& gt)
{
    float x = mRadius * sinf(mPhi) * cosf(mTheta);
    float z = mRadius * sinf(mPhi) * sinf(mTheta);
    float y = mRadius * cosf(mPhi);

    mEyePosW = { x, y, z };

    mStarRotation += gt.DeltaTime() * 0.0f;

    XMVECTOR pos = XMVectorSet(x, y, z, 1.0f);
    XMVECTOR target = XMVectorZero();
    XMVECTOR up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
    XMStoreFloat4x4(&mView, XMMatrixLookAtLH(pos, target, up));

    float s = 0.1f;
    XMMATRIX sponzaWorld = XMMatrixScaling(s, s, s);
    XMStoreFloat4x4(&mWorld, sponzaWorld);
    XMStoreFloat4x4(&mSponzaWorld, sponzaWorld); // для ray cast
}

void BoxApp::Draw(const GameTimer& gt)
{
    ThrowIfFailed(mDirectCmdListAlloc->Reset());
    ThrowIfFailed(mCommandList->Reset(mDirectCmdListAlloc.Get(), nullptr));

    mCommandList->RSSetViewports(1, &mScreenViewport);
    mCommandList->RSSetScissorRects(1, &mScissorRect);

    mCommandList->ClearDepthStencilView(
        DepthStencilView(),
        D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL,
        1.0f, 0, 0, nullptr);

    // ---- GEOMETRY PASS ----
    {
        ID3D12DescriptorHeap* heaps[] = { mObjectSrvHeap.Get() };
        mCommandList->SetDescriptorHeaps(_countof(heaps), heaps);
    }

    mRenderingSystem.BeginGeometryPass(mCommandList.Get(), DepthStencilView());

    XMMATRIX world = XMLoadFloat4x4(&mWorld);
    XMMATRIX view = XMLoadFloat4x4(&mView);
    XMMATRIX proj = XMLoadFloat4x4(&mProj);

    GeometryPassConstants geomConsts;
    XMStoreFloat4x4(&geomConsts.WorldViewProj,
        XMMatrixTranspose(world * view * proj));
    XMStoreFloat4x4(&geomConsts.World,
        XMMatrixTranspose(world));
    XMMATRIX wit = XMMatrixTranspose(XMMatrixInverse(nullptr, world));
    XMStoreFloat4x4(&geomConsts.WorldInvTranspose,
        XMMatrixTranspose(wit));
    mRenderingSystem.SetGeometryPassConstants(geomConsts);

    XMMATRIX starWorld = XMMatrixScaling(1.0f, 1.0f, 1.0f) *
        XMMatrixRotationY(mStarRotation) *
        XMMatrixTranslation(3.0f, 10.0f, 0.0f);

    GeometryPassConstants starConsts;
    XMStoreFloat4x4(&starConsts.WorldViewProj,
        XMMatrixTranspose(starWorld * view * proj));
    XMStoreFloat4x4(&starConsts.World,
        XMMatrixTranspose(starWorld));
    XMMATRIX starWit = XMMatrixTranspose(XMMatrixInverse(nullptr, starWorld));
    XMStoreFloat4x4(&starConsts.WorldInvTranspose,
        XMMatrixTranspose(starWit));

    geomConsts.Time = 0.0f;
    starConsts.Time = gt.TotalTime();

    UINT srvSize = md3dDevice->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    mCommandList->IASetVertexBuffers(0, 1, &mModelGeo->VertexBufferView());
    mCommandList->IASetIndexBuffer(&mModelGeo->IndexBufferView());
    mCommandList->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    // Рисуем Sponza
    mRenderingSystem.SetGeometryPassConstants(geomConsts);
    for (const auto& ri : mRenderItems)
    {
        if (ri.IsStar) continue;

        CD3DX12_GPU_DESCRIPTOR_HANDLE texHandle(
            mObjectSrvHeap->GetGPUDescriptorHandleForHeapStart());
        texHandle.Offset(ri.TexSrvIndex, srvSize);
        mCommandList->SetGraphicsRootDescriptorTable(1, texHandle);

        const auto& sub = mModelGeo->DrawArgs[ri.SubmeshName];
        mCommandList->DrawIndexedInstanced(
            sub.IndexCount, 1,
            sub.StartIndexLocation,
            sub.BaseVertexLocation, 0);
    }

    // Рисуем звезду
    mRenderingSystem.SetGeometryPassConstants(starConsts);
    for (const auto& ri : mRenderItems)
    {
        if (!ri.IsStar) continue;

        CD3DX12_GPU_DESCRIPTOR_HANDLE texHandle(
            mObjectSrvHeap->GetGPUDescriptorHandleForHeapStart());
        texHandle.Offset(ri.TexSrvIndex, srvSize);
        mCommandList->SetGraphicsRootDescriptorTable(1, texHandle);

        const auto& sub = mModelGeo->DrawArgs[ri.SubmeshName];
        mCommandList->DrawIndexedInstanced(
            sub.IndexCount, 1,
            sub.StartIndexLocation,
            sub.BaseVertexLocation, 0);
    }

    mRenderingSystem.EndGeometryPass(mCommandList.Get());

    mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(
        mDepthStencilBuffer.Get(),
        D3D12_RESOURCE_STATE_DEPTH_WRITE,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));


    // ---- LIGHTING PASS ----
    {
        ID3D12DescriptorHeap* heaps[] = { mSrvHeap.Get() };
        mCommandList->SetDescriptorHeaps(_countof(heaps), heaps);
    }

    mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(
        CurrentBackBuffer(),
        D3D12_RESOURCE_STATE_PRESENT,
        D3D12_RESOURCE_STATE_RENDER_TARGET));

    mCommandList->ClearRenderTargetView(
        CurrentBackBufferView(), Colors::Black, 0, nullptr);

    // Собираем все источники света: базовые + выстрелянные
    mRenderingSystem.ClearLights();
    // --- базовые ---
    mRenderingSystem.AddDirectionalLight(
        { 0.3f, -1.0f, 0.5f }, { 1.0f, 0.95f, 0.8f }, 1.0f);
    mRenderingSystem.AddPointLight(
        { 0.0f, 2.0f,  0.0f }, { 1.0f, 0.2f, 0.1f }, 3.0f, 8.0f);
    mRenderingSystem.AddPointLight(
        { 5.0f, 2.0f, -3.0f }, { 0.1f, 0.5f, 1.0f }, 2.0f, 6.0f);
    mRenderingSystem.AddSpotLight(
        { 0.0f, 5.0f, 0.0f }, { 0.0f, -1.0f, 0.0f },
        { 1.0f, 1.0f, 0.8f }, 5.0f, 10.0f, 30.0f);
    // --- выстрелянные ---
    for (const auto& sl : mShotLights)
        mRenderingSystem.AddPointLight(sl.Position, sl.Color, 10.0f, sl.Range); 

    // Вычисляем InvViewProj для восстановления world pos из depth в шейдере
// Вычисляем матрицы
    XMMATRIX invView     = XMMatrixInverse(nullptr, view);
    XMMATRIX invProj     = XMMatrixInverse(nullptr, proj);
    XMMATRIX invViewProj = XMMatrixInverse(nullptr, view * proj);

    XMFLOAT4X4 invVP, invV, invP;
    XMStoreFloat4x4(&invVP, XMMatrixTranspose(invViewProj));
    XMStoreFloat4x4(&invV, XMMatrixTranspose(invView));
    XMStoreFloat4x4(&invP, XMMatrixTranspose(invProj));

    mRenderingSystem.DoLightingPass(
        mCommandList.Get(),
        CurrentBackBufferView(),
        DepthStencilView(),
        mEyePosW,
        invVP,
        invV,
        invP,
        mDepthSrvGpuHandle);

    mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(
        mDepthStencilBuffer.Get(),
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
        D3D12_RESOURCE_STATE_DEPTH_WRITE));

    // ---- PRESENT ----
    mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(
        CurrentBackBuffer(),
        D3D12_RESOURCE_STATE_RENDER_TARGET,
        D3D12_RESOURCE_STATE_PRESENT));

    ThrowIfFailed(mCommandList->Close());
    ID3D12CommandList* cmdsLists[] = { mCommandList.Get() };
    mCommandQueue->ExecuteCommandLists(_countof(cmdsLists), cmdsLists);
    ThrowIfFailed(mSwapChain->Present(0, 0));
    mCurrBackBuffer = (mCurrBackBuffer + 1) % SwapChainBufferCount;
    FlushCommandQueue();
}

void BoxApp::OnResize()
{
    D3DApp::OnResize();
    XMStoreFloat4x4(&mProj,
        XMMatrixPerspectiveFovLH(0.25f * MathHelper::Pi, AspectRatio(), 1.0f, 5000.0f));

    if (mGbufferRtvHeap == nullptr) return;
    mRenderingSystem.OnResize(
        md3dDevice.Get(),
        mClientWidth, mClientHeight,
        mGbufferRtvHeap.Get(), mSrvHeap.Get(),
        mGbufferRtvOffset, mGbufferSrvOffset);

    // Пересоздаём depth SRV (depth buffer пересоздаётся при ресайзе)
    BuildDepthSRV();
}

void BoxApp::OnMouseDown(WPARAM btnState, int x, int y)
{
    mLastMousePos.x = x; mLastMousePos.y = y;
    SetCapture(mhMainWnd);
}

void BoxApp::OnMouseUp(WPARAM btnState, int x, int y) { ReleaseCapture(); }

void BoxApp::OnMouseMove(WPARAM btnState, int x, int y)
{
    if ((btnState & MK_LBUTTON) != 0) {
        mTheta += XMConvertToRadians(0.25f * (x - mLastMousePos.x));
        mPhi += XMConvertToRadians(0.25f * (y - mLastMousePos.y));
        mPhi = MathHelper::Clamp(mPhi, 0.1f, MathHelper::Pi - 0.1f);
    }
    else if ((btnState & MK_RBUTTON) != 0) {
        mRadius += 0.005f * (x - mLastMousePos.x) - 0.005f * (y - mLastMousePos.y);
        mRadius = MathHelper::Clamp(mRadius, 1.0f, 150.0f);
    }
    mLastMousePos.x = x; mLastMousePos.y = y;
}