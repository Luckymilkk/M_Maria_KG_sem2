//***************************************************************************************
// BoxApp.cpp by Frank Luna (C) 2015 All Rights Reserved.
//
// Shows how to draw a box in Direct3D 12.
//
// Controls:
//   Hold the left mouse button down and move the mouse to rotate.
//   Hold the right mouse button down and move the mouse to zoom in and out.
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
};

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

    void LoadTextures();
    void BuildDescriptorHeaps();
    void BuildModelGeometry();

private:
    RenderingSystem mRenderingSystem;

    // Куча для G-buffer RTV
    ComPtr<ID3D12DescriptorHeap> mGbufferRtvHeap;

    // Куча для G-buffer SRV (читается в lighting pass)
    ComPtr<ID3D12DescriptorHeap> mSrvHeap;

    // Отдельная куча для текстур объектов (читается в geometry pass)
    ComPtr<ID3D12DescriptorHeap> mObjectSrvHeap;

    std::vector<RenderItem> mRenderItems;
    XMFLOAT3 mEyePosW = { 0.0f, 0.0f, 0.0f };

    static const UINT mGbufferRtvOffset = 0;
    static const UINT mGbufferSrvOffset = 0;

    std::vector<std::unique_ptr<MyTexture>> mAllTextures;
    std::unique_ptr<MeshGeometry> mModelGeo = nullptr;

    XMFLOAT4X4 mWorld = MathHelper::Identity4x4();
    XMFLOAT4X4 mView = MathHelper::Identity4x4();
    XMFLOAT4X4 mProj = MathHelper::Identity4x4();

    float mTheta = 1.5f * XM_PI;
    float mPhi = XM_PIDIV4;
    float mRadius = 10.0f;
    POINT mLastMousePos;
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
    return true;
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

   
}

void BoxApp::BuildDescriptorHeaps()
{
    LoadTextures();

    // G-buffer RTV куча (3 слота)
    D3D12_DESCRIPTOR_HEAP_DESC rtvDesc = {};
    rtvDesc.NumDescriptors = GBuffer::NumRTs;
    rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    ThrowIfFailed(md3dDevice->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&mGbufferRtvHeap)));

    // G-buffer SRV куча (3 слота, для lighting pass)
    D3D12_DESCRIPTOR_HEAP_DESC srvDesc = {};
    srvDesc.NumDescriptors = GBuffer::NumRTs;
    srvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ThrowIfFailed(md3dDevice->CreateDescriptorHeap(&srvDesc, IID_PPV_ARGS(&mSrvHeap)));

    // Отдельная куча для текстур объектов (для geometry pass)
    D3D12_DESCRIPTOR_HEAP_DESC objSrvDesc = {};
    objSrvDesc.NumDescriptors = 64;
    objSrvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    objSrvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ThrowIfFailed(md3dDevice->CreateDescriptorHeap(&objSrvDesc, IID_PPV_ARGS(&mObjectSrvHeap)));

    // Инициализируем RenderingSystem
    mRenderingSystem.Init(
        md3dDevice.Get(), mCommandList.Get(),
        mClientWidth, mClientHeight,
        mBackBufferFormat, mDepthStencilFormat,
        mGbufferRtvHeap.Get(), mSrvHeap.Get(),
        mGbufferRtvOffset, mGbufferSrvOffset
    );

    // Кладём текстуры объектов в mObjectSrvHeap начиная со слота 0
    UINT srvSize = md3dDevice->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    CD3DX12_CPU_DESCRIPTOR_HANDLE hDesc(
        mObjectSrvHeap->GetCPUDescriptorHandleForHeapStart());

    for (auto& tex : mAllTextures) {
        D3D12_SHADER_RESOURCE_VIEW_DESC srvD = {};
        srvD.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvD.Format = tex->Resource->GetDesc().Format;
        srvD.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvD.Texture2D.MipLevels = tex->Resource->GetDesc().MipLevels;
        md3dDevice->CreateShaderResourceView(tex->Resource.Get(), &srvD, hDesc);
        hDesc.Offset(1, srvSize);
    }

    // Источники света
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
    config.triangulate = false;

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

    float s = 0.01f;
    XMStoreFloat4x4(&mWorld, XMMatrixScaling(s, s, s));
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

    // ====================================================
    // GEOMETRY PASS — используем кучу текстур объектов
    // ====================================================
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

    UINT srvSize = md3dDevice->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    mCommandList->IASetVertexBuffers(0, 1, &mModelGeo->VertexBufferView());
    mCommandList->IASetIndexBuffer(&mModelGeo->IndexBufferView());
    mCommandList->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    for (const auto& ri : mRenderItems)
    {
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

    // ====================================================
    // LIGHTING PASS — переключаемся на кучу G-buffer SRV
    // ====================================================
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

    mRenderingSystem.DoLightingPass(
        mCommandList.Get(),
        CurrentBackBufferView(),
        DepthStencilView(),
        mEyePosW);

    // ====================================================
    // PRESENT
    // ====================================================
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