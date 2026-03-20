#pragma once
#include "Common/d3dUtil.h"
#include "Common/d3dx12.h"

// GBuffer хранит несколько render target'ов, в которые geometry pass
// записывает данные о каждом пикселе сцены.
//
// Раскладка слотов:
//   RT0 (t0 в шейдере): Albedo      — RGBA8_UNORM   (цвет материала)
//   RT1 (t1 в шейдере): Normal      — RGBA16_FLOAT  (нормаль в world space)
//   RT2 (t2 в шейдере): WorldPos    — RGBA32_FLOAT  (позиция в world space)
//
// Depth stencil буфер используется как обычно (D24_UNORM_S8_UINT).

class GBuffer
{
public:
    // Количество render target'ов в G-buffer
    static const int NumRTs = 3;

    GBuffer() = default;
    ~GBuffer() = default;

    // Нельзя копировать — у нас GPU-ресурсы
    GBuffer(const GBuffer&) = delete;
    GBuffer& operator=(const GBuffer&) = delete;

    // Инициализация: создаём текстуры нужного размера
    // device      — наше DX12 устройство
    // width/height — размер экрана
    // rtvHeap     — кучa для RTV (render target views)
    // srvHeap     — куча для SRV (shader resource views, чтобы читать в lighting pass)
    // rtvOffset   — с какого дескриптора начинать в rtvHeap
    // srvOffset   — с какого дескриптора начинать в srvHeap
    void Init(
        ID3D12Device* device,
        UINT width,
        UINT height,
        ID3D12DescriptorHeap* rtvHeap,
        ID3D12DescriptorHeap* srvHeap,
        UINT rtvOffset,
        UINT srvOffset
    );

    // Вызывается при изменении размера окна
    void OnResize(
        ID3D12Device* device,
        UINT width,
        UINT height,
        ID3D12DescriptorHeap* rtvHeap,
        ID3D12DescriptorHeap* srvHeap,
        UINT rtvOffset,
        UINT srvOffset
    );

    // Переводим RT'ы из PRESENT в RENDER_TARGET (перед geometry pass)
    void TransitionToWrite(ID3D12GraphicsCommandList* cmdList);

    // Переводим RT'ы из RENDER_TARGET в PIXEL_SHADER_RESOURCE (перед lighting pass)
    void TransitionToRead(ID3D12GraphicsCommandList* cmdList);

    // Очищаем все RT'ы (вызывается в начале каждого кадра)
    void ClearRenderTargets(ID3D12GraphicsCommandList* cmdList);

    // Устанавливаем G-buffer RT'ы как цели рендеринга
    // dsvHandle — depth stencil view (передаём снаружи)
    void BindAsRenderTargets(ID3D12GraphicsCommandList* cmdList,
                             D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle);

    // Геттеры для RTV handle'ов (чтобы узнать куда писать)
    D3D12_CPU_DESCRIPTOR_HANDLE GetRTV(int index) const { return mRtvHandles[index]; }

    // Геттер для GPU handle первого SRV (для привязки всей таблицы текстур)
    D3D12_GPU_DESCRIPTOR_HANDLE GetSRVTable() const { return mSrvGpuHandle; }

    // Форматы наших RT'ов (нужны при создании PSO)
    static DXGI_FORMAT GetFormat(int index);

private:
    // Вспомогательная функция: создаём одну текстуру-RT
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

    void ResetFirstFrame() { mFirstFrame = true; }

    // Ресурсы (сами текстуры в GPU памяти)
    Microsoft::WRL::ComPtr<ID3D12Resource> mTextures[NumRTs];

    // CPU-хэндлы для RTV (нужны чтобы писать в RT)
    D3D12_CPU_DESCRIPTOR_HANDLE mRtvHandles[NumRTs] = {};

    // GPU-хэндл начала таблицы SRV (нужен чтобы читать в lighting pass)
    D3D12_GPU_DESCRIPTOR_HANDLE mSrvGpuHandle = {};

    UINT mWidth = 0;
    UINT mHeight = 0;
    bool mFirstFrame = true;
};
