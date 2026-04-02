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
    return t > 0.001f;
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
    virtual LRESULT MsgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) override;

    void LoadTextures();
    void BuildDescriptorHeaps();
    void BuildModelGeometry();
    void BuildDepthSRV();
    void ShootLightFromCamera();

private:
    RenderingSystem mRenderingSystem;

    ComPtr<ID3D12DescriptorHeap> mGbufferRtvHeap;
    ComPtr<ID3D12DescriptorHeap> mSrvHeap;
    ComPtr<ID3D12DescriptorHeap> mObjectSrvHeap;

    D3D12_GPU_DESCRIPTOR_HANDLE mDepthSrvGpuHandle = {};

    std::vector<RenderItem> mRenderItems;
    XMFLOAT3 mEyePosW = { 0.0f, 0.0f, 0.0f };

    static const UINT mGbufferRtvOffset = 0;
    static const UINT mGbufferSrvOffset = 0;
    static const UINT mDepthSrvOffset = GBuffer::NumRTs; 

    std::vector<std::unique_ptr<MyTexture>> mAllTextures;
    std::unique_ptr<MeshGeometry> mModelGeo = nullptr;

    std::vector<XMFLOAT3>    mCpuVertices;
    std::vector<uint32_t>    mCpuIndices;
    XMFLOAT4X4 mSponzaWorld = MathHelper::Identity4x4();

    XMFLOAT4X4 mWorld = MathHelper::Identity4x4();
    XMFLOAT4X4 mView = MathHelper::Identity4x4();
    XMFLOAT4X4 mProj = MathHelper::Identity4x4();

    float mTheta = 1.5f * XM_PI;
    float mPhi = XM_PIDIV4;
    float mRadius = 7.0f;
    float mStarRotation = 0.0f;
    POINT mLastMousePos;

    struct ShotLight
    {
        XMFLOAT3 Origin;       
        XMFLOAT3 Direction;    
        XMFLOAT3 Position;     
        XMFLOAT3 Velocity;     
        XMFLOAT3 Color;
        float    Range;
        float    TargetT;      
        float    CurrentT;     
        bool     IsFlying;     
    };

    std::vector<ShotLight> mShotLights;
    const float mLightSpeed = 150.0f;
    int mShotCount = 0;
    bool mShootRequested = false;
    static const size_t mMaxShotLights = 48;
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

    BuildDepthSRV();
    return true;
}

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

    D3D12_DESCRIPTOR_HEAP_DESC rtvDesc = {};
    rtvDesc.NumDescriptors = GBuffer::NumRTs;
    rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    ThrowIfFailed(md3dDevice->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&mGbufferRtvHeap)));

    D3D12_DESCRIPTOR_HEAP_DESC srvDesc = {};
    srvDesc.NumDescriptors = GBuffer::NumRTs + 1;
    srvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ThrowIfFailed(md3dDevice->CreateDescriptorHeap(&srvDesc, IID_PPV_ARGS(&mSrvHeap)));

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
            else
                v.Normal = { 0.0f, 1.0f, 0.0f };
            if (index.texcoord_index >= 0)
                v.TexC = {
                    attrib.texcoords[2 * index.texcoord_index + 0],
                    1.0f - attrib.texcoords[2 * index.texcoord_index + 1]
            };
            else
                v.TexC = { 0.0f, 0.0f }; 
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

    mCpuVertices.reserve(allVertices.size());
    for (const auto& v : allVertices)
        mCpuVertices.push_back(v.Pos);
    mCpuIndices = allIndices;

    // Загрузка звезды
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
            if (mAllTextures[i]->Name == "star_diffuse") { texIndex = i; break; }

        RenderItem ri;
        ri.SubmeshName = "star";
        ri.TexSrvIndex = texIndex;
        ri.IsStar = true;
        mRenderItems.push_back(ri);
    }

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

void BoxApp::ShootLightFromCamera()
{
    XMVECTOR eye = XMLoadFloat3(&mEyePosW);
    XMVECTOR dir = XMVector3Normalize(XMVectorNegate(eye));

    const float kStartOffset = 0.12f;
    XMVECTOR rayOrigin = eye + dir * kStartOffset;

    XMMATRIX world = XMLoadFloat4x4(&mSponzaWorld);

    float tMin = FLT_MAX;
    bool hit = false;
    const float kMinHitDistance = 0.001f; 

    //CPU Raycast 
    uint32_t triCount = (uint32_t)mCpuIndices.size() / 3;
    for (uint32_t i = 0; i < triCount; ++i)
    {
        XMVECTOR v0 = XMVector3Transform(XMLoadFloat3(&mCpuVertices[mCpuIndices[3 * i + 0]]), world);
        XMVECTOR v1 = XMVector3Transform(XMLoadFloat3(&mCpuVertices[mCpuIndices[3 * i + 1]]), world);
        XMVECTOR v2 = XMVector3Transform(XMLoadFloat3(&mCpuVertices[mCpuIndices[3 * i + 2]]), world);

        float t = 0.0f;
        if (RayTriangleIntersect(rayOrigin, dir, v0, v1, v2, t) && t > kMinHitDistance)
        {
            if (t < tMin) { tMin = t; hit = true; }
        }
    }

    if (!hit) tMin = 60.0f; 

    ShotLight sl;
    XMStoreFloat3(&sl.Origin, rayOrigin);
    XMStoreFloat3(&sl.Direction, dir);
    XMStoreFloat3(&sl.Position, rayOrigin);
    XMStoreFloat3(&sl.Velocity, dir * mLightSpeed);

    static const XMFLOAT3 palette[] = {
        { 1.0f, 0.4f, 0.1f }, { 0.2f, 0.6f, 1.0f }, { 0.4f, 1.0f, 0.4f },
        { 1.0f, 0.2f, 0.8f }, { 1.0f, 1.0f, 0.3f }, { 0.5f, 0.2f, 1.0f }
    };
    sl.Color = palette[mShotCount % 6];
    sl.Range = 10.0f;    
    sl.TargetT = tMin;   
    sl.CurrentT = 0.0f;
    sl.IsFlying = true;

    mShotLights.push_back(sl);
    if (mShotLights.size() > mMaxShotLights)
        mShotLights.erase(mShotLights.begin());
    mShotCount++;
}

LRESULT BoxApp::MsgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_KEYDOWN)
    {
        if (wParam == VK_SPACE && ((lParam & 0x40000000) == 0))
            mShootRequested = true;
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

    XMVECTOR pos = XMVectorSet(x, y, z, 1.0f);
    XMVECTOR target = XMVectorZero();
    XMVECTOR up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
    XMStoreFloat4x4(&mView, XMMatrixLookAtLH(pos, target, up));

    float s = 1.0f;
    XMMATRIX sponzaWorld = XMMatrixScaling(s, s, s);
    XMStoreFloat4x4(&mWorld, sponzaWorld);
    mSponzaWorld = mWorld; 

    if (mShootRequested)
    {
        ShootLightFromCamera();
        mShootRequested = false;
    }

    // 2. Обновляем полет света
    const float kMarkerRadius = 50.8f;   
    const float kSurfaceBias = 0.03f;    
    for (auto& sl : mShotLights) {
        if (sl.IsFlying) {
            float dt = gt.DeltaTime();
            float stepDist = mLightSpeed * dt;

            float triggerT = sl.TargetT - kMarkerRadius;
            if (triggerT < 0.0f) triggerT = 0.0f;
            if (sl.CurrentT + stepDist >= triggerT) {
                sl.IsFlying = false;
                sl.Range = 28.0f; 

                XMVECTOR o = XMLoadFloat3(&sl.Origin);
                XMVECTOR d = XMLoadFloat3(&sl.Direction);
                float placeT = sl.TargetT - kSurfaceBias;
                if (placeT < 0.0f) placeT = 0.0f;
                XMStoreFloat3(&sl.Position, o + d * placeT);
                sl.Velocity = { 0.0f, 0.0f, 0.0f };
                sl.CurrentT = sl.TargetT;
            }
            else {
                sl.CurrentT += stepDist;
                XMVECTOR p = XMLoadFloat3(&sl.Position);
                XMVECTOR v = XMLoadFloat3(&sl.Velocity);
                XMStoreFloat3(&sl.Position, p + v * dt);
            }
        }
    }
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
    geomConsts.Time = 0.0f;

    UINT srvSize = md3dDevice->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    mCommandList->IASetVertexBuffers(0, 1, &mModelGeo->VertexBufferView());
    mCommandList->IASetIndexBuffer(&mModelGeo->IndexBufferView());
    mCommandList->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    UINT geomCbIndex = 0;
    mRenderingSystem.SetGeometryPassConstants(mCommandList.Get(), geomConsts, geomCbIndex++);
    for (const auto& ri : mRenderItems)
    {
        if (ri.IsStar) continue;
        CD3DX12_GPU_DESCRIPTOR_HANDLE texHandle(
            mObjectSrvHeap->GetGPUDescriptorHandleForHeapStart());
        texHandle.Offset(ri.TexSrvIndex, srvSize);
        mCommandList->SetGraphicsRootDescriptorTable(1, texHandle);
        const auto& sub = mModelGeo->DrawArgs[ri.SubmeshName];
        mCommandList->DrawIndexedInstanced(
            sub.IndexCount, 1, sub.StartIndexLocation, sub.BaseVertexLocation, 0);
    }

    //маркер полёта
    for (const auto& sl : mShotLights)
    {
        XMMATRIX shotWorld =
            XMMatrixScaling(0.12f, 0.12f, 0.12f) *
            XMMatrixRotationY(gt.TotalTime() * 2.0f) *
            XMMatrixTranslation(sl.Position.x, sl.Position.y, sl.Position.z);

        GeometryPassConstants shotConsts;
        XMStoreFloat4x4(&shotConsts.WorldViewProj,
            XMMatrixTranspose(shotWorld * view * proj));
        XMStoreFloat4x4(&shotConsts.World,
            XMMatrixTranspose(shotWorld));
        XMMATRIX shotWit = XMMatrixTranspose(XMMatrixInverse(nullptr, shotWorld));
        XMStoreFloat4x4(&shotConsts.WorldInvTranspose,
            XMMatrixTranspose(shotWit));
        shotConsts.Time = gt.TotalTime();

        mRenderingSystem.SetGeometryPassConstants(mCommandList.Get(), shotConsts, geomCbIndex++);

        for (const auto& ri : mRenderItems)
        {
            if (!ri.IsStar) continue;
            CD3DX12_GPU_DESCRIPTOR_HANDLE texHandle(
                mObjectSrvHeap->GetGPUDescriptorHandleForHeapStart());
            texHandle.Offset(ri.TexSrvIndex, srvSize);
            mCommandList->SetGraphicsRootDescriptorTable(1, texHandle);
            const auto& sub = mModelGeo->DrawArgs[ri.SubmeshName];
            mCommandList->DrawIndexedInstanced(
                sub.IndexCount, 1, sub.StartIndexLocation, sub.BaseVertexLocation, 0);
        }
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

    mRenderingSystem.ClearLights();
    mRenderingSystem.AddDirectionalLight(
        { 0.3f, -1.0f, 0.5f }, { 1.0f, 0.95f, 0.8f }, 1.0f);
    mRenderingSystem.AddPointLight(
        { 0.0f, 2.0f,  0.0f }, { 1.0f, 0.2f, 0.1f }, 3.0f, 8.0f);
    mRenderingSystem.AddPointLight(
        { 5.0f, 2.0f, -3.0f }, { 0.1f, 0.5f, 1.0f }, 2.0f, 6.0f);
    mRenderingSystem.AddSpotLight(
        { 0.0f, 5.0f, 0.0f }, { 0.0f, -1.0f, 0.0f },
        { 1.0f, 1.0f, 0.8f }, 5.0f, 10.0f, 30.0f);


    XMMATRIX invView = XMMatrixInverse(nullptr, view);
    XMMATRIX invProj = XMMatrixInverse(nullptr, proj);
    XMMATRIX invViewProj = XMMatrixInverse(nullptr, view * proj);

    XMFLOAT4X4 ivp, iv, ip;
    XMStoreFloat4x4(&ivp, XMMatrixTranspose(invViewProj));
    XMStoreFloat4x4(&iv, XMMatrixTranspose(invView));
    XMStoreFloat4x4(&ip, XMMatrixTranspose(invProj));

    const int kBaseLights = 4;
    int shotBudget = kMaxLights - kBaseLights;
    if (shotBudget < 0) shotBudget = 0;

    for (int i = (int)mShotLights.size() - 1; i >= 0 && shotBudget > 0; --i)
    {
        const auto& sl = mShotLights[i];
        if (!sl.IsFlying) continue;
        float intensity = 36.0f;
        mRenderingSystem.AddPointLight(sl.Position, sl.Color, intensity, 30.0f);
        --shotBudget;
    }

    for (int i = (int)mShotLights.size() - 1; i >= 0 && shotBudget > 0; --i)
    {
        const auto& sl = mShotLights[i];
        if (sl.IsFlying) continue;
        float intensity = 35.0f;
        mRenderingSystem.AddPointLight(sl.Position, sl.Color, intensity, sl.Range);
        --shotBudget;
    }

    mRenderingSystem.DoLightingPass(
        mCommandList.Get(), CurrentBackBufferView(), DepthStencilView(),
        mEyePosW, ivp, iv, ip, mDepthSrvGpuHandle);

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