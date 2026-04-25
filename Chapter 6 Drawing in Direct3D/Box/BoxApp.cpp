

//***************************************************************************************
// BoxApp.cpp
// Deferred rendering with Sponza + tessellation (displacement + normal map + distance LOD)
//***************************************************************************************
#include "Common/d3dApp.h"
#include "Common/MathHelper.h"
#include "Common/UploadBuffer.h"
#include "Common/GeometryGenerator.h"
#include "Common/DDSTextureLoader.h"
#include "Common/d3dx12.h"
#include <Windows.h>
#define TINYOBJLOADER_IMPLEMENTATION
#include "tiny_obj_loader.h"
#include "RenderingSystem.h"
#include <DirectXCollision.h>
using Microsoft::WRL::ComPtr;
using namespace DirectX;
using namespace DirectX::PackedVector;


struct Vertex
{
    XMFLOAT3 Pos;
    XMFLOAT3 Normal;
    XMFLOAT2 TexC;
    XMFLOAT3 Tangent; 
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
    int         NormalSrvIndex = -1;
    int         DisplaceSrvIndex = -1;
    bool        IsStar = false;
    bool        UseTess = false;

    XMFLOAT4X4  World = MathHelper::Identity4x4();
    BoundingBox Bounds;                           
    bool        IsVisible = true;                 
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
    D3D12_GPU_DESCRIPTOR_HANDLE  mDepthSrvGpuHandle = {};

    std::vector<RenderItem> mRenderItems;

    XMFLOAT3 mEyePosW = { 0.0f, 0.0f, 0.0f };
    XMFLOAT3 mCurrCameraPos = { 0.0f, 2.0f, -15.0f }; // Начальная позиция

    static const UINT mGbufferRtvOffset = 0;
    static const UINT mGbufferSrvOffset = 0;
    static const UINT mDepthSrvOffset = GBuffer::NumRTs;

    std::vector<std::unique_ptr<MyTexture>> mAllTextures;
    std::unique_ptr<MeshGeometry>           mModelGeo = nullptr;

    std::vector<XMFLOAT3>  mCpuVertices;
    std::vector<uint32_t>  mCpuIndices;

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
    const float  mLightSpeed = 150.0f;
    int          mShotCount = 0;
    bool         mShootRequested = false;
    static const size_t mMaxShotLights = 48;


    int mTessObjBaseSrvIndex = -1; 

    float mTessDisplaceScale = 0.04f; 
    float mTessMinTessDist = 2.0f;
    float mTessMaxTessDist = 40.0f;
    float mTessMinTess = 1.0f;
    float mTessMaxTess = 16.0f;
    float mTessWorldScale = 1.22f;   
  
    XMFLOAT3 mTessWorldOffset = { 0.0f, 0.12f, 1.75f };

    enum class CullingMode { None = 0, BruteForce = 1, Octree = 2 };
    CullingMode mCullingMode = CullingMode::None;

    std::vector<RenderItem> mInstancedItems; 

    struct OctreeNode {
        BoundingBox Box;
        std::vector<int> ItemIndices;
        std::unique_ptr<OctreeNode> Children[8];
        bool IsLeaf = true;
    };
    std::unique_ptr<OctreeNode> mRootNode;

    // Прототипы новых методов
    void BuildInstancedItems();
    void BuildOctree(OctreeNode* node, int depth);
    void GetVisibleItemsOctree(OctreeNode* node, const BoundingFrustum& frustum);
};


int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE prevInstance,
    PSTR cmdLine, int showCmd)
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
    BuildInstancedItems();
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
            if (dotPos != std::string::npos) baseName = baseName.substr(0, dotPos);
            size_t slashPos = baseName.rfind('/');
            if (slashPos != std::string::npos) baseName = baseName.substr(slashPos + 1);
            slashPos = baseName.rfind('\\');
            if (slashPos != std::string::npos) baseName = baseName.substr(slashPos + 1);
            for (auto& t : mAllTextures)
                if (t->Name == baseName) return true;
            std::wstring wName(baseName.begin(), baseName.end());
            std::wstring fullPath = texDir + wName + L".dds";
            auto tex = std::make_unique<MyTexture>();
            tex->Name = baseName;
            tex->Filename = fullPath;
            HRESULT hr = DirectX::CreateDDSTextureFromFile12(
                md3dDevice.Get(), mCommandList.Get(),
                tex->Filename.c_str(), tex->Resource, tex->UploadHeap);
            if (FAILED(hr)) return false;
            mAllTextures.push_back(std::move(tex));
            return true;
        };

    for (const auto& mat : materials)
        addTex(mat.diffuse_texname);
    if (mAllTextures.empty())
        addTex("default");

    auto addTexDDS = [&](std::wstring path, std::string name)
        {
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


    addTexDDS(L"models/source/convertio.in_albedo.dds", "tess_diffuse");     // t0
    addTexDDS(L"models/source/convertio.in_normal.dds", "tess_normal");      // t1
    addTexDDS(L"models/source/convertio.in_displacement.dds", "tess_displacement"); // t2

    addTexDDS(L"models/source/CC556105.dds", "human_sprite");
}


void BoxApp::BuildDescriptorHeaps()
{
    LoadTextures();

    // RTV heap для G-buffer
    D3D12_DESCRIPTOR_HEAP_DESC rtvDesc = {};
    rtvDesc.NumDescriptors = GBuffer::NumRTs;
    rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    ThrowIfFailed(md3dDevice->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&mGbufferRtvHeap)));

    // SRV heap для G-buffer + depth
    D3D12_DESCRIPTOR_HEAP_DESC srvDesc = {};
    srvDesc.NumDescriptors = GBuffer::NumRTs + 1;
    srvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ThrowIfFailed(md3dDevice->CreateDescriptorHeap(&srvDesc, IID_PPV_ARGS(&mSrvHeap)));

    // SRV heap для объектных текстур (увеличили до 128 — хватит с запасом)
    D3D12_DESCRIPTOR_HEAP_DESC objSrvDesc = {};
    objSrvDesc.NumDescriptors = 128;
    objSrvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    objSrvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ThrowIfFailed(md3dDevice->CreateDescriptorHeap(&objSrvDesc, IID_PPV_ARGS(&mObjectSrvHeap)));

    mRenderingSystem.Init(
        md3dDevice.Get(), mCommandList.Get(),
        mClientWidth, mClientHeight,
        mBackBufferFormat, mDepthStencilFormat,
        mGbufferRtvHeap.Get(), mSrvHeap.Get(),
        mGbufferRtvOffset, mGbufferSrvOffset);

    UINT srvSize = md3dDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    CD3DX12_CPU_DESCRIPTOR_HANDLE hDesc(mObjectSrvHeap->GetCPUDescriptorHandleForHeapStart());


    for (int i = 0; i < (int)mAllTextures.size(); ++i)
    {
        auto& tex = mAllTextures[i];

        if (tex->Name == "tess_diffuse")
            mTessObjBaseSrvIndex = i;

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


        const auto& meshIndices = shape.mesh.indices;
        size_t triCount = meshIndices.size() / 3;

        for (size_t tri = 0; tri < triCount; ++tri)
        {
           
            const auto& i0 = meshIndices[tri * 3 + 0];
            const auto& i1 = meshIndices[tri * 3 + 1];
            const auto& i2 = meshIndices[tri * 3 + 2];

          
            XMFLOAT3 pos0 = { attrib.vertices[3 * i0.vertex_index + 0],
                               attrib.vertices[3 * i0.vertex_index + 1],
                               attrib.vertices[3 * i0.vertex_index + 2] };
            XMFLOAT3 pos1 = { attrib.vertices[3 * i1.vertex_index + 0],
                               attrib.vertices[3 * i1.vertex_index + 1],
                               attrib.vertices[3 * i1.vertex_index + 2] };
            XMFLOAT3 pos2 = { attrib.vertices[3 * i2.vertex_index + 0],
                               attrib.vertices[3 * i2.vertex_index + 1],
                               attrib.vertices[3 * i2.vertex_index + 2] };

          
            XMFLOAT2 uv0 = (i0.texcoord_index >= 0) ?
                XMFLOAT2{ attrib.texcoords[2 * i0.texcoord_index + 0],
                          1.0f - attrib.texcoords[2 * i0.texcoord_index + 1] } :
                XMFLOAT2{ 0,0 };
            XMFLOAT2 uv1 = (i1.texcoord_index >= 0) ?
                XMFLOAT2{ attrib.texcoords[2 * i1.texcoord_index + 0],
                          1.0f - attrib.texcoords[2 * i1.texcoord_index + 1] } :
                XMFLOAT2{ 0,0 };
            XMFLOAT2 uv2 = (i2.texcoord_index >= 0) ?
                XMFLOAT2{ attrib.texcoords[2 * i2.texcoord_index + 0],
                          1.0f - attrib.texcoords[2 * i2.texcoord_index + 1] } :
                XMFLOAT2{ 0,0 };


            XMFLOAT3 edge1 = { pos1.x - pos0.x, pos1.y - pos0.y, pos1.z - pos0.z };
            XMFLOAT3 edge2 = { pos2.x - pos0.x, pos2.y - pos0.y, pos2.z - pos0.z };
            XMFLOAT2 deltaUV1 = { uv1.x - uv0.x, uv1.y - uv0.y };
            XMFLOAT2 deltaUV2 = { uv2.x - uv0.x, uv2.y - uv0.y };

            float denom = deltaUV1.x * deltaUV2.y - deltaUV2.x * deltaUV1.y;
            float f = (fabsf(denom) > 1e-8f) ? (1.0f / denom) : 0.0f;

            XMFLOAT3 tangent;
            tangent.x = f * (deltaUV2.y * edge1.x - deltaUV1.y * edge2.x);
            tangent.y = f * (deltaUV2.y * edge1.y - deltaUV1.y * edge2.y);
            tangent.z = f * (deltaUV2.y * edge1.z - deltaUV1.y * edge2.z);

    
            XMVECTOR T = XMLoadFloat3(&tangent);
            float len = XMVectorGetX(XMVector3Length(T));
            if (len > 1e-6f)
                XMStoreFloat3(&tangent, XMVector3Normalize(T));
            else
                tangent = { 1.0f, 0.0f, 0.0f };


            auto makeVert = [&](const tinyobj::index_t& idx,
                const XMFLOAT3& pos,
                const XMFLOAT2& uv) -> Vertex
                {
                    Vertex v = {};
                    v.Pos = pos;
                    v.Normal = (idx.normal_index >= 0) ?
                        XMFLOAT3{ attrib.normals[3 * idx.normal_index + 0],
                                   attrib.normals[3 * idx.normal_index + 1],
                                   attrib.normals[3 * idx.normal_index + 2] } :
                        XMFLOAT3{ 0.0f, 1.0f, 0.0f };
                    v.TexC = uv;
                    v.Tangent = tangent;
                    return v;
                };

            allVertices.push_back(makeVert(i0, pos0, uv0));
            allIndices.push_back((UINT)(allVertices.size() - 1));
            allVertices.push_back(makeVert(i1, pos1, uv1));
            allIndices.push_back((UINT)(allVertices.size() - 1));
            allVertices.push_back(makeVert(i2, pos2, uv2));
            allIndices.push_back((UINT)(allVertices.size() - 1));
            indexCount += 3;
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
        ri.UseTess = false; 
        mRenderItems.push_back(ri);
    }

 
    mCpuVertices.reserve(allVertices.size());
    for (const auto& v : allVertices)
        mCpuVertices.push_back(v.Pos);
    mCpuIndices = allIndices;

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
                v.Tangent = { 1.0f, 0.0f, 0.0f }; // заглушка для звезды
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
        ri.UseTess = false;
        mRenderItems.push_back(ri);
    }


    {
        tinyobj::ObjReader reader3;
        tinyobj::ObjReaderConfig config3;
        config3.triangulate = true;
        const bool tessObjOk = reader3.ParseFromFile("models/source/model.obj", config3)
            && !reader3.GetShapes().empty();
        if (tessObjOk)
        {
            auto& attrib3 = reader3.GetAttrib();
            auto& shapes3 = reader3.GetShapes();

            UINT indexOffset = (UINT)allIndices.size();
            UINT indexCount = 0;

            for (const auto& shape : shapes3)
            {
                const auto& mi = shape.mesh.indices;
                size_t tris = mi.size() / 3;
                for (size_t tri = 0; tri < tris; ++tri)
                {
                    const auto& j0 = mi[tri * 3 + 0];
                    const auto& j1 = mi[tri * 3 + 1];
                    const auto& j2 = mi[tri * 3 + 2];

                    XMFLOAT3 p0 = { attrib3.vertices[3 * j0.vertex_index + 0],
                                    attrib3.vertices[3 * j0.vertex_index + 1],
                                    attrib3.vertices[3 * j0.vertex_index + 2] };
                    XMFLOAT3 p1 = { attrib3.vertices[3 * j1.vertex_index + 0],
                                    attrib3.vertices[3 * j1.vertex_index + 1],
                                    attrib3.vertices[3 * j1.vertex_index + 2] };
                    XMFLOAT3 p2 = { attrib3.vertices[3 * j2.vertex_index + 0],
                                    attrib3.vertices[3 * j2.vertex_index + 1],
                                    attrib3.vertices[3 * j2.vertex_index + 2] };

                    XMFLOAT2 u0 = (j0.texcoord_index >= 0) ?
                        XMFLOAT2{ attrib3.texcoords[2 * j0.texcoord_index + 0],
                                 1.0f - attrib3.texcoords[2 * j0.texcoord_index + 1] } : XMFLOAT2{ 0,0 };
                    XMFLOAT2 u1 = (j1.texcoord_index >= 0) ?
                        XMFLOAT2{ attrib3.texcoords[2 * j1.texcoord_index + 0],
                                 1.0f - attrib3.texcoords[2 * j1.texcoord_index + 1] } : XMFLOAT2{ 0,0 };
                    XMFLOAT2 u2 = (j2.texcoord_index >= 0) ?
                        XMFLOAT2{ attrib3.texcoords[2 * j2.texcoord_index + 0],
                                 1.0f - attrib3.texcoords[2 * j2.texcoord_index + 1] } : XMFLOAT2{ 0,0 };

                 
                    XMFLOAT3 e1 = { p1.x - p0.x,p1.y - p0.y,p1.z - p0.z };
                    XMFLOAT3 e2 = { p2.x - p0.x,p2.y - p0.y,p2.z - p0.z };
                    XMFLOAT2 d1 = { u1.x - u0.x,u1.y - u0.y };
                    XMFLOAT2 d2 = { u2.x - u0.x,u2.y - u0.y };
                    float det = d1.x * d2.y - d2.x * d1.y;
                    float ff = (fabsf(det) > 1e-8f) ? (1.0f / det) : 0.0f;
                    XMFLOAT3 tang;
                    tang.x = ff * (d2.y * e1.x - d1.y * e2.x);
                    tang.y = ff * (d2.y * e1.y - d1.y * e2.y);
                    tang.z = ff * (d2.y * e1.z - d1.y * e2.z);
                    XMVECTOR TT = XMLoadFloat3(&tang);
                    if (XMVectorGetX(XMVector3Length(TT)) > 1e-6f)
                        XMStoreFloat3(&tang, XMVector3Normalize(TT));
                    else tang = { 1,0,0 };

                    auto mv = [&](const tinyobj::index_t& idx,
                        const XMFLOAT3& pp, const XMFLOAT2& uu) -> Vertex
                        {
                            Vertex vv = {};
                            vv.Pos = pp;
                            vv.Normal = (idx.normal_index >= 0) ?
                                XMFLOAT3{ attrib3.normals[3 * idx.normal_index + 0],
                                         attrib3.normals[3 * idx.normal_index + 1],
                                         attrib3.normals[3 * idx.normal_index + 2] } :
                                XMFLOAT3{ 0,1,0 };
                            vv.TexC = uu;
                            vv.Tangent = tang;
                            return vv;
                        };
                    allVertices.push_back(mv(j0, p0, u0)); allIndices.push_back((UINT)(allVertices.size() - 1));
                    allVertices.push_back(mv(j1, p1, u1)); allIndices.push_back((UINT)(allVertices.size() - 1));
                    allVertices.push_back(mv(j2, p2, u2)); allIndices.push_back((UINT)(allVertices.size() - 1));
                    indexCount += 3;
                }
            }

            SubmeshGeometry submesh;
            submesh.IndexCount = indexCount;
            submesh.StartIndexLocation = indexOffset;
            submesh.BaseVertexLocation = 0;
            mModelGeo->DrawArgs["tessMesh"] = submesh;

            RenderItem ri;
            ri.SubmeshName = "tessMesh";
            ri.UseTess = true;
            ri.TexSrvIndex = mTessObjBaseSrvIndex;     // diffuse (t0)
            ri.NormalSrvIndex = mTessObjBaseSrvIndex + 1; // normal  (t1)
            ri.DisplaceSrvIndex = mTessObjBaseSrvIndex + 2; // displace(t2)
            ri.IsStar = false;
            mRenderItems.push_back(ri);
        }
        else
        {
     
            GeometryGenerator gen;
            GeometryGenerator::MeshData grid = gen.CreateGrid(4.0f, 4.0f, 7, 7);

            UINT indexOffset = (UINT)allIndices.size();
            UINT baseV = (UINT)allVertices.size();
            for (const auto& gv : grid.Vertices)
            {
                Vertex v = {};
                v.Pos = gv.Position;
                v.Normal = gv.Normal;
                v.TexC = gv.TexC;
                v.Tangent = gv.TangentU;
                allVertices.push_back(v);
            }
            for (uint32_t ix : grid.Indices32)
                allIndices.push_back(baseV + ix);

            SubmeshGeometry submesh;
            submesh.IndexCount = (UINT)grid.Indices32.size();
            submesh.StartIndexLocation = indexOffset;
            submesh.BaseVertexLocation = 0;
            mModelGeo->DrawArgs["tessMesh"] = submesh;

            RenderItem ri;
            ri.SubmeshName = "tessMesh";
            ri.UseTess = true;
            ri.TexSrvIndex = mTessObjBaseSrvIndex;
            ri.NormalSrvIndex = mTessObjBaseSrvIndex + 1;
            ri.DisplaceSrvIndex = mTessObjBaseSrvIndex + 2;
            ri.IsStar = false;
            mRenderItems.push_back(ri);
        }
    }

   

        {
            GeometryGenerator gen;
    
            GeometryGenerator::MeshData grid = gen.CreateGrid(20.0f, 20.0f, 30, 30);

            UINT indexOffset = (UINT)allIndices.size();
            UINT baseV = (UINT)allVertices.size();

            for (const auto& gv : grid.Vertices)
            {
                Vertex v = {};
                v.Pos = gv.Position;
                v.Normal = gv.Normal;
                v.TexC = gv.TexC;
                v.Tangent = gv.TangentU;
                allVertices.push_back(v);
            }
            for (uint32_t ix : grid.Indices32)
                allIndices.push_back(baseV + ix);

            SubmeshGeometry submesh;
            submesh.IndexCount = (UINT)grid.Indices32.size();
            submesh.StartIndexLocation = indexOffset;
            submesh.BaseVertexLocation = 0;
            mModelGeo->DrawArgs["wavePlane"] = submesh;

            RenderItem ri;
            ri.SubmeshName = "wavePlane";
            ri.UseTess = true;
 
            ri.TexSrvIndex = mTessObjBaseSrvIndex;         
            ri.NormalSrvIndex = mTessObjBaseSrvIndex + 1;  
            ri.DisplaceSrvIndex = mTessObjBaseSrvIndex + 2; 
            ri.IsStar = false;
            mRenderItems.push_back(ri);


        }


        //Билборд
        {
            GeometryGenerator gen;
            GeometryGenerator::MeshData quad = gen.CreateGrid(1.0f, 1.0f, 2, 2);

            UINT indexOffset = (UINT)allIndices.size();
            UINT baseV = (UINT)allVertices.size();

            for (const auto& v : quad.Vertices)
            {
                Vertex vert = {};

                vert.Pos = { v.Position.x, v.Position.z, v.Position.y };
                vert.Normal = { 0.0f, 0.0f, -1.0f };
                vert.TexC = v.TexC;
                vert.Tangent = { 1.0f, 0.0f, 0.0f };
                allVertices.push_back(vert);
            }
            for (uint32_t ix : quad.Indices32)
                allIndices.push_back(baseV + ix);

            SubmeshGeometry submesh;
            submesh.IndexCount = (UINT)quad.Indices32.size();
            submesh.StartIndexLocation = indexOffset;
            submesh.BaseVertexLocation = 0;
            mModelGeo->DrawArgs["billboard"] = submesh;
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
    XMVECTOR eye = XMLoadFloat3(&mCurrCameraPos);

    float x = sinf(mPhi) * cosf(mTheta);
    float z = sinf(mPhi) * sinf(mTheta);
    float y = cosf(mPhi);
    XMVECTOR dir = XMVectorSet(x, y, z, 0.0f);

    const float kStartOffset = 0.5f; 
    XMVECTOR rayOrigin = XMVectorAdd(eye, XMVectorScale(dir, kStartOffset));

    XMMATRIX world = XMLoadFloat4x4(&mSponzaWorld);
    float tMin = FLT_MAX;
    bool hit = false;
    const float kMinHitDistance = 0.1f;

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

    if (!hit) tMin = 100.0f;

    ShotLight sl;
    XMStoreFloat3(&sl.Origin, rayOrigin);
    XMStoreFloat3(&sl.Direction, dir);
    XMStoreFloat3(&sl.Position, rayOrigin);
    XMStoreFloat3(&sl.Velocity, XMVectorScale(dir, mLightSpeed));

    static const XMFLOAT3 palette[] = {
        {1.0f,0.4f,0.1f},{0.2f,0.6f,1.0f},{0.4f,1.0f,0.4f},
        {1.0f,0.2f,0.8f},{1.0f,1.0f,0.3f},{0.5f,0.2f,1.0f}
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
        if (wParam == VK_OEM_4) // [
            mTessDisplaceScale = MathHelper::Max(0.0f, mTessDisplaceScale - 0.01f);
        if (wParam == VK_OEM_6) // ]
            mTessDisplaceScale += 0.01f;
        if (wParam == VK_PRIOR)
            mTessMaxTess = MathHelper::Min(64.0f, mTessMaxTess + 1.0f);
        if (wParam == VK_NEXT)
            mTessMaxTess = MathHelper::Max(1.0f, mTessMaxTess - 1.0f);
        if (wParam == '1') mCullingMode = CullingMode::None;
        if (wParam == '2') mCullingMode = CullingMode::BruteForce;
        if (wParam == '3') mCullingMode = CullingMode::Octree;
    }
    return D3DApp::MsgProc(hwnd, msg, wParam, lParam);
}

void BoxApp::Update(const GameTimer& gt)
{
    float dt = gt.DeltaTime();
    const float moveSpeed = 40.0f;
    float x = sinf(mPhi) * cosf(mTheta);
    float z = sinf(mPhi) * sinf(mTheta);
    float y = cosf(mPhi);

    XMVECTOR lookDir = XMVectorSet(x, y, z, 0.0f);
    XMVECTOR pos = XMLoadFloat3(&mCurrCameraPos);

    if (GetAsyncKeyState('W') & 0x8000)
        pos = XMVectorAdd(pos, XMVectorScale(lookDir, moveSpeed * dt));
    if (GetAsyncKeyState('S') & 0x8000)
        pos = XMVectorSubtract(pos, XMVectorScale(lookDir, moveSpeed * dt));

    XMVECTOR upVec = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
    XMVECTOR rightDir = XMVector3Normalize(XMVector3Cross(upVec, lookDir));

    if (GetAsyncKeyState('A') & 0x8000)
        pos = XMVectorAdd(pos, XMVectorScale(rightDir, moveSpeed * dt));
    if (GetAsyncKeyState('D') & 0x8000)
        pos = XMVectorSubtract(pos, XMVectorScale(rightDir, moveSpeed * dt));

    XMStoreFloat3(&mCurrCameraPos, pos);
    mEyePosW = mCurrCameraPos;

    XMVECTOR target = XMVectorAdd(pos, lookDir);
    XMStoreFloat4x4(&mView, XMMatrixLookAtLH(pos, target, upVec));


    //FRUSTUM CULLING 
    XMMATRIX projMat = XMLoadFloat4x4(&mProj);
    XMMATRIX viewMat = XMLoadFloat4x4(&mView);

    BoundingFrustum frustum;
    BoundingFrustum::CreateFromMatrix(frustum, projMat);

    XMMATRIX invView = XMMatrixInverse(nullptr, viewMat);
    frustum.Transform(frustum, invView);

    for (auto& item : mInstancedItems)
        item.IsVisible = (mCullingMode == CullingMode::None);

    if (mCullingMode == CullingMode::BruteForce) {

        for (auto& item : mInstancedItems) {
            BoundingBox worldBox;
            XMMATRIX worldMat = XMLoadFloat4x4(&item.World);
            item.Bounds.Transform(worldBox, worldMat);
            if (frustum.Contains(worldBox) != DISJOINT)
                item.IsVisible = true;
        }
    }
    else if (mCullingMode == CullingMode::Octree) {
        GetVisibleItemsOctree(mRootNode.get(), frustum);
    }

    // Обновляем заголовок окна информацией о видимости
    int visibleCount = 0;
    for (auto& item : mInstancedItems) if (item.IsVisible) visibleCount++;

    std::wstring modeName = (mCullingMode == CullingMode::None) ? L"None" :
        (mCullingMode == CullingMode::BruteForce) ? L"BruteForce" : L"Octree";

    std::wstring stats = L"Culling: " + modeName +
        L" | Visible: " + std::to_wstring(visibleCount) +
        L" / " + std::to_wstring(mInstancedItems.size());
    SetWindowText(mhMainWnd, stats.c_str());


    // SHOT LIGHTS
    if (mShootRequested)
    {
        ShootLightFromCamera();
        mShootRequested = false;
    }

    const float kMarkerRadius = 0.12f;
    const float kSurfaceBias = 0.03f;
    for (auto& sl : mShotLights)
    {
        if (sl.IsFlying)
        {
            float stepDist = mLightSpeed * dt;
            float triggerT = sl.TargetT - kMarkerRadius;
            if (triggerT < 0.0f) triggerT = 0.0f;

            if (sl.CurrentT + stepDist >= triggerT)
            {
                sl.IsFlying = false;
                sl.Range = 28.0f;
                XMVECTOR o = XMLoadFloat3(&sl.Origin);
                XMVECTOR d = XMLoadFloat3(&sl.Direction);
                float placeT = sl.TargetT - kSurfaceBias;
                if (placeT < 0.0f) placeT = 0.0f;
                XMStoreFloat3(&sl.Position, XMVectorAdd(o, XMVectorScale(d, placeT)));
                sl.Velocity = { 0.0f, 0.0f, 0.0f };
                sl.CurrentT = sl.TargetT;
            }
            else
            {
                sl.CurrentT += stepDist;
                XMVECTOR p = XMLoadFloat3(&sl.Position);
                XMVECTOR v = XMLoadFloat3(&sl.Velocity);
                XMStoreFloat3(&sl.Position, XMVectorAdd(p, XMVectorScale(v, dt)));
            }
        }
    }

    XMStoreFloat4x4(&mWorld, XMMatrixScaling(1.0f, 1.0f, 1.0f));
    mSponzaWorld = mWorld;
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

    // Heap для объектных текстур
    {
        ID3D12DescriptorHeap* heaps[] = { mObjectSrvHeap.Get() };
        mCommandList->SetDescriptorHeaps(_countof(heaps), heaps);
    }

   
    XMMATRIX world = XMLoadFloat4x4(&mWorld);
    XMMATRIX view = XMLoadFloat4x4(&mView);
    XMMATRIX proj = XMLoadFloat4x4(&mProj);

    UINT srvSize = md3dDevice->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    mCommandList->IASetVertexBuffers(0, 1, &mModelGeo->VertexBufferView());
    mCommandList->IASetIndexBuffer(&mModelGeo->IndexBufferView());

    // GEOMETRY PASS

    mRenderingSystem.BeginGeometryPass(mCommandList.Get(), DepthStencilView());
    mCommandList->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);


    UINT geomCbIndex = 0;

    // Sponza
    {
        GeometryPassConstants geomConsts;
        XMStoreFloat4x4(&geomConsts.WorldViewProj,
            XMMatrixTranspose(world * view * proj));
        XMStoreFloat4x4(&geomConsts.World, XMMatrixTranspose(world));
        XMStoreFloat4x4(&geomConsts.WorldInvTranspose,
            XMMatrixTranspose(XMMatrixTranspose(XMMatrixInverse(nullptr, world))));
        geomConsts.Time = 0.0f;

        mRenderingSystem.SetGeometryPassConstants(mCommandList.Get(), geomConsts, geomCbIndex++);

        for (const auto& ri : mRenderItems)
        {
            if (ri.IsStar || ri.UseTess) continue;

            CD3DX12_GPU_DESCRIPTOR_HANDLE texHandle(
                mObjectSrvHeap->GetGPUDescriptorHandleForHeapStart());
            texHandle.Offset(ri.TexSrvIndex, srvSize);
            mCommandList->SetGraphicsRootDescriptorTable(1, texHandle);

            const auto& sub = mModelGeo->DrawArgs[ri.SubmeshName];
            mCommandList->DrawIndexedInstanced(
                sub.IndexCount, 1, sub.StartIndexLocation, sub.BaseVertexLocation, 0);
        }
    }

    // Звёзды
    for (const auto& sl : mShotLights)
    {
        XMMATRIX shotWorld =
            XMMatrixScaling(0.12f, 0.12f, 0.12f) *
            XMMatrixRotationY(gt.TotalTime() * 2.0f) *
            XMMatrixTranslation(sl.Position.x, sl.Position.y, sl.Position.z);

        GeometryPassConstants shotConsts;
        XMStoreFloat4x4(&shotConsts.WorldViewProj,
            XMMatrixTranspose(shotWorld * view * proj));
        XMStoreFloat4x4(&shotConsts.World, XMMatrixTranspose(shotWorld));
        XMStoreFloat4x4(&shotConsts.WorldInvTranspose,
            XMMatrixTranspose(XMMatrixTranspose(XMMatrixInverse(nullptr, shotWorld))));
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

    for (const auto& ri : mInstancedItems)
    {
        if (!ri.IsVisible) continue; // Куллинг!

        GeometryPassConstants gc;
        XMMATRIX worldMat = XMLoadFloat4x4(&ri.World);

        XMStoreFloat4x4(&gc.WorldViewProj, XMMatrixTranspose(worldMat * view * proj));
        XMStoreFloat4x4(&gc.World, XMMatrixTranspose(worldMat));
        XMStoreFloat4x4(&gc.WorldInvTranspose, MathHelper::InverseTranspose(worldMat));

        gc.pad.x = 2.0f;

        mRenderingSystem.SetGeometryPassConstants(mCommandList.Get(), gc, geomCbIndex++);

        CD3DX12_GPU_DESCRIPTOR_HANDLE texHandle(mObjectSrvHeap->GetGPUDescriptorHandleForHeapStart());
        texHandle.Offset(ri.TexSrvIndex, srvSize);
        mCommandList->SetGraphicsRootDescriptorTable(1, texHandle);

        const auto& sub = mModelGeo->DrawArgs[ri.SubmeshName];
        mCommandList->DrawIndexedInstanced(sub.IndexCount, 1, sub.StartIndexLocation, sub.BaseVertexLocation, 0);
    }

    mRenderingSystem.BeginTessellationPass(mCommandList.Get(), DepthStencilView());

    const float ts = mTessWorldScale;
    XMMATRIX tessWorld =
        XMMatrixScaling(ts, ts, ts) *
        XMMatrixTranslation(mTessWorldOffset.x, mTessWorldOffset.y, mTessWorldOffset.z) *
        world;

    UINT tessCbSlot = 0;
    for (const auto& ri : mRenderItems)
    {
        if (!ri.UseTess) continue;
        if (ri.TexSrvIndex < 0 || ri.NormalSrvIndex < 0 || ri.DisplaceSrvIndex < 0) continue;

       
        GeometryPassConstants gc;
        XMMATRIX finalWorld;

    
        if (ri.SubmeshName == "wavePlane")
        {
            finalWorld = XMMatrixTranslation(0.0f, -1.0f, 0.0f);
            gc.pad.x = 1.0f; 
        }
        else
        {
            const float ts = mTessWorldScale;
            finalWorld = XMMatrixScaling(ts, ts, ts) *
                XMMatrixTranslation(mTessWorldOffset.x, mTessWorldOffset.y, mTessWorldOffset.z) *
                world;
            gc.pad.x = 0.0f; 
        }

     
        XMStoreFloat4x4(&gc.WorldViewProj, XMMatrixTranspose(finalWorld * view * proj));
        XMStoreFloat4x4(&gc.World, XMMatrixTranspose(finalWorld));
        XMStoreFloat4x4(&gc.WorldInvTranspose,
            XMMatrixTranspose(XMMatrixTranspose(XMMatrixInverse(nullptr, finalWorld))));
        gc.Time = gt.TotalTime();

        TessellationConstants tc;
        tc.EyePosW = mEyePosW;
        tc.MinTessDist = mTessMinTessDist;
        tc.MaxTessDist = mTessMaxTessDist;
        tc.MinTess = mTessMinTess;
        tc.MaxTess = mTessMaxTess;

        if (ri.SubmeshName == "wavePlane")
            tc.DisplaceScale = 0.5f;
        else
            tc.DisplaceScale = mTessDisplaceScale;

        CD3DX12_GPU_DESCRIPTOR_HANDLE srvBase(mObjectSrvHeap->GetGPUDescriptorHandleForHeapStart());
        srvBase.Offset(ri.TexSrvIndex, srvSize);
        mCommandList->SetGraphicsRootDescriptorTable(2, srvBase);

    
        mRenderingSystem.SetTessellationConstants(mCommandList.Get(), gc, geomCbIndex++, tc, tessCbSlot++);

     
        const auto& sub = mModelGeo->DrawArgs[ri.SubmeshName];
        mCommandList->DrawIndexedInstanced(
            sub.IndexCount, 1, sub.StartIndexLocation, sub.BaseVertexLocation, 0);
    }

    // После отрисовки основных RenderItems в Tessellation Pass:
    for (const auto& ri : mInstancedItems)
    {
        if (!ri.IsVisible) continue; 

        XMMATRIX worldMat = XMLoadFloat4x4(&ri.World);
        GeometryPassConstants gc;
        XMStoreFloat4x4(&gc.WorldViewProj, XMMatrixTranspose(worldMat * view * proj));
        XMStoreFloat4x4(&gc.World, XMMatrixTranspose(worldMat));
        XMStoreFloat4x4(&gc.WorldInvTranspose, MathHelper::InverseTranspose(worldMat));
        gc.Time = gt.TotalTime();
        gc.pad.x = 0.0f;

        TessellationConstants tc; 
        tc.EyePosW = mEyePosW;
        tc.DisplaceScale = mTessDisplaceScale;
        tc.MinTessDist = mTessMinTessDist; tc.MaxTessDist = mTessMaxTessDist;
        tc.MinTess = mTessMinTess; tc.MaxTess = mTessMaxTess;

        CD3DX12_GPU_DESCRIPTOR_HANDLE srv(mObjectSrvHeap->GetGPUDescriptorHandleForHeapStart());
        srv.Offset(ri.TexSrvIndex, srvSize);
        mCommandList->SetGraphicsRootDescriptorTable(2, srv);

        mRenderingSystem.SetTessellationConstants(mCommandList.Get(), gc, geomCbIndex++, tc, tessCbSlot++);
        mCommandList->DrawIndexedInstanced(mModelGeo->DrawArgs[ri.SubmeshName].IndexCount, 1,
            mModelGeo->DrawArgs[ri.SubmeshName].StartIndexLocation, 0, 0);
    }

    
    mRenderingSystem.EndGeometryPass(mCommandList.Get());

  
    // LIGHTING PASS
  
    mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(
        mDepthStencilBuffer.Get(),
        D3D12_RESOURCE_STATE_DEPTH_WRITE,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));

    {
        ID3D12DescriptorHeap* heaps[] = { mSrvHeap.Get() };
        mCommandList->SetDescriptorHeaps(_countof(heaps), heaps);
    }

    mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(
        CurrentBackBuffer(),
        D3D12_RESOURCE_STATE_PRESENT,
        D3D12_RESOURCE_STATE_RENDER_TARGET));
    mCommandList->ClearRenderTargetView(CurrentBackBufferView(), Colors::Black, 0, nullptr);

    mRenderingSystem.ClearLights();
    mRenderingSystem.AddDirectionalLight({ 0.3f,-1.0f,0.5f }, { 1.0f,0.95f,0.8f }, 1.0f);
    mRenderingSystem.AddPointLight({ 0.0f,2.0f, 0.0f }, { 1.0f,0.2f,0.1f }, 3.0f, 8.0f);
    mRenderingSystem.AddPointLight({ 5.0f,2.0f,-3.0f }, { 0.1f,0.5f,1.0f }, 2.0f, 6.0f);
    mRenderingSystem.AddSpotLight({ 0.0f,5.0f,0.0f }, { 0.0f,-1.0f,0.0f },
        { 1.0f,1.0f,0.8f }, 5.0f, 10.0f, 30.0f);

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
        mRenderingSystem.AddPointLight(sl.Position, sl.Color, 36.0f, 30.0f);
        --shotBudget;
    }
    for (int i = (int)mShotLights.size() - 1; i >= 0 && shotBudget > 0; --i)
    {
        const auto& sl = mShotLights[i];
        if (sl.IsFlying) continue;
        mRenderingSystem.AddPointLight(sl.Position, sl.Color, 35.0f, sl.Range);
        --shotBudget;
    }

    mRenderingSystem.DoLightingPass(
        mCommandList.Get(), CurrentBackBufferView(), DepthStencilView(),
        mEyePosW, ivp, iv, ip, mDepthSrvGpuHandle);

    mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(
        mDepthStencilBuffer.Get(),
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
        D3D12_RESOURCE_STATE_DEPTH_WRITE));

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
        md3dDevice.Get(), mClientWidth, mClientHeight,
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

void BoxApp::BuildInstancedItems() {
    mInstancedItems.clear();

    int nX = 6, nY = 6, nZ = 6;

    int billboardTexIndex = 0;
    for (int i = 0; i < (int)mAllTextures.size(); ++i) {
        if (mAllTextures[i]->Name == "human_sprite") { 
            billboardTexIndex = i; break;
        }
    }

    BoundingBox baseBox;
    baseBox.Center = { 0, 0, 0 };
    baseBox.Extents = { 1.0f, 1.0f, 1.0f };

    for (int x = 0; x < nX; ++x) {
        for (int y = 0; y < nY; ++y) {
            for (int z = 0; z < nZ; ++z) {
                RenderItem ri;
                ri.SubmeshName = "billboard";
                ri.TexSrvIndex = billboardTexIndex;
                ri.UseTess = false;

                XMMATRIX w = XMMatrixTranslation(x * 8.0f - 40.0f, y * 3.0f + 1.0f, z * 8.0f + 20.0f);
                XMStoreFloat4x4(&ri.World, w);
                ri.Bounds = baseBox;
                mInstancedItems.push_back(ri);
            }
        }
    }

    mRootNode = std::make_unique<OctreeNode>();
    mRootNode->Box = BoundingBox(XMFLOAT3(0, 10, 50), XMFLOAT3(100, 30, 100));

    for (int i = 0; i < (int)mInstancedItems.size(); ++i)
        mRootNode->ItemIndices.push_back(i);

    BuildOctree(mRootNode.get(), 0);
}

void BoxApp::BuildOctree(OctreeNode* node, int depth) {
    if (depth > 3 || node->ItemIndices.size() <= 5) return;
    node->IsLeaf = false;
    XMFLOAT3 c = node->Box.Center;
    XMFLOAT3 e = node->Box.Extents;
    XMFLOAT3 h = XMFLOAT3(e.x * 0.5f, e.y * 0.5f, e.z * 0.5f);

    for (int i = 0; i < 8; ++i) {
        node->Children[i] = std::make_unique<OctreeNode>();
        XMFLOAT3 nc = c;
        nc.x += h.x * ((i & 1) ? 1 : -1); nc.y += h.y * ((i & 2) ? 1 : -1); nc.z += h.z * ((i & 4) ? 1 : -1);//магия двоички - кубики распределяются по углам) 1,2,4 это 2^(0||1||2) порядок разряда для проверки)
        node->Children[i]->Box = BoundingBox(nc, h);
        for (int idx : node->ItemIndices) {
            BoundingBox wb;
            mInstancedItems[idx].Bounds.Transform(wb, XMLoadFloat4x4(&mInstancedItems[idx].World));
            if (node->Children[i]->Box.Intersects(wb)) node->Children[i]->ItemIndices.push_back(idx);
        }
        BuildOctree(node->Children[i].get(), depth + 1);
    }
    node->ItemIndices.clear();
}

void BoxApp::GetVisibleItemsOctree(OctreeNode* node, const BoundingFrustum& frustum) {
    if (frustum.Contains(node->Box) == DISJOINT) return;
    if (node->IsLeaf) {
        for (int idx : node->ItemIndices) {
            BoundingBox wb;
            mInstancedItems[idx].Bounds.Transform(wb, XMLoadFloat4x4(&mInstancedItems[idx].World));
            if (frustum.Contains(wb) != DISJOINT) mInstancedItems[idx].IsVisible = true;
        }
    }
    else {
        for (int i = 0; i < 8; ++i) GetVisibleItemsOctree(node->Children[i].get(), frustum);
    }
}