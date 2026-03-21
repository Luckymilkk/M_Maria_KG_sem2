#include "GBuffer.h"


DXGI_FORMAT GBuffer::GetFormat(int index)
{
    switch (index)
    {
    case 0: return DXGI_FORMAT_R8G8B8A8_UNORM;    // Albedo
    case 1: return DXGI_FORMAT_R16G16B16A16_FLOAT; // Normal
    case 2: return DXGI_FORMAT_R8G8B8A8_UNORM;    // Specular + Roughness
    default: return DXGI_FORMAT_UNKNOWN;
    }
}

void GBuffer::Init(
    ID3D12Device* device,
    UINT width, UINT height,
    ID3D12DescriptorHeap* rtvHeap,
    ID3D12DescriptorHeap* srvHeap,
    UINT rtvOffset,
    UINT srvOffset)
{
    mFirstFrame = true;
    mWidth = width;
    mHeight = height;

    UINT srvDescSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    CD3DX12_GPU_DESCRIPTOR_HANDLE gpuHandle(srvHeap->GetGPUDescriptorHandleForHeapStart());
    gpuHandle.Offset(srvOffset, srvDescSize);
    mSrvGpuHandle = gpuHandle;

    for (int i = 0; i < NumRTs; ++i)
        CreateTexture(device, width, height, GetFormat(i), i, rtvHeap, srvHeap, rtvOffset, srvOffset);
}

void GBuffer::OnResize(
    ID3D12Device* device,
    UINT width, UINT height,
    ID3D12DescriptorHeap* rtvHeap,
    ID3D12DescriptorHeap* srvHeap,
    UINT rtvOffset,
    UINT srvOffset)
{
    mFirstFrame = true;
    for (int i = 0; i < NumRTs; ++i)
        mTextures[i].Reset();
    Init(device, width, height, rtvHeap, srvHeap, rtvOffset, srvOffset);
}

void GBuffer::CreateTexture(
    ID3D12Device* device,
    UINT width, UINT height,
    DXGI_FORMAT format,
    int index,
    ID3D12DescriptorHeap* rtvHeap,
    ID3D12DescriptorHeap* srvHeap,
    UINT rtvOffset,
    UINT srvOffset)
{
    D3D12_RESOURCE_DESC texDesc = {};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width = width;
    texDesc.Height = height;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels = 1;
    texDesc.Format = format;
    texDesc.SampleDesc.Count = 1;
    texDesc.SampleDesc.Quality = 0;
    texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    texDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    float clearColor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    D3D12_CLEAR_VALUE clearVal = {};
    clearVal.Format = format;
    clearVal.Color[0] = clearColor[0];
    clearVal.Color[1] = clearColor[1];
    clearVal.Color[2] = clearColor[2];
    clearVal.Color[3] = clearColor[3];

    auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    ThrowIfFailed(device->CreateCommittedResource(
        &heapProps,
        D3D12_HEAP_FLAG_NONE,
        &texDesc,
        D3D12_RESOURCE_STATE_RENDER_TARGET,
        &clearVal,
        IID_PPV_ARGS(&mTextures[index])
    ));

    UINT rtvDescSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(rtvHeap->GetCPUDescriptorHandleForHeapStart());
    rtvHandle.Offset(rtvOffset + index, rtvDescSize);
    device->CreateRenderTargetView(mTextures[index].Get(), nullptr, rtvHandle);
    mRtvHandles[index] = rtvHandle;

    UINT srvDescSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    CD3DX12_CPU_DESCRIPTOR_HANDLE srvHandle(srvHeap->GetCPUDescriptorHandleForHeapStart());
    srvHandle.Offset(srvOffset + index, srvDescSize);

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Format = format;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(mTextures[index].Get(), &srvDesc, srvHandle);
}

void GBuffer::TransitionToWrite(ID3D12GraphicsCommandList* cmdList)
{
    if (mFirstFrame) { mFirstFrame = false; return; }

    D3D12_RESOURCE_BARRIER barriers[NumRTs];
    for (int i = 0; i < NumRTs; ++i)
    {
        barriers[i] = CD3DX12_RESOURCE_BARRIER::Transition(
            mTextures[i].Get(),
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_RENDER_TARGET
        );
    }
    cmdList->ResourceBarrier(NumRTs, barriers);
}

void GBuffer::TransitionToRead(ID3D12GraphicsCommandList* cmdList)
{
    D3D12_RESOURCE_BARRIER barriers[NumRTs];
    for (int i = 0; i < NumRTs; ++i)
    {
        barriers[i] = CD3DX12_RESOURCE_BARRIER::Transition(
            mTextures[i].Get(),
            D3D12_RESOURCE_STATE_RENDER_TARGET,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
        );
    }
    cmdList->ResourceBarrier(NumRTs, barriers);
}

void GBuffer::ClearRenderTargets(ID3D12GraphicsCommandList* cmdList)
{
    float clearColor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    for (int i = 0; i < NumRTs; ++i)
        cmdList->ClearRenderTargetView(mRtvHandles[i], clearColor, 0, nullptr);
}

void GBuffer::BindAsRenderTargets(ID3D12GraphicsCommandList* cmdList,
    D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle)
{
    cmdList->OMSetRenderTargets(NumRTs, mRtvHandles, FALSE, &dsvHandle);
}
