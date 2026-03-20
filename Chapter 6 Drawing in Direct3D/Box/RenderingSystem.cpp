#include "RenderingSystem.h"
#include "Common/d3dUtil.h"

using namespace DirectX;
using Microsoft::WRL::ComPtr;

// ============================================================
// Вспомогательная структура для вершины fullscreen quad
// ============================================================
struct QuadVertex
{
    XMFLOAT3 Pos;
    XMFLOAT2 TexC;
};

// ============================================================
// Init
// ============================================================
void RenderingSystem::Init(
    ID3D12Device* device,
    ID3D12GraphicsCommandList* cmdList,
    UINT width, UINT height,
    DXGI_FORMAT backBufferFormat,
    DXGI_FORMAT depthStencilFormat,
    ID3D12DescriptorHeap* rtvHeap,
    ID3D12DescriptorHeap* srvHeap,
    UINT gbufferRtvOffset,
    UINT gbufferSrvOffset)
{
    mBackBufferFormat   = backBufferFormat;
    mDepthStencilFormat = depthStencilFormat;
    mSrvHeap            = srvHeap;
    mGbufferSrvOffset   = gbufferSrvOffset;

    // 1. Инициализируем G-buffer — создаёт текстуры RT0, RT1, RT2
    mGBuffer.Init(device, width, height, rtvHeap, srvHeap, gbufferRtvOffset, gbufferSrvOffset);

    // 2. Создаём константные буферы
    mGeomCB   = std::make_unique<UploadBuffer<GeometryPassConstants>>(device, 1, true);
    mLightCB  = std::make_unique<UploadBuffer<LightingPassConstants>>(device, 1, true);

    // 3. Компилируем шейдеры и создаём PSO
    BuildRootSignatures(device);
    BuildGeometryPassPSO(device, depthStencilFormat);
    BuildLightingPassPSO(device, backBufferFormat, depthStencilFormat);

    // 4. Создаём fullscreen quad для lighting pass
    BuildFullscreenQuad(device, cmdList);
}

void RenderingSystem::OnResize(
    ID3D12Device* device,
    UINT width, UINT height,
    ID3D12DescriptorHeap* rtvHeap,
    ID3D12DescriptorHeap* srvHeap,
    UINT gbufferRtvOffset,
    UINT gbufferSrvOffset)
{
    mSrvHeap          = srvHeap;
    mGbufferSrvOffset = gbufferSrvOffset;
    mGBuffer.OnResize(device, width, height, rtvHeap, srvHeap, gbufferRtvOffset, gbufferSrvOffset);
}

// ============================================================
// Источники света
// ============================================================
void RenderingSystem::AddDirectionalLight(XMFLOAT3 direction, XMFLOAT3 color, float intensity)
{
    if ((int)mLights.size() >= kMaxLights) return;
    LightData l = {};
    XMStoreFloat3(&l.Direction, XMVector3Normalize(XMLoadFloat3(&direction)));
    l.Color = { color.x * intensity, color.y * intensity, color.z * intensity };
    l.Type  = (int)LightType::Directional;
    mLights.push_back(l);
}

void RenderingSystem::AddPointLight(XMFLOAT3 position, XMFLOAT3 color, float intensity, float range)
{
    if ((int)mLights.size() >= kMaxLights) return;
    LightData l = {};
    l.Position = position;
    l.Color    = { color.x * intensity, color.y * intensity, color.z * intensity };
    l.Range    = range;
    l.Type     = (int)LightType::Point;
    mLights.push_back(l);
}

void RenderingSystem::AddSpotLight(XMFLOAT3 position, XMFLOAT3 direction,
    XMFLOAT3 color, float intensity, float range, float spotAngleDegrees)
{
    if ((int)mLights.size() >= kMaxLights) return;
    LightData l = {};
    l.Position  = position;
    XMStoreFloat3(&l.Direction, XMVector3Normalize(XMLoadFloat3(&direction)));
    l.Color      = { color.x * intensity, color.y * intensity, color.z * intensity };
    l.Range      = range;
    l.SpotAngle  = XMConvertToRadians(spotAngleDegrees);
    l.Type       = (int)LightType::Spot;
    mLights.push_back(l);
}

// ============================================================
// Geometry pass
// ============================================================
void RenderingSystem::BeginGeometryPass(
    ID3D12GraphicsCommandList* cmdList,
    D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle)
{
    // Переводим G-buffer текстуры в режим записи
    mGBuffer.TransitionToWrite(cmdList);

    // Очищаем RT'ы
    mGBuffer.ClearRenderTargets(cmdList);

    // Устанавливаем G-buffer как цели рендеринга
    mGBuffer.BindAsRenderTargets(cmdList, dsvHandle);

    // Привязываем geometry PSO и root signature
    cmdList->SetPipelineState(mGeometryPSO.Get());
    cmdList->SetGraphicsRootSignature(mGeometryRootSig.Get());

    // Привязываем константный буфер к слоту b0
    cmdList->SetGraphicsRootConstantBufferView(0, mGeomCB->Resource()->GetGPUVirtualAddress());
}

void RenderingSystem::EndGeometryPass(ID3D12GraphicsCommandList* cmdList)
{
    // Переводим G-buffer в режим чтения для следующего прохода
    mGBuffer.TransitionToRead(cmdList);
}

void RenderingSystem::SetGeometryPassConstants(const GeometryPassConstants& constants)
{
    mGeomCB->CopyData(0, constants);
}

// ============================================================
// Lighting pass
// ============================================================
void RenderingSystem::DoLightingPass(
    ID3D12GraphicsCommandList* cmdList,
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle,
    D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle,
    XMFLOAT3 eyePos)
{
    // --- Заполняем константный буфер освещения ---
    LightingPassConstants lightConsts = {};
    lightConsts.NumLights = (int)mLights.size();
    lightConsts.EyePosW   = eyePos;
    for (int i = 0; i < (int)mLights.size(); ++i)
        lightConsts.Lights[i] = mLights[i];
    mLightCB->CopyData(0, lightConsts);

    // --- Устанавливаем back buffer как RT ---
    cmdList->OMSetRenderTargets(1, &rtvHandle, true, &dsvHandle);

    // --- Привязываем lighting PSO ---
    cmdList->SetPipelineState(mLightingPSO.Get());
    cmdList->SetGraphicsRootSignature(mLightingRootSig.Get());

    // --- Привязываем ресурсы ---
    // Слот 0: константный буфер с данными освещения (b0)
    cmdList->SetGraphicsRootConstantBufferView(0, mLightCB->Resource()->GetGPUVirtualAddress());
    // Слот 1: таблица текстур G-buffer (t0, t1, t2)
    cmdList->SetGraphicsRootDescriptorTable(1, mGBuffer.GetSRVTable());

    // --- Рисуем fullscreen quad ---
    // 6 вершин = 2 треугольника, покрывающих весь экран
    cmdList->IASetVertexBuffers(0, 1, &mQuadVBView);
    cmdList->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmdList->DrawInstanced(6, 1, 0, 0);
}

// ============================================================
// PSO и шейдеры
// ============================================================
void RenderingSystem::BuildRootSignatures(ID3D12Device* device)
{
    // --- Geometry pass root signature ---
    {
        CD3DX12_DESCRIPTOR_RANGE texTable;
        texTable.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);

        CD3DX12_ROOT_PARAMETER params[2];
        params[0].InitAsConstantBufferView(0);
        params[1].InitAsDescriptorTable(1, &texTable, D3D12_SHADER_VISIBILITY_PIXEL);

        auto sampler = CD3DX12_STATIC_SAMPLER_DESC(0, D3D12_FILTER_MIN_MAG_MIP_LINEAR);
        CD3DX12_ROOT_SIGNATURE_DESC desc(2, params, 1, &sampler,
            D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

        ComPtr<ID3DBlob> serial, err;
        ThrowIfFailed(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &serial, &err));
        ThrowIfFailed(device->CreateRootSignature(0, serial->GetBufferPointer(),
            serial->GetBufferSize(), IID_PPV_ARGS(&mGeometryRootSig)));
    }

    // --- Lighting pass root signature ---
    {
        CD3DX12_DESCRIPTOR_RANGE gbufTable;
        gbufTable.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, GBuffer::NumRTs, 0);

        CD3DX12_ROOT_PARAMETER params[2];
        params[0].InitAsConstantBufferView(0);
        params[1].InitAsDescriptorTable(1, &gbufTable, D3D12_SHADER_VISIBILITY_PIXEL);

        auto sampler = CD3DX12_STATIC_SAMPLER_DESC(0, D3D12_FILTER_MIN_MAG_MIP_POINT);
        CD3DX12_ROOT_SIGNATURE_DESC desc(2, params, 1, &sampler,
            D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

        ComPtr<ID3DBlob> serial, err;
        ThrowIfFailed(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &serial, &err));
        ThrowIfFailed(device->CreateRootSignature(0, serial->GetBufferPointer(),
            serial->GetBufferSize(), IID_PPV_ARGS(&mLightingRootSig)));
    }
}
void RenderingSystem::BuildGeometryPassPSO(ID3D12Device* device, DXGI_FORMAT depthFmt)
{
    // Компилируем geometry pass шейдеры
    mGeomVS = d3dUtil::CompileShader(L"Shaders\\gbuffer.hlsl", nullptr, "VS", "vs_5_1");
    mGeomPS = d3dUtil::CompileShader(L"Shaders\\gbuffer.hlsl", nullptr, "PS", "ps_5_1");

    std::vector<D3D12_INPUT_ELEMENT_DESC> inputLayout = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.InputLayout    = { inputLayout.data(), (UINT)inputLayout.size() };
    psoDesc.pRootSignature = mGeometryRootSig.Get();
    psoDesc.VS             = { mGeomVS->GetBufferPointer(), mGeomVS->GetBufferSize() };
    psoDesc.PS             = { mGeomPS->GetBufferPointer(), mGeomPS->GetBufferSize() };
    psoDesc.RasterizerState   = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
    psoDesc.BlendState        = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    psoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    psoDesc.SampleMask        = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;

    // Записываем в 3 RT'а одновременно (MRT)
    psoDesc.NumRenderTargets = GBuffer::NumRTs;
    for (int i = 0; i < GBuffer::NumRTs; ++i)
        psoDesc.RTVFormats[i] = GBuffer::GetFormat(i);

    psoDesc.SampleDesc.Count = 1;
    psoDesc.DSVFormat        = depthFmt;

    ThrowIfFailed(device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&mGeometryPSO)));
}

void RenderingSystem::BuildLightingPassPSO(ID3D12Device* device,
    DXGI_FORMAT backBufferFmt, DXGI_FORMAT depthFmt)
{
    mLightVS = d3dUtil::CompileShader(L"Shaders\\lighting.hlsl", nullptr, "VS", "vs_5_1");
    mLightPS = d3dUtil::CompileShader(L"Shaders\\lighting.hlsl", nullptr, "PS", "ps_5_1");

    // Lighting pass рисует fullscreen quad — те же атрибуты (Pos, TexC)
    std::vector<D3D12_INPUT_ELEMENT_DESC> inputLayout = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.InputLayout    = { inputLayout.data(), (UINT)inputLayout.size() };
    psoDesc.pRootSignature = mLightingRootSig.Get();
    psoDesc.VS             = { mLightVS->GetBufferPointer(), mLightVS->GetBufferSize() };
    psoDesc.PS             = { mLightPS->GetBufferPointer(), mLightPS->GetBufferSize() };
    psoDesc.RasterizerState   = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE; // quad двусторонний
    psoDesc.BlendState        = CD3DX12_BLEND_DESC(D3D12_DEFAULT);

    // Lighting pass не пишет в depth
    auto dsDesc = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    dsDesc.DepthEnable = FALSE;
    dsDesc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    psoDesc.DepthStencilState = dsDesc;

    psoDesc.SampleMask       = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0]    = backBufferFmt; // пишем в back buffer
    psoDesc.SampleDesc.Count = 1;
    psoDesc.DSVFormat        = depthFmt;

    ThrowIfFailed(device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&mLightingPSO)));
}

void RenderingSystem::BuildFullscreenQuad(ID3D12Device* device,
                                           ID3D12GraphicsCommandList* cmdList)
{
    // Два треугольника, покрывающих NDC [-1,1] x [-1,1]
    // UV идут от (0,0) до (1,1)
    QuadVertex verts[6] = {
        { { -1.0f,  1.0f, 0.0f }, { 0.0f, 0.0f } }, // левый верх
        { {  1.0f,  1.0f, 0.0f }, { 1.0f, 0.0f } }, // правый верх
        { { -1.0f, -1.0f, 0.0f }, { 0.0f, 1.0f } }, // левый низ
        { { -1.0f, -1.0f, 0.0f }, { 0.0f, 1.0f } }, // левый низ
        { {  1.0f,  1.0f, 0.0f }, { 1.0f, 0.0f } }, // правый верх
        { {  1.0f, -1.0f, 0.0f }, { 1.0f, 1.0f } }, // правый низ
    };

    UINT byteSize = sizeof(verts);
    mQuadVB = d3dUtil::CreateDefaultBuffer(device, cmdList, verts, byteSize, mQuadVBUploader);

    mQuadVBView.BufferLocation = mQuadVB->GetGPUVirtualAddress();
    mQuadVBView.StrideInBytes  = sizeof(QuadVertex);
    mQuadVBView.SizeInBytes    = byteSize;
}
