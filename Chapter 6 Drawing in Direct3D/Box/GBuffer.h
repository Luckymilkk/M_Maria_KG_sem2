#pragma once
#include "Common/d3dUtil.h"
#include "Common/d3dx12.h"


// Раскладка слотов:
//   RT0 (t0 в шейдере): Albedo   — RGBA8_UNORM    (diffuse цвет)
//   RT1 (t1 в шейдере): Normal   — RGBA16_FLOAT   (нормаль в world space)
//   RT2 (t2 в шейдере): Specular — RGBA8_UNORM    (RGB=specular, A=roughness)


class GBuffer
{
public:
    static const int NumRTs = 3;

    GBuffer() = default;
    ~GBuffer() = default;

    GBuffer(const GBuffer&) = delete;
    GBuffer& operator=(const GBuffer&) = delete;

    void Init(
        ID3D12Device* device,
        UINT width,
        UINT height,
        ID3D12DescriptorHeap* rtvHeap,
        ID3D12DescriptorHeap* srvHeap,
        UINT rtvOffset,
        UINT srvOffset
    );

    void OnResize(
        ID3D12Device* device,
        UINT width,
        UINT height,
        ID3D12DescriptorHeap* rtvHeap,
        ID3D12DescriptorHeap* srvHeap,
        UINT rtvOffset,
        UINT srvOffset
    );

    void TransitionToWrite(ID3D12GraphicsCommandList* cmdList);
    void TransitionToRead(ID3D12GraphicsCommandList* cmdList);
    void ClearRenderTargets(ID3D12GraphicsCommandList* cmdList);
    void BindAsRenderTargets(ID3D12GraphicsCommandList* cmdList,
        D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle);

    D3D12_CPU_DESCRIPTOR_HANDLE GetRTV(int index) const { return mRtvHandles[index]; }
    D3D12_GPU_DESCRIPTOR_HANDLE GetSRVTable()     const { return mSrvGpuHandle; }

    static DXGI_FORMAT GetFormat(int index);

private:
    void CreateTexture(
        ID3D12Device* device,
        UINT width, UINT height,
        DXGI_FORMAT format,
        int index,
        ID3D12DescriptorHeap* rtvHeap,
        ID3D12DescriptorHeap* srvHeap,
        UINT rtvOffset,
        UINT srvOffset
    );

    Microsoft::WRL::ComPtr<ID3D12Resource> mTextures[NumRTs];
    D3D12_CPU_DESCRIPTOR_HANDLE            mRtvHandles[NumRTs] = {};
    D3D12_GPU_DESCRIPTOR_HANDLE            mSrvGpuHandle = {};

    UINT mWidth = 0;
    UINT mHeight = 0;
    bool mFirstFrame = true;
};