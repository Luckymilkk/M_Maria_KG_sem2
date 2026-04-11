#include "RenderingSystem.h"
#include "Common/d3dUtil.h"

using namespace DirectX;
using Microsoft::WRL::ComPtr;

struct QuadVertex
{
    XMFLOAT3 Pos;
    XMFLOAT2 TexC;
};


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
    mBackBufferFormat = backBufferFormat;
    mDepthStencilFormat = depthStencilFormat;
    mSrvHeap = srvHeap;
    mGbufferSrvOffset = gbufferSrvOffset;

    mGBuffer.Init(device, width, height, rtvHeap, srvHeap, gbufferRtvOffset, gbufferSrvOffset);

    mGeomCBByteSize = d3dUtil::CalcConstantBufferByteSize(sizeof(GeometryPassConstants));
    mGeomCB = std::make_unique<UploadBuffer<GeometryPassConstants>>(device, kMaxGeometryCBs, true);
    mLightCB = std::make_unique<UploadBuffer<LightingPassConstants>>(device, 1, true);
    mTessCB = std::make_unique<UploadBuffer<TessellationConstants>>(device, kMaxTessCBs, true);

    BuildRootSignatures(device);
    BuildGeometryPassPSO(device, depthStencilFormat);
    BuildLightingPassPSO(device, backBufferFormat, depthStencilFormat);
    BuildTessellationPSO(device, depthStencilFormat);

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
    mSrvHeap = srvHeap;
    mGbufferSrvOffset = gbufferSrvOffset;
    mGBuffer.OnResize(device, width, height, rtvHeap, srvHeap, gbufferRtvOffset, gbufferSrvOffset);
}


void RenderingSystem::AddDirectionalLight(XMFLOAT3 direction, XMFLOAT3 color, float intensity)
{
    if ((int)mLights.size() >= kMaxLights) return;
    LightData l = {};
    XMStoreFloat3(&l.Direction, XMVector3Normalize(XMLoadFloat3(&direction)));
    l.Color = { color.x * intensity, color.y * intensity, color.z * intensity };
    l.Type = (int)LightType::Directional;
    mLights.push_back(l);
}

void RenderingSystem::AddPointLight(XMFLOAT3 position, XMFLOAT3 color, float intensity, float range)
{
    if ((int)mLights.size() >= kMaxLights) return;
    LightData l = {};
    l.Position = position;
    l.Color = { color.x * intensity, color.y * intensity, color.z * intensity };
    l.Range = range;
    l.Type = (int)LightType::Point;
    mLights.push_back(l);
}

void RenderingSystem::AddSpotLight(XMFLOAT3 position, XMFLOAT3 direction,
    XMFLOAT3 color, float intensity, float range, float spotAngleDegrees)
{
    if ((int)mLights.size() >= kMaxLights) return;
    LightData l = {};
    l.Position = position;
    XMStoreFloat3(&l.Direction, XMVector3Normalize(XMLoadFloat3(&direction)));
    l.Color = { color.x * intensity, color.y * intensity, color.z * intensity };
    l.Range = range;
    l.SpotAngle = XMConvertToRadians(spotAngleDegrees);
    l.Type = (int)LightType::Spot;
    mLights.push_back(l);
}


// ---------------------------------------------------------------------------
//  Geometry pass (без тесселяции)
// ---------------------------------------------------------------------------
void RenderingSystem::BeginGeometryPass(
    ID3D12GraphicsCommandList* cmdList,
    D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle)
{
    mGBuffer.TransitionToWrite(cmdList);
    mGBuffer.ClearRenderTargets(cmdList);
    mGBuffer.BindAsRenderTargets(cmdList, dsvHandle);

    cmdList->SetPipelineState(mGeometryPSO.Get());
    cmdList->SetGraphicsRootSignature(mGeometryRootSig.Get());
    cmdList->SetGraphicsRootConstantBufferView(0, mGeomCB->Resource()->GetGPUVirtualAddress());
}

void RenderingSystem::EndGeometryPass(ID3D12GraphicsCommandList* cmdList)
{
    mGBuffer.TransitionToRead(cmdList);
}

void RenderingSystem::SetGeometryPassConstants(
    ID3D12GraphicsCommandList* cmdList,
    const GeometryPassConstants& constants,
    UINT cbIndex)
{
    if (cbIndex >= kMaxGeometryCBs) cbIndex = kMaxGeometryCBs - 1;

    mGeomCB->CopyData((int)cbIndex, constants);
    D3D12_GPU_VIRTUAL_ADDRESS addr =
        mGeomCB->Resource()->GetGPUVirtualAddress() + (UINT64)cbIndex * mGeomCBByteSize;
    cmdList->SetGraphicsRootConstantBufferView(0, addr);
}

// ---------------------------------------------------------------------------
//  Tessellation pass
// ---------------------------------------------------------------------------
void RenderingSystem::BeginTessellationPass(
    ID3D12GraphicsCommandList* cmdList,
    D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle)
{
 
    mGBuffer.BindAsRenderTargets(cmdList, dsvHandle);

    cmdList->SetPipelineState(mTessPSO.Get());
    cmdList->SetGraphicsRootSignature(mTessRootSig.Get());

  
    cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST);
}

void RenderingSystem::SetTessellationConstants(
    ID3D12GraphicsCommandList* cmdList,
    const GeometryPassConstants& geomConsts,
    UINT geomCbIndex,
    const TessellationConstants& tessConsts,
    UINT tessIndex)
{
    if (geomCbIndex >= kMaxGeometryCBs) geomCbIndex = kMaxGeometryCBs - 1;
    if (tessIndex >= kMaxTessCBs)     tessIndex = kMaxTessCBs - 1;

    mGeomCB->CopyData((int)geomCbIndex, geomConsts);
    mTessCB->CopyData((int)tessIndex, tessConsts);

    UINT geomStride = d3dUtil::CalcConstantBufferByteSize(sizeof(GeometryPassConstants));
    UINT tessStride = d3dUtil::CalcConstantBufferByteSize(sizeof(TessellationConstants));

    D3D12_GPU_VIRTUAL_ADDRESS geomAddr =
        mGeomCB->Resource()->GetGPUVirtualAddress() + (UINT64)geomCbIndex * geomStride;
    D3D12_GPU_VIRTUAL_ADDRESS tessAddr =
        mTessCB->Resource()->GetGPUVirtualAddress() + (UINT64)tessIndex * tessStride;

    // Слот 0: GeometryPassConstants (b0)
    cmdList->SetGraphicsRootConstantBufferView(0, geomAddr);
    // Слот 1: TessellationConstants (b1)
    cmdList->SetGraphicsRootConstantBufferView(1, tessAddr);
    // Слот 2: таблица текстур (diffuse t0, normal t1, displacement t2) — устанавливает вызывающий код
}


// ---------------------------------------------------------------------------
//  Lighting pass
// ---------------------------------------------------------------------------
void RenderingSystem::DoLightingPass(
    ID3D12GraphicsCommandList* cmdList,
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle,
    D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle,
    XMFLOAT3 eyePos,
    XMFLOAT4X4 invViewProj,
    XMFLOAT4X4 invView,
    XMFLOAT4X4 invProj,
    D3D12_GPU_DESCRIPTOR_HANDLE depthSrvHandle)
{
    LightingPassConstants lightConsts = {};
    lightConsts.NumLights = (int)mLights.size();
    lightConsts.EyePosW = eyePos;
    lightConsts.InvViewProj = invViewProj;
    lightConsts.InvView = invView;
    lightConsts.InvProj = invProj;
    for (int i = 0; i < (int)mLights.size(); ++i)
        lightConsts.Lights[i] = mLights[i];

    mLightCB->CopyData(0, lightConsts);

    cmdList->OMSetRenderTargets(1, &rtvHandle, true, &dsvHandle);
    cmdList->SetPipelineState(mLightingPSO.Get());
    cmdList->SetGraphicsRootSignature(mLightingRootSig.Get());
    cmdList->SetGraphicsRootConstantBufferView(0, mLightCB->Resource()->GetGPUVirtualAddress());
    cmdList->SetGraphicsRootDescriptorTable(1, mGBuffer.GetSRVTable());
    cmdList->SetGraphicsRootDescriptorTable(2, depthSrvHandle);

    cmdList->IASetVertexBuffers(0, 1, &mQuadVBView);
    cmdList->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmdList->DrawInstanced(6, 1, 0, 0);
}


// ---------------------------------------------------------------------------
//  Root signatures
// ---------------------------------------------------------------------------
void RenderingSystem::BuildRootSignatures(ID3D12Device* device)
{
    // --- Geometry pass (без тесселяции): только diffuse (t0).
    // Карта нормалей для задания — в tessellation.hlsl (там три SRV подряд).
    {
        CD3DX12_DESCRIPTOR_RANGE texTable;
        texTable.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0); // t0

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

    // --- Lighting pass ---
    {
        CD3DX12_DESCRIPTOR_RANGE gbufTable;
        gbufTable.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, GBuffer::NumRTs, 0);

        CD3DX12_DESCRIPTOR_RANGE depthTable;
        depthTable.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, GBuffer::NumRTs);

        CD3DX12_ROOT_PARAMETER params[3];
        params[0].InitAsConstantBufferView(0);
        params[1].InitAsDescriptorTable(1, &gbufTable, D3D12_SHADER_VISIBILITY_PIXEL);
        params[2].InitAsDescriptorTable(1, &depthTable, D3D12_SHADER_VISIBILITY_PIXEL);

        auto sampler = CD3DX12_STATIC_SAMPLER_DESC(0, D3D12_FILTER_MIN_MAG_MIP_POINT);
        CD3DX12_ROOT_SIGNATURE_DESC desc(3, params, 1, &sampler,
            D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

        ComPtr<ID3DBlob> serial, err;
        ThrowIfFailed(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &serial, &err));
        ThrowIfFailed(device->CreateRootSignature(0, serial->GetBufferPointer(),
            serial->GetBufferSize(), IID_PPV_ARGS(&mLightingRootSig)));
    }


    {
        CD3DX12_DESCRIPTOR_RANGE texTable;
        texTable.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 0); // t0..t2

        CD3DX12_ROOT_PARAMETER params[3];
        params[0].InitAsConstantBufferView(0);                              // b0 GeomCB
        params[1].InitAsConstantBufferView(1);                              // b1 TessCB
        params[2].InitAsDescriptorTable(1, &texTable, D3D12_SHADER_VISIBILITY_ALL);

        // CLAMP убирает «точки» на швах UV при сильной тесселяции; LINEAR — без лишнего mip-шума.
        CD3DX12_STATIC_SAMPLER_DESC tessSampler(
            0,
            D3D12_FILTER_MIN_MAG_MIP_LINEAR,
            D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
            D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
            D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
            0.0f,
            1);
        CD3DX12_ROOT_SIGNATURE_DESC desc(3, params, 1, &tessSampler,
            D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

        ComPtr<ID3DBlob> serial, err;
        ThrowIfFailed(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &serial, &err));
        ThrowIfFailed(device->CreateRootSignature(0, serial->GetBufferPointer(),
            serial->GetBufferSize(), IID_PPV_ARGS(&mTessRootSig)));
    }
}

// ---------------------------------------------------------------------------
//  Geometry PSO (без тесселяции)
// ---------------------------------------------------------------------------
void RenderingSystem::BuildGeometryPassPSO(ID3D12Device* device, DXGI_FORMAT depthFmt)
{
    mGeomVS = d3dUtil::CompileShader(L"Shaders\\gbuffer.hlsl", nullptr, "VS", "vs_5_1");
    mGeomPS = d3dUtil::CompileShader(L"Shaders\\gbuffer.hlsl", nullptr, "PS", "ps_5_1");

    // Добавляем TANGENT в input layout
    std::vector<D3D12_INPUT_ELEMENT_DESC> inputLayout = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TANGENT",  0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.InputLayout = { inputLayout.data(), (UINT)inputLayout.size() };
    psoDesc.pRootSignature = mGeometryRootSig.Get();
    psoDesc.VS = { mGeomVS->GetBufferPointer(), mGeomVS->GetBufferSize() };
    psoDesc.PS = { mGeomPS->GetBufferPointer(), mGeomPS->GetBufferSize() };
    psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
    psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    psoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = GBuffer::NumRTs;
    for (int i = 0; i < GBuffer::NumRTs; ++i)
        psoDesc.RTVFormats[i] = GBuffer::GetFormat(i);
    psoDesc.SampleDesc.Count = 1;
    psoDesc.DSVFormat = depthFmt;

    ThrowIfFailed(device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&mGeometryPSO)));
}

// ---------------------------------------------------------------------------
//  Lighting PSO
// ---------------------------------------------------------------------------
void RenderingSystem::BuildLightingPassPSO(ID3D12Device* device,
    DXGI_FORMAT backBufferFmt, DXGI_FORMAT depthFmt)
{
    mLightVS = d3dUtil::CompileShader(L"Shaders\\lighting.hlsl", nullptr, "VS", "vs_5_1");
    mLightPS = d3dUtil::CompileShader(L"Shaders\\lighting.hlsl", nullptr, "PS", "ps_5_1");

    std::vector<D3D12_INPUT_ELEMENT_DESC> inputLayout = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.InputLayout = { inputLayout.data(), (UINT)inputLayout.size() };
    psoDesc.pRootSignature = mLightingRootSig.Get();
    psoDesc.VS = { mLightVS->GetBufferPointer(), mLightVS->GetBufferSize() };
    psoDesc.PS = { mLightPS->GetBufferPointer(), mLightPS->GetBufferSize() };
    psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);

    auto dsDesc = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    dsDesc.DepthEnable = FALSE;
    dsDesc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    psoDesc.DepthStencilState = dsDesc;

    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = backBufferFmt;
    psoDesc.SampleDesc.Count = 1;
    psoDesc.DSVFormat = depthFmt;

    ThrowIfFailed(device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&mLightingPSO)));
}


void RenderingSystem::BuildTessellationPSO(ID3D12Device* device, DXGI_FORMAT depthFmt)
{
    mTessVS = d3dUtil::CompileShader(L"Shaders\\tessellation.hlsl", nullptr, "VS", "vs_5_1");
    mTessHS = d3dUtil::CompileShader(L"Shaders\\tessellation.hlsl", nullptr, "HS", "hs_5_1");
    mTessDS = d3dUtil::CompileShader(L"Shaders\\tessellation.hlsl", nullptr, "DS", "ds_5_1");
    mTessPS = d3dUtil::CompileShader(L"Shaders\\tessellation.hlsl", nullptr, "PS", "ps_5_1");

    // Тот же layout, что и для обычной геометрии (+ TANGENT)
    std::vector<D3D12_INPUT_ELEMENT_DESC> inputLayout = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TANGENT",  0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.InputLayout = { inputLayout.data(), (UINT)inputLayout.size() };
    psoDesc.pRootSignature = mTessRootSig.Get();
    psoDesc.VS = { mTessVS->GetBufferPointer(), mTessVS->GetBufferSize() };
    psoDesc.HS = { mTessHS->GetBufferPointer(), mTessHS->GetBufferSize() };
    psoDesc.DS = { mTessDS->GetBufferPointer(), mTessDS->GetBufferSize() };
    psoDesc.PS = { mTessPS->GetBufferPointer(), mTessPS->GetBufferSize() };

    psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_WIREFRAME;
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
    psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    psoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    psoDesc.SampleMask = UINT_MAX;

    // Для тесселяции обязательно PATCH
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH;

    psoDesc.NumRenderTargets = GBuffer::NumRTs;
    for (int i = 0; i < GBuffer::NumRTs; ++i)
        psoDesc.RTVFormats[i] = GBuffer::GetFormat(i);
    psoDesc.SampleDesc.Count = 1;
    psoDesc.DSVFormat = depthFmt;

    ThrowIfFailed(device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&mTessPSO)));
}

void RenderingSystem::BuildFullscreenQuad(ID3D12Device* device,
    ID3D12GraphicsCommandList* cmdList)
{
    QuadVertex verts[6] = {
        { { -1.0f,  1.0f, 0.0f }, { 0.0f, 0.0f } },
        { {  1.0f,  1.0f, 0.0f }, { 1.0f, 0.0f } },
        { { -1.0f, -1.0f, 0.0f }, { 0.0f, 1.0f } },
        { { -1.0f, -1.0f, 0.0f }, { 0.0f, 1.0f } },
        { {  1.0f,  1.0f, 0.0f }, { 1.0f, 0.0f } },
        { {  1.0f, -1.0f, 0.0f }, { 1.0f, 1.0f } },
    };

    UINT byteSize = sizeof(verts);
    mQuadVB = d3dUtil::CreateDefaultBuffer(device, cmdList, verts, byteSize, mQuadVBUploader);

    mQuadVBView.BufferLocation = mQuadVB->GetGPUVirtualAddress();
    mQuadVBView.StrideInBytes = sizeof(QuadVertex);
    mQuadVBView.SizeInBytes = byteSize;
}
