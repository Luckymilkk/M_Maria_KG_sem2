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

struct TessellationConstants
{
    DirectX::XMFLOAT3 EyePosW = { 0,0,0 };
    float             DisplaceScale = 0.05f;

    float MinTessDist = 2.0f;
    float MaxTessDist = 50.0f;
    float MinTess = 1.0f;
    float MaxTess = 16.0f;
};

struct ShadowPassConstants
{
    DirectX::XMFLOAT4X4 WorldViewProj = MathHelper::Identity4x4();
    float               UseAlphaTest = 0.0f;
    DirectX::XMFLOAT3   Pad = {};
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

    DirectX::XMFLOAT4X4 LightViewProj[3];
    float               CascadeEndDepths[4];
};

struct PostProcessConstants
{
    int   EnableThermal = 0;
    int   EnableChromatic = 0;
    int   EnableLensFlare = 0;
    float Pad0 = 0.0f;
    DirectX::XMFLOAT2 LightScreenPos = { 0.5f, 0.5f };
    float LightVisible = 0.0f;
    float Time = 0.0f;
};

class RenderingSystem
{
public:
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
        UINT gbufferSrvOffset
    );

    void OnResize(
        ID3D12Device* device,
        UINT width, UINT height,
        ID3D12DescriptorHeap* rtvHeap,
        ID3D12DescriptorHeap* srvHeap,
        UINT gbufferRtvOffset,
        UINT gbufferSrvOffset
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

    void BeginTessellationPass(
        ID3D12GraphicsCommandList* cmdList,
        D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle);

    void SetTessellationConstants(
        ID3D12GraphicsCommandList* cmdList,
        const GeometryPassConstants& geomConsts,
        UINT geomCbIndex,
        const TessellationConstants& tessConsts,
        UINT tessIndex = 0);

    void BeginShadowPass(ID3D12GraphicsCommandList* cmdList,
        D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle,
        D3D12_VIEWPORT viewport, D3D12_RECT scissor);

    void SetShadowPassConstants(
        ID3D12GraphicsCommandList* cmdList,
        const ShadowPassConstants& constants,
        UINT cbIndex);

    ID3D12RootSignature* GetGeometryRootSignature() const { return mGeometryRootSig.Get(); }
    ID3D12PipelineState* GetGeometryPSO()           const { return mGeometryPSO.Get(); }
    ID3D12Resource* GetGeometryCBResource()    const { return mGeomCB->Resource(); }

    ID3D12RootSignature* GetTessellationRootSignature() const { return mTessRootSig.Get(); }
    ID3D12PipelineState* GetTessellationPSO()           const { return mTessPSO.Get(); }

    ID3D12RootSignature* GetShadowRootSignature() const { return mShadowRootSig.Get(); }
    ID3D12PipelineState* GetShadowPSO()           const { return mShadowPSO.Get(); }

    void DoLightingPass(ID3D12GraphicsCommandList* cmdList,
        D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle,
        D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle,
        DirectX::XMFLOAT3 eyePos,
        DirectX::XMFLOAT4X4 invViewProj,
        DirectX::XMFLOAT4X4 invView,
        DirectX::XMFLOAT4X4 invProj,
        D3D12_GPU_DESCRIPTOR_HANDLE depthSrvHandle,
        D3D12_GPU_DESCRIPTOR_HANDLE shadowSrvHandle,
        const DirectX::XMMATRIX* lightViewProjMats,
        const float* splitDepths);

    void DoPostProcessPass(ID3D12GraphicsCommandList* cmdList,
        D3D12_CPU_DESCRIPTOR_HANDLE backBufferRtv,
        bool enableThermal,
        bool enableChromatic,
        bool enableLensFlare,
        DirectX::XMFLOAT2 lightScreenPos,
        bool lightVisible,
        float time);

private:
    void BuildGeometryPassPSO(ID3D12Device* device, DXGI_FORMAT depthStencilFormat);
    void BuildLightingPassPSO(ID3D12Device* device, DXGI_FORMAT backBufferFormat,
        DXGI_FORMAT depthStencilFormat);
    void BuildTessellationPSO(ID3D12Device* device, DXGI_FORMAT depthStencilFormat);
    void BuildShadowPSO(ID3D12Device* device);
    void BuildPostProcessPSO(ID3D12Device* device, DXGI_FORMAT backBufferFormat);
    void BuildRootSignatures(ID3D12Device* device);
    void BuildOffscreenResources(ID3D12Device* device, UINT width, UINT height,
        ID3D12DescriptorHeap* rtvHeap, ID3D12DescriptorHeap* srvHeap);

    GBuffer mGBuffer;

    Microsoft::WRL::ComPtr<ID3D12RootSignature> mGeometryRootSig;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> mLightingRootSig;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> mTessRootSig;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> mShadowRootSig;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> mPostProcessRootSig;

    Microsoft::WRL::ComPtr<ID3D12PipelineState> mGeometryPSO;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mLightingPSO;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mTessPSO;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mShadowPSO;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mPostProcessPSO;

    Microsoft::WRL::ComPtr<ID3DBlob> mGeomVS, mGeomPS;
    Microsoft::WRL::ComPtr<ID3DBlob> mLightVS, mLightPS;
    Microsoft::WRL::ComPtr<ID3DBlob> mTessVS, mTessHS, mTessDS, mTessPS;
    Microsoft::WRL::ComPtr<ID3DBlob> mShadowVS, mShadowPS;
    Microsoft::WRL::ComPtr<ID3DBlob> mPostProcessVS, mPostProcessPS;

    std::unique_ptr<UploadBuffer<GeometryPassConstants>>  mGeomCB;
    std::unique_ptr<UploadBuffer<LightingPassConstants>>  mLightCB;
    std::unique_ptr<UploadBuffer<TessellationConstants>>  mTessCB;
    std::unique_ptr<UploadBuffer<ShadowPassConstants>>    mShadowCB;
    std::unique_ptr<UploadBuffer<PostProcessConstants>>   mPostProcessCB;

    UINT mGeomCBByteSize = 0;
    UINT mShadowCBByteSize = 0;
    static const UINT kMaxGeometryCBs = 4096;
    static const UINT kMaxTessCBs = 512;

    std::vector<LightData> mLights;

    ID3D12DescriptorHeap* mSrvHeap = nullptr;
    UINT                  mGbufferSrvOffset = 0;

    DXGI_FORMAT mBackBufferFormat = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT mDepthStencilFormat = DXGI_FORMAT_UNKNOWN;

    Microsoft::WRL::ComPtr<ID3D12Resource> mOffscreenTex;
    D3D12_CPU_DESCRIPTOR_HANDLE            mOffscreenRtvHandle = {};
    D3D12_GPU_DESCRIPTOR_HANDLE            mOffscreenSrvHandle = {};

    UINT mOffscreenRtvOffset = 0;
    UINT mOffscreenSrvOffset = 0;
};
