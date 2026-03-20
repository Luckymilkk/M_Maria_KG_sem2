#include "GBuffer.h"

// Форматы для каждого из трёх RT'ов
// Index 0: Albedo      — 4 байта (R8G8B8A8), достаточно для цвета
// Index 1: Normal      — 8 байт (16F на канал), нужна точность для нормалей
// Index 2: WorldPos    — 16 байт (32F на канал), нужна высокая точность
DXGI_FORMAT GBuffer::GetFormat(int index)
{
    switch (index)
    {
    case 0: return DXGI_FORMAT_R8G8B8A8_UNORM;    // Albedo
    case 1: return DXGI_FORMAT_R16G16B16A16_FLOAT; // Normal
    case 2: return DXGI_FORMAT_R32G32B32A32_FLOAT; // WorldPos
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

    // Запоминаем GPU-хэндл начала нашей части SRV-кучи.
    // Lighting pass привяжет эту таблицу целиком через SetGraphicsRootDescriptorTable.
    UINT srvDescSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    CD3DX12_GPU_DESCRIPTOR_HANDLE gpuHandle(srvHeap->GetGPUDescriptorHandleForHeapStart());
    gpuHandle.Offset(srvOffset, srvDescSize);
    mSrvGpuHandle = gpuHandle;

    // Создаём по одной текстуре на каждый RT
    for (int i = 0; i < NumRTs; ++i)
    {
        CreateTexture(device, width, height, GetFormat(i),
                      i, rtvHeap, srvHeap, rtvOffset, srvOffset);
    }
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
    // Просто пересоздаём все текстуры с новым размером.
    // Старые ресурсы освободятся автоматически через ComPtr.
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
    // --- Описание текстуры ---
    D3D12_RESOURCE_DESC texDesc = {};
    texDesc.Dimension          = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width              = width;
    texDesc.Height             = height;
    texDesc.DepthOrArraySize   = 1;
    texDesc.MipLevels          = 1;
    texDesc.Format             = format;
    texDesc.SampleDesc.Count   = 1;
    texDesc.SampleDesc.Quality = 0;
    texDesc.Layout             = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    // Флаги: текстура будет и RT (писать), и SRV (читать)
    texDesc.Flags              = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    // Начальное значение при очистке (прозрачный чёрный)
    float clearColor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    D3D12_CLEAR_VALUE clearVal = {};
    clearVal.Format   = format;
    clearVal.Color[0] = clearColor[0];
    clearVal.Color[1] = clearColor[1];
    clearVal.Color[2] = clearColor[2];
    clearVal.Color[3] = clearColor[3];

    // --- Создаём GPU-ресурс в default heap ---
    // D3D12_HEAP_TYPE_DEFAULT — ресурс живёт в GPU-памяти, CPU его не видит.
    // Начальное состояние RENDER_TARGET, потому что geometry pass будет писать в него.
    auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    ThrowIfFailed(device->CreateCommittedResource(
        &heapProps,
        D3D12_HEAP_FLAG_NONE,
        &texDesc,
        D3D12_RESOURCE_STATE_RENDER_TARGET,
        &clearVal,
        IID_PPV_ARGS(&mTextures[index])
    ));

    // --- Создаём RTV (Render Target View) ---
    // RTV нужен, чтобы GPU знал «как писать» в эту текстуру.
    UINT rtvDescSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(rtvHeap->GetCPUDescriptorHandleForHeapStart());
    // Прыгаем на нужный слот в куче
    rtvHandle.Offset(rtvOffset + index, rtvDescSize);
    device->CreateRenderTargetView(mTextures[index].Get(), nullptr, rtvHandle);
    mRtvHandles[index] = rtvHandle; // Запоминаем handle

    // --- Создаём SRV (Shader Resource View) ---
    // SRV нужен, чтобы lighting pass мог читать текстуру как обычный Texture2D.
    UINT srvDescSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    CD3DX12_CPU_DESCRIPTOR_HANDLE srvHandle(srvHeap->GetCPUDescriptorHandleForHeapStart());
    srvHandle.Offset(srvOffset + index, srvDescSize);

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Format                  = format;
    srvDesc.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels     = 1;
    device->CreateShaderResourceView(mTextures[index].Get(), &srvDesc, srvHandle);
}

void GBuffer::TransitionToWrite(ID3D12GraphicsCommandList* cmdList)
{
    // В первом кадре текстуры уже в RENDER_TARGET — переход не нужен
    if (mFirstFrame)
    {
        mFirstFrame = false;
        return;
    }

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
    // Переводим все текстуры из RENDER_TARGET → PIXEL_SHADER_RESOURCE
    // Это нужно делать после geometry pass, перед lighting pass.
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
    // Очищаем каждый RT в начале кадра (как ClearRenderTargetView для back buffer)
    float clearColor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    for (int i = 0; i < NumRTs; ++i)
    {
        cmdList->ClearRenderTargetView(mRtvHandles[i], clearColor, 0, nullptr);
    }
}

void GBuffer::BindAsRenderTargets(ID3D12GraphicsCommandList* cmdList,
                                   D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle)
{
    // Устанавливаем все три RT'а одновременно.
    // Geometry pass будет писать в них через MRT (Multiple Render Targets).
    // В HLSL это будет: SV_Target0 → RT0 (Albedo), SV_Target1 → RT1 (Normal), ...
    cmdList->OMSetRenderTargets(NumRTs, mRtvHandles, FALSE, &dsvHandle);
}
