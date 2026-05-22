#pragma once
#include "Common/d3dUtil.h"
#include "Common/d3dx12.h"

class ShadowMap
{
public:
    static const int NumCascades = 3;

    ShadowMap() = default;
    ~ShadowMap() = default;

    ShadowMap(const ShadowMap&) = delete;
    ShadowMap& operator=(const ShadowMap&) = delete;

    void Init(
        ID3D12Device* device,
        UINT width, UINT height,
        ID3D12DescriptorHeap* dsvHeap, UINT dsvOffset,
        ID3D12DescriptorHeap* srvHeap, UINT srvOffset
    );

    UINT Width() const { return mWidth; }
    UINT Height() const { return mHeight; }

    ID3D12Resource* Resource() const { return mShadowMapArray.Get(); }
    D3D12_GPU_DESCRIPTOR_HANDLE SRV() const { return mSrvGpuHandle; }
    D3D12_CPU_DESCRIPTOR_HANDLE DSV(int index) const { return mDsvCpuHandles[index]; }

    D3D12_VIEWPORT Viewport() const { return mViewport; }
    D3D12_RECT ScissorRect() const { return mScissorRect; }

private:
    UINT mWidth = 0;
    UINT mHeight = 0;

    Microsoft::WRL::ComPtr<ID3D12Resource> mShadowMapArray;
    D3D12_CPU_DESCRIPTOR_HANDLE mDsvCpuHandles[NumCascades] = {};
    D3D12_GPU_DESCRIPTOR_HANDLE mSrvGpuHandle = {};

    D3D12_VIEWPORT mViewport = {};
    D3D12_RECT mScissorRect = {};
}; 
