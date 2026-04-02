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

    ID3D12RootSignature* GetGeometryRootSignature() const { return mGeometryRootSig.Get(); }
    ID3D12PipelineState* GetGeometryPSO()           const { return mGeometryPSO.Get(); }
    ID3D12Resource* GetGeometryCBResource()    const { return mGeomCB->Resource(); }

    void DoLightingPass(ID3D12GraphicsCommandList* cmdList,
        D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle,
        D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle,
        DirectX::XMFLOAT3 eyePos,
        DirectX::XMFLOAT4X4 invViewProj,
        DirectX::XMFLOAT4X4 invView,
        DirectX::XMFLOAT4X4 invProj,
        D3D12_GPU_DESCRIPTOR_HANDLE depthSrvHandle);

private:
    void BuildGeometryPassPSO(ID3D12Device* device,
        DXGI_FORMAT depthStencilFormat);
    void BuildLightingPassPSO(ID3D12Device* device,
        DXGI_FORMAT backBufferFormat,
        DXGI_FORMAT depthStencilFormat);
    void BuildRootSignatures(ID3D12Device* device);
    void BuildFullscreenQuad(ID3D12Device* device,
        ID3D12GraphicsCommandList* cmdList);

    GBuffer mGBuffer;

    Microsoft::WRL::ComPtr<ID3D12RootSignature> mGeometryRootSig;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> mLightingRootSig;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mGeometryPSO;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mLightingPSO;

    Microsoft::WRL::ComPtr<ID3DBlob> mGeomVS, mGeomPS;
    Microsoft::WRL::ComPtr<ID3DBlob> mLightVS, mLightPS;

    std::unique_ptr<UploadBuffer<GeometryPassConstants>>  mGeomCB;
    std::unique_ptr<UploadBuffer<LightingPassConstants>>  mLightCB;
    UINT mGeomCBByteSize = 0;
    static const UINT kMaxGeometryCBs = 512;

    std::vector<LightData> mLights;

    Microsoft::WRL::ComPtr<ID3D12Resource> mQuadVB;
    Microsoft::WRL::ComPtr<ID3D12Resource> mQuadVBUploader;
    D3D12_VERTEX_BUFFER_VIEW               mQuadVBView = {};

    ID3D12DescriptorHeap* mSrvHeap = nullptr;
    UINT                  mGbufferSrvOffset = 0;

    DXGI_FORMAT mBackBufferFormat = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT mDepthStencilFormat = DXGI_FORMAT_UNKNOWN;
};
