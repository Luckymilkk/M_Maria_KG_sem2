#pragma once
#include "Common/d3dUtil.h"
#include "Common/d3dx12.h"
#include "Common/UploadBuffer.h"
#include "GBuffer.h"
#include <vector>


enum class LightType : int
{
    Directional = 0,
    Point = 1,
    Spot = 2
};

struct LightData
{
    DirectX::XMFLOAT3 Position;
    float             Range;

    DirectX::XMFLOAT3 Direction;
    float             SpotAngle;

    DirectX::XMFLOAT3 Color;
    int               Type;
};

struct GeometryPassConstants
{
    DirectX::XMFLOAT4X4 WorldViewProj = MathHelper::Identity4x4();
    DirectX::XMFLOAT4X4 World = MathHelper::Identity4x4();
    DirectX::XMFLOAT4X4 WorldInvTranspose = MathHelper::Identity4x4();
    float       Time = 0.0f;
    DirectX::XMFLOAT3    pad = {};
};

// Константы для тесселяционного прохода (cbuffer b1 в tessellation.hlsl)
struct TessellationConstants
{
    DirectX::XMFLOAT3 EyePosW = { 0,0,0 };
    float             DisplaceScale = 0.05f;  // сила смещения

    float MinTessDist = 2.0f;   // расстояние "вблизи"  → MaxTess
    float MaxTessDist = 50.0f;  // расстояние "вдали"   → MinTess
    float MinTess = 1.0f;   // TF когда объект далеко
    float MaxTess = 16.0f;  // TF когда объект близко
};


static const int kMaxLights = 64;

struct LightingPassConstants
{
    LightData           Lights[kMaxLights];
    int                 NumLights;
    float               pad0;
    float               pad1;
    float               pad2;
    DirectX::XMFLOAT3   EyePosW;
    float               pad3;

    DirectX::XMFLOAT4X4 InvViewProj = MathHelper::Identity4x4();
    DirectX::XMFLOAT4X4 InvView;
    DirectX::XMFLOAT4X4 InvProj;
    DirectX::XMFLOAT4X4 View;

    DirectX::XMFLOAT4X4 CascadeShadowTransform[4];
    DirectX::XMFLOAT4 CascadeSplits = { 10.0f, 30.0f, 80.0f, 150.0f };
    DirectX::XMFLOAT4 ShadowParams = { 0.0020f, 1.0f / 1024.0f, 1.0f, 0.0f }; // bias, texelSize, pcfRadius, pad
};

class RenderingSystem
{
public:
    static const UINT kShadowCascadeCount = 2;

    RenderingSystem() = default;
    ~RenderingSystem() = default;

    RenderingSystem(const RenderingSystem&) = delete;
    RenderingSystem& operator=(const RenderingSystem&) = delete;

    void Init(
        ID3D12Device* device,
        ID3D12GraphicsCommandList* cmdList,
        UINT width, UINT height,
        DXGI_FORMAT backBufferFormat,
        DXGI_FORMAT depthStencilFormat,
        ID3D12DescriptorHeap* rtvHeap,
        ID3D12DescriptorHeap* srvHeap,
        UINT gbufferRtvOffset,
        UINT gbufferSrvOffset,
        UINT shadowSrvOffset
    );

    void OnResize(
        ID3D12Device* device,
        UINT width, UINT height,
        ID3D12DescriptorHeap* rtvHeap,
        ID3D12DescriptorHeap* srvHeap,
        UINT gbufferRtvOffset,
        UINT gbufferSrvOffset,
        UINT shadowSrvOffset
    );

    void ClearLights() { mLights.clear(); }

    void AddDirectionalLight(DirectX::XMFLOAT3 direction,
        DirectX::XMFLOAT3 color,
        float intensity);

    void AddPointLight(DirectX::XMFLOAT3 position,
        DirectX::XMFLOAT3 color,
        float intensity,
        float range);

    void AddSpotLight(DirectX::XMFLOAT3 position,
        DirectX::XMFLOAT3 direction,
        DirectX::XMFLOAT3 color,
        float intensity,
        float range,
        float spotAngleDegrees);

    void BeginGeometryPass(ID3D12GraphicsCommandList* cmdList,
        D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle);

    void EndGeometryPass(ID3D12GraphicsCommandList* cmdList);

    void SetGeometryPassConstants(
        ID3D12GraphicsCommandList* cmdList,
        const GeometryPassConstants& constants,
        UINT cbIndex);

    void BeginShadowPass(ID3D12GraphicsCommandList* cmdList);
    void BeginShadowCascade(ID3D12GraphicsCommandList* cmdList, UINT cascadeIndex);
    void EndShadowPass(ID3D12GraphicsCommandList* cmdList);

    // ---- Tessellation pass ----

    // Начало геометрического прохода с тесселяцией.
    // Вызывать вместо BeginGeometryPass для объектов с тесселяцией.
    void BeginTessellationPass(
        ID3D12GraphicsCommandList* cmdList,
        D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle);

    // Устанавливает константы тесселяции (cbuffer b1) и геометрии (cbuffer b0).
    // tessIndex — слот в буфере тесселяции (обычно 0, если объект один).
    void SetTessellationConstants(
        ID3D12GraphicsCommandList* cmdList,
        const GeometryPassConstants& geomConsts,
        UINT geomCbIndex,
        const TessellationConstants& tessConsts,
        UINT tessIndex = 0);

    // ---- Root signatures / PSOs (геттеры) ----
    ID3D12RootSignature* GetGeometryRootSignature() const { return mGeometryRootSig.Get(); }
    ID3D12PipelineState* GetGeometryPSO()           const { return mGeometryPSO.Get(); }
    ID3D12Resource* GetGeometryCBResource()    const { return mGeomCB->Resource(); }

    ID3D12RootSignature* GetTessellationRootSignature() const { return mTessRootSig.Get(); }
    ID3D12PipelineState* GetTessellationPSO()           const { return mTessPSO.Get(); }

    void DoLightingPass(ID3D12GraphicsCommandList* cmdList,
        D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle,
        D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle,
        DirectX::XMFLOAT3 eyePos,
        DirectX::XMFLOAT4X4 invViewProj,
        DirectX::XMFLOAT4X4 invView,
        DirectX::XMFLOAT4X4 invProj,
        DirectX::XMFLOAT4X4 view,
        const DirectX::XMFLOAT4X4* cascadeShadowTransforms,
        const float* cascadeSplits,
        D3D12_GPU_DESCRIPTOR_HANDLE depthSrvHandle,
        D3D12_GPU_DESCRIPTOR_HANDLE shadowSrvHandle);

private:
    void BuildGeometryPassPSO(ID3D12Device* device, DXGI_FORMAT depthStencilFormat);
    void BuildLightingPassPSO(ID3D12Device* device, DXGI_FORMAT backBufferFormat,
        DXGI_FORMAT depthStencilFormat);
    void BuildTessellationPSO(ID3D12Device* device, DXGI_FORMAT depthStencilFormat);
    void BuildShadowPassPSO(ID3D12Device* device, DXGI_FORMAT depthStencilFormat);
    void BuildRootSignatures(ID3D12Device* device);
    void BuildFullscreenQuad(ID3D12Device* device, ID3D12GraphicsCommandList* cmdList);
    void BuildShadowMaps(ID3D12Device* device, UINT shadowSrvOffset);

    GBuffer mGBuffer;

    // Root signatures
    Microsoft::WRL::ComPtr<ID3D12RootSignature> mGeometryRootSig;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> mLightingRootSig;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> mTessRootSig;     // для тесселяционного прохода
    Microsoft::WRL::ComPtr<ID3D12RootSignature> mShadowRootSig;

    // Pipeline states
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mGeometryPSO;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mLightingPSO;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mTessPSO;         // с HS + DS
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mShadowPSO;

    // Shaders
    Microsoft::WRL::ComPtr<ID3DBlob> mGeomVS, mGeomPS;
    Microsoft::WRL::ComPtr<ID3DBlob> mLightVS, mLightPS;
    Microsoft::WRL::ComPtr<ID3DBlob> mTessVS, mTessHS, mTessDS, mTessPS;
    Microsoft::WRL::ComPtr<ID3DBlob> mShadowVS;

    // Constant buffers
    std::unique_ptr<UploadBuffer<GeometryPassConstants>>  mGeomCB;
    std::unique_ptr<UploadBuffer<LightingPassConstants>>  mLightCB;
    std::unique_ptr<UploadBuffer<TessellationConstants>>  mTessCB;
    UINT mGeomCBByteSize = 0;
    static const UINT kMaxGeometryCBs = 4096;
    static const UINT kMaxTessCBs = 512;

    std::vector<LightData> mLights;

    // Fullscreen quad
    Microsoft::WRL::ComPtr<ID3D12Resource> mQuadVB;
    Microsoft::WRL::ComPtr<ID3D12Resource> mQuadVBUploader;
    D3D12_VERTEX_BUFFER_VIEW               mQuadVBView = {};

    ID3D12DescriptorHeap* mSrvHeap = nullptr;
    UINT                  mGbufferSrvOffset = 0;
    UINT                  mShadowSrvOffset = 0;
    UINT                  mSrvDescriptorSize = 0;
    UINT                  mDsvDescriptorSize = 0;

    Microsoft::WRL::ComPtr<ID3D12Resource> mShadowMap = nullptr;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> mShadowDsvHeap = nullptr;
    D3D12_VIEWPORT mShadowViewport = {};
    D3D12_RECT     mShadowScissorRect = {};
    static const UINT kShadowMapSize = 1024;

    DXGI_FORMAT mBackBufferFormat = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT mDepthStencilFormat = DXGI_FORMAT_UNKNOWN;
};

