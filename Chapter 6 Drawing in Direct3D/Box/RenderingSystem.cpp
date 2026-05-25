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
    mShadowCBByteSize = d3dUtil::CalcConstantBufferByteSize(sizeof(ShadowPassConstants));

    mGeomCB = std::make_unique<UploadBuffer<GeometryPassConstants>>(device, kMaxGeometryCBs, true);
    mLightCB = std::make_unique<UploadBuffer<LightingPassConstants>>(device, 1, true);
    mTessCB = std::make_unique<UploadBuffer<TessellationConstants>>(device, kMaxTessCBs, true);
    mShadowCB = std::make_unique<UploadBuffer<ShadowPassConstants>>(device, kMaxGeometryCBs, true);

    BuildRootSignatures(device);
    BuildGeometryPassPSO(device, depthStencilFormat);
    BuildLightingPassPSO(device, backBufferFormat, depthStencilFormat);
    BuildTessellationPSO(device, depthStencilFormat);
    BuildShadowPSO(device);

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

    cmdList->SetGraphicsRootConstantBufferView(0, geomAddr);
    cmdList->SetGraphicsRootConstantBufferView(1, tessAddr);
}

// ---- Shadow Pass ----
void RenderingSystem::BeginShadowPass(
    ID3D12GraphicsCommandList* cmdList,
    D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle,
    D3D12_VIEWPORT viewport, D3D12_RECT scissor)
{
    cmdList->RSSetViewports(1, &viewport);
    cmdList->RSSetScissorRects(1, &scissor);
    cmdList->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    cmdList->OMSetRenderTargets(0, nullptr, FALSE, &dsvHandle);

    cmdList->SetPipelineState(mShadowPSO.Get());
    cmdList->SetGraphicsRootSignature(mShadowRootSig.Get());
}

void RenderingSystem::SetShadowPassConstants(
    ID3D12GraphicsCommandList* cmdList,
    const ShadowPassConstants& constants,
    UINT cbIndex)
{
    if (cbIndex >= kMaxGeometryCBs) cbIndex = kMaxGeometryCBs - 1;

    mShadowCB->CopyData((int)cbIndex, constants);
    D3D12_GPU_VIRTUAL_ADDRESS addr =
        mShadowCB->Resource()->GetGPUVirtualAddress() + (UINT64)cbIndex * mShadowCBByteSize;
    cmdList->SetGraphicsRootConstantBufferView(0, addr);
}

void RenderingSystem::DoLightingPass(
    ID3D12GraphicsCommandList* cmdList,
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle,
    D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle,
    XMFLOAT3 eyePos,
    XMFLOAT4X4 invViewProj,
    XMFLOAT4X4 invView,
    XMFLOAT4X4 invProj,
    D3D12_GPU_DESCRIPTOR_HANDLE depthSrvHandle,
    D3D12_GPU_DESCRIPTOR_HANDLE shadowSrvHandle,
    const XMMATRIX* lightViewProjMats,
    const float* splitDepths)
{
    LightingPassConstants lightConsts = {};
    lightConsts.NumLights = (int)mLights.size();
    lightConsts.EyePosW = eyePos;
    lightConsts.InvViewProj = invViewProj;
    lightConsts.InvView = invView;
    lightConsts.InvProj = invProj;
    for (int i = 0; i < (int)mLights.size(); ++i)
        lightConsts.Lights[i] = mLights[i];

    // CSM
    for (int i = 0; i < 3; ++i)
    {
        XMStoreFloat4x4(&lightConsts.LightViewProj[i], XMMatrixTranspose(lightViewProjMats[i]));
        lightConsts.CascadeEndDepths[i] = splitDepths[i];
    }

    mLightCB->CopyData(0, lightConsts);

    cmdList->OMSetRenderTargets(1, &rtvHandle, true, &dsvHandle);
    cmdList->SetPipelineState(mLightingPSO.Get());
    cmdList->SetGraphicsRootSignature(mLightingRootSig.Get());
    cmdList->SetGraphicsRootConstantBufferView(0, mLightCB->Resource()->GetGPUVirtualAddress());
    cmdList->SetGraphicsRootDescriptorTable(1, mGBuffer.GetSRVTable());
    cmdList->SetGraphicsRootDescriptorTable(2, depthSrvHandle);
    cmdList->SetGraphicsRootDescriptorTable(3, shadowSrvHandle); // CSM SRV под t4

    cmdList->IASetVertexBuffers(0, 1, &mQuadVBView);
    cmdList->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmdList->DrawInstanced(6, 1, 0, 0);
}

void RenderingSystem::BuildRootSignatures(ID3D12Device* device)
{
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

    {
        CD3DX12_DESCRIPTOR_RANGE gbufTable;
        gbufTable.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, GBuffer::NumRTs, 0); // t0, t1, t2

        CD3DX12_DESCRIPTOR_RANGE depthTable;
        depthTable.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, GBuffer::NumRTs); // t3

        CD3DX12_DESCRIPTOR_RANGE shadowTable;
        shadowTable.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, GBuffer::NumRTs + 1); // t4 (CSM)

        CD3DX12_ROOT_PARAMETER params[4];
        params[0].InitAsConstantBufferView(0);
        params[1].InitAsDescriptorTable(1, &gbufTable, D3D12_SHADER_VISIBILITY_PIXEL);
        params[2].InitAsDescriptorTable(1, &depthTable, D3D12_SHADER_VISIBILITY_PIXEL);
        params[3].InitAsDescriptorTable(1, &shadowTable, D3D12_SHADER_VISIBILITY_PIXEL);

        // Статические сэмплеры:
        // s0: Point Sampler для G-buffer
        // s1: Comparison Sampler для PCF теней
        CD3DX12_STATIC_SAMPLER_DESC samplers[2];
        samplers[0] = CD3DX12_STATIC_SAMPLER_DESC(0, D3D12_FILTER_MIN_MAG_MIP_POINT);
        samplers[1] = CD3DX12_STATIC_SAMPLER_DESC(
            1, // register s1
            D3D12_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR,
            D3D12_TEXTURE_ADDRESS_MODE_BORDER,
            D3D12_TEXTURE_ADDRESS_MODE_BORDER,
            D3D12_TEXTURE_ADDRESS_MODE_BORDER,
            0.0f,
            16,
            D3D12_COMPARISON_FUNC_LESS_EQUAL,
            D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE
        );

        CD3DX12_ROOT_SIGNATURE_DESC desc(4, params, 2, samplers,
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
        params[0].InitAsConstantBufferView(0);
        params[1].InitAsConstantBufferView(1);
        params[2].InitAsDescriptorTable(1, &texTable, D3D12_SHADER_VISIBILITY_ALL);

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

void RenderingSystem::BuildGeometryPassPSO(ID3D12Device* device, DXGI_FORMAT depthFmt)
{
    mGeomVS = d3dUtil::CompileShader(L"Shaders\\gbuffer.hlsl", nullptr, "VS", "vs_5_1");
    mGeomPS = d3dUtil::CompileShader(L"Shaders\\gbuffer.hlsl", nullptr, "PS", "ps_5_1");

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

    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH;

    psoDesc.NumRenderTargets = GBuffer::NumRTs;
    for (int i = 0; i < GBuffer::NumRTs; ++i)
        psoDesc.RTVFormats[i] = GBuffer::GetFormat(i);
    psoDesc.SampleDesc.Count = 1;
    psoDesc.DSVFormat = depthFmt;

    ThrowIfFailed(device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&mTessPSO)));
}

void RenderingSystem::BuildShadowPSO(ID3D12Device* device)
{
    // Описываем расширенную Root Signature для прохода теней, которая принимает текстуры
    CD3DX12_DESCRIPTOR_RANGE texTable;
    texTable.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0); // t0 - текстура маски (альфа-канала)

    CD3DX12_ROOT_PARAMETER params[2];
    params[0].InitAsConstantBufferView(0); // b0 - константный буфер ShadowPassConstants
    params[1].InitAsDescriptorTable(1, &texTable, D3D12_SHADER_VISIBILITY_PIXEL); // t0 - привязка текстуры в PS

    // Линейный статический сэмплер для выборки из текстуры маски
    auto sampler = CD3DX12_STATIC_SAMPLER_DESC(0, D3D12_FILTER_MIN_MAG_MIP_LINEAR);

    CD3DX12_ROOT_SIGNATURE_DESC desc(2, params, 1, &sampler,
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

    ComPtr<ID3DBlob> serial, err;
    ThrowIfFailed(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &serial, &err));
    ThrowIfFailed(device->CreateRootSignature(0, serial->GetBufferPointer(),
        serial->GetBufferSize(), IID_PPV_ARGS(&mShadowRootSig)));

    // Компилируем и вершинный, и пиксельный шейдеры для теней
    mShadowVS = d3dUtil::CompileShader(L"Shaders\\shadow.hlsl", nullptr, "VS", "vs_5_1");
    mShadowPS = d3dUtil::CompileShader(L"Shaders\\shadow.hlsl", nullptr, "PS", "ps_5_1");

    std::vector<D3D12_INPUT_ELEMENT_DESC> inputLayout = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TANGENT",  0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.InputLayout = { inputLayout.data(), (UINT)inputLayout.size() };
    psoDesc.pRootSignature = mShadowRootSig.Get();
    psoDesc.VS = { mShadowVS->GetBufferPointer(), mShadowVS->GetBufferSize() };
    psoDesc.PS = { mShadowPS->GetBufferPointer(), mShadowPS->GetBufferSize() }; // Связываем пиксельный шейдер

    psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);

    // Настройка смещения для устранения артефактов ("shadow acne")
    psoDesc.RasterizerState.DepthBias = 1500;
    psoDesc.RasterizerState.DepthBiasClamp = 0.0f;
    psoDesc.RasterizerState.SlopeScaledDepthBias = 1.5f;

    psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    psoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 0; // Запись идет только в буфер глубины (карту теней)
    psoDesc.SampleDesc.Count = 1;
    psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;

    ThrowIfFailed(device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&mShadowPSO)));
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
