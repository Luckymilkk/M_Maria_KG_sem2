#pragma once
#include "Common/d3dUtil.h"
#include "Common/d3dx12.h"
#include "Common/UploadBuffer.h"
#include "GBuffer.h"
#include <vector>

// ============================================================
// Структуры данных для источников света
// ============================================================

enum class LightType : int
{
    Directional = 0, // Направленный (солнце) — нет позиции, только направление
    Point       = 1, // Точечный — есть позиция, светит во все стороны
    Spot        = 2  // Прожектор — позиция + направление + угол конуса
};

// Эта структура передаётся в GPU как часть ConstantBuffer.
// ВАЖНО: выравнивание по 16 байт (правило HLSL cbuffer).
struct LightData
{
    DirectX::XMFLOAT3 Position;   // Мировые координаты (для Point/Spot)
    float             Range;      // Радиус влияния (для Point/Spot)

    DirectX::XMFLOAT3 Direction;  // Направление (для Directional/Spot)
    float             SpotAngle;  // Полуугол конуса прожектора в радианах

    DirectX::XMFLOAT3 Color;      // Цвет × интенсивность
    int               Type;       // LightType как int (HLSL не знает enum)
};

// Константный буфер для geometry pass (совпадает с боксовым ObjectConstants,
// но дополнен матрицами World и View отдельно — они нужны в шейдере для
// преобразования нормалей и позиций в world space).
struct GeometryPassConstants
{
    DirectX::XMFLOAT4X4 WorldViewProj   = MathHelper::Identity4x4();
    DirectX::XMFLOAT4X4 World           = MathHelper::Identity4x4();
    DirectX::XMFLOAT4X4 WorldInvTranspose = MathHelper::Identity4x4(); // для нормалей
};

// Константный буфер для lighting pass
static const int kMaxLights = 16; // Максимум источников света за кадр

struct LightingPassConstants
{
    LightData           Lights[kMaxLights];
    int                 NumLights;
    float               pad0;      // выравнивание до float4
    float               pad1;
    float               pad2;
    DirectX::XMFLOAT3   EyePosW;
    float               pad3;
};

// ============================================================
// Класс RenderingSystem
// ============================================================
class RenderingSystem
{
public:
    RenderingSystem() = default;
    ~RenderingSystem() = default;

    RenderingSystem(const RenderingSystem&) = delete;
    RenderingSystem& operator=(const RenderingSystem&) = delete;

    // Инициализация: создаём PSO, шейдеры, G-buffer, fullscreen quad
    void Init(
        ID3D12Device* device,
        ID3D12GraphicsCommandList* cmdList,
        UINT width, UINT height,
        DXGI_FORMAT backBufferFormat,
        DXGI_FORMAT depthStencilFormat,
        ID3D12DescriptorHeap* rtvHeap,  // общая RTV-куча (backbuffer + gbuffer)
        ID3D12DescriptorHeap* srvHeap,  // общая SRV-куча
        UINT gbufferRtvOffset,          // с какого индекса в rtvHeap начать для gbuffer
        UINT gbufferSrvOffset           // с какого индекса в srvHeap начать для gbuffer
    );

    // Вызывается при изменении размера окна
    void OnResize(
        ID3D12Device* device,
        UINT width, UINT height,
        ID3D12DescriptorHeap* rtvHeap,
        ID3D12DescriptorHeap* srvHeap,
        UINT gbufferRtvOffset,
        UINT gbufferSrvOffset
    );

    // --- Добавление источников света ---
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

    // --- Два прохода ---

    // ПЕРВЫЙ ПРОХОД: рисуем геометрию в G-buffer
    // Вызывается снаружи для каждого меша сцены.
    // cmdList должен быть уже в состоянии записи.
    void BeginGeometryPass(ID3D12GraphicsCommandList* cmdList,
                           D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle);

    void EndGeometryPass(ID3D12GraphicsCommandList* cmdList);

    // Обновляем константный буфер geometry pass для конкретного объекта
    void SetGeometryPassConstants(const GeometryPassConstants& constants);

    // Геттеры для привязки в geometry pass
    ID3D12RootSignature* GetGeometryRootSignature() const { return mGeometryRootSig.Get(); }
    ID3D12PipelineState* GetGeometryPSO()           const { return mGeometryPSO.Get(); }
    ID3D12Resource*      GetGeometryCBResource()    const { return mGeomCB->Resource(); }

    // ВТОРОЙ ПРОХОД: fullscreen quad + lighting
    // dsvHandle — depth stencil view (передаём из основного приложения)
    void DoLightingPass(ID3D12GraphicsCommandList* cmdList,
                        D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle, // back buffer RTV
                        D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle,
                        DirectX::XMFLOAT3 eyePos);

private:
    void BuildGeometryPassPSO(ID3D12Device* device,
                               DXGI_FORMAT depthStencilFormat);
    void BuildLightingPassPSO(ID3D12Device* device,
                               DXGI_FORMAT backBufferFormat,
                               DXGI_FORMAT depthStencilFormat);
    void BuildRootSignatures(ID3D12Device* device);
    void BuildFullscreenQuad(ID3D12Device* device,
                              ID3D12GraphicsCommandList* cmdList);

    // G-buffer (хранит RT'ы)
    GBuffer mGBuffer;

    // PSO и root signature для каждого прохода
    Microsoft::WRL::ComPtr<ID3D12RootSignature> mGeometryRootSig;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> mLightingRootSig;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mGeometryPSO;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mLightingPSO;

    // Шейдеры
    Microsoft::WRL::ComPtr<ID3DBlob> mGeomVS, mGeomPS;
    Microsoft::WRL::ComPtr<ID3DBlob> mLightVS, mLightPS;

    // Константные буферы
    std::unique_ptr<UploadBuffer<GeometryPassConstants>>  mGeomCB;
    std::unique_ptr<UploadBuffer<LightingPassConstants>>  mLightCB;

    // Список источников света на текущий кадр
    std::vector<LightData> mLights;

    // Fullscreen quad (2 треугольника, покрывающие весь экран)
    // Нужен для lighting pass
    Microsoft::WRL::ComPtr<ID3D12Resource> mQuadVB;
    Microsoft::WRL::ComPtr<ID3D12Resource> mQuadVBUploader;
    D3D12_VERTEX_BUFFER_VIEW               mQuadVBView = {};

    // SRV-куча (хранится снаружи, только запоминаем указатель)
    ID3D12DescriptorHeap* mSrvHeap = nullptr;
    UINT                  mGbufferSrvOffset = 0;

    DXGI_FORMAT mBackBufferFormat    = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT mDepthStencilFormat  = DXGI_FORMAT_UNKNOWN;
};
