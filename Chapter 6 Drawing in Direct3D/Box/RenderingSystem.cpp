#include "RenderingSystem.h"
#include "Common/d3dUtil.h"

using namespace DirectX;
using Microsoft::WRL::ComPtr;

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

    mOffscreenRtvOffset = gbufferRtvOffset + GBuffer::NumRTs;
    mOffscreenSrvOffset = gbufferSrvOffset + GBuffer::NumRTs + 2;

    mGBuffer.Init(device, width, height, rtvHeap, srvHeap, gbufferRtvOffset, gbufferSrvOffset);

    mGeomCBByteSize = d3dUtil::CalcConstantBufferByteSize(sizeof(GeometryPassConstants));
    mShadowCBByteSize = d3dUtil::CalcConstantBufferByteSize(sizeof(ShadowPassConstants));

    mGeomCB = std::make_unique<UploadBuffer<GeometryPassConstants>>(device, kMaxGeometryCBs, true);
    mLightCB = std::make_unique<UploadBuffer<LightingPassConstants>>(device, 1, true);
    mTessCB = std::make_unique<UploadBuffer<TessellationConstants>>(device, kMaxTessCBs, true);
    mShadowCB = std::make_unique<UploadBuffer<ShadowPassConstants>>(device, kMaxGeometryCBs, true);
    mPostProcessCB = std::make_unique<UploadBuffer<PostProcessConstants>>(device, 1, true);

    BuildRootSignatures(device);
    BuildGeometryPassPSO(device, depthStencilFormat);
    BuildLightingPassPSO(device, backBufferFormat, depthStencilFormat);
    BuildTessellationPSO(device, depthStencilFormat);
    BuildShadowPSO(device);
    BuildPostProcessPSO(device, backBufferFormat);

    BuildOffscreenResources(device, width, height, rtvHeap, srvHeap);
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
    BuildOffscreenResources(device, width, height, rtvHeap, srvHeap);
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
    D3D12_GPU_DESCRIPTOR_HANDLE iblSrvHandle, // IBL SRV
    const XMMATRIX* lightViewProjMats,
    const float* splitDepths)
{
    cmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(
        mOffscreenTex.Get(),
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
        D3D12_RESOURCE_STATE_RENDER_TARGET));

    float clearColor[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    cmdList->ClearRenderTargetView(mOffscreenRtvHandle, clearColor, 0, nullptr);

    LightingPassConstants lightConsts = {};
    lightConsts.NumLights = (int)mLights.size();
    lightConsts.EyePosW = eyePos;
    lightConsts.InvViewProj = invViewProj;
    lightConsts.InvView = invView;
    lightConsts.InvProj = invProj;
    for (int i = 0; i < (int)mLights.size(); ++i)
        lightConsts.Lights[i] = mLights[i];

    for (int i = 0; i < 3; ++i)
    {
        XMStoreFloat4x4(&lightConsts.LightViewProj[i], XMMatrixTranspose(lightViewProjMats[i]));
        lightConsts.CascadeEndDepths[i] = splitDepths[i];
    }

    mLightCB->CopyData(0, lightConsts);

    cmdList->OMSetRenderTargets(1, &mOffscreenRtvHandle, true, &dsvHandle);
    cmdList->SetPipelineState(mLightingPSO.Get());
    cmdList->SetGraphicsRootSignature(mLightingRootSig.Get());
    cmdList->SetGraphicsRootConstantBufferView(0, mLightCB->Resource()->GetGPUVirtualAddress());
    cmdList->SetGraphicsRootDescriptorTable(1, mGBuffer.GetSRVTable());
    cmdList->SetGraphicsRootDescriptorTable(2, depthSrvHandle);
    cmdList->SetGraphicsRootDescriptorTable(3, shadowSrvHandle);
    cmdList->SetGraphicsRootDescriptorTable(4, iblSrvHandle); // Назначаем IBL текстуры

    cmdList->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmdList->DrawInstanced(3, 1, 0, 0);

    cmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(
        mOffscreenTex.Get(),
        D3D12_RESOURCE_STATE_RENDER_TARGET,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));
}

void RenderingSystem::DoPostProcessPass(
    ID3D12GraphicsCommandList* cmdList,
    D3D12_CPU_DESCRIPTOR_HANDLE backBufferRtv,
    bool enableThermal,
    bool enableChromatic,
    bool enableLensFlare,
    XMFLOAT2 lightScreenPos,
    bool lightVisible,
    float time)
{
    cmdList->OMSetRenderTargets(1, &backBufferRtv, true, nullptr);

    PostProcessConstants consts = {};
    consts.EnableThermal = enableThermal ? 1 : 0;
    consts.EnableChromatic = enableChromatic ? 1 : 0;
    consts.EnableLensFlare = enableLensFlare ? 1 : 0;
    consts.LightScreenPos = lightScreenPos;
    consts.LightVisible = lightVisible ? 1.0f : 0.0f;
    consts.Time = time;

    mPostProcessCB->CopyData(0, consts);

    cmdList->SetPipelineState(mPostProcessPSO.Get());
    cmdList->SetGraphicsRootSignature(mPostProcessRootSig.Get());
    cmdList->SetGraphicsRootConstantBufferView(0, mPostProcessCB->Resource()->GetGPUVirtualAddress());
    cmdList->SetGraphicsRootDescriptorTable(1, mOffscreenSrvHandle);

    cmdList->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmdList->DrawInstanced(3, 1, 0, 0);
}

void RenderingSystem::BuildRootSignatures(ID3D12Device* device)
{
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

    {
        // Сигнатура для фазы Lighting
        CD3DX12_DESCRIPTOR_RANGE gbufTable;
        gbufTable.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, GBuffer::NumRTs, 0); // t0-t2

        CD3DX12_DESCRIPTOR_RANGE depthTable;
        depthTable.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, GBuffer::NumRTs); // t3

        CD3DX12_DESCRIPTOR_RANGE shadowTable;
        shadowTable.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, GBuffer::NumRTs + 1); // t4

        CD3DX12_DESCRIPTOR_RANGE iblTable;
        iblTable.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, GBuffer::NumRTs + 2); // t5-t7 для IBL

        CD3DX12_ROOT_PARAMETER params[5];
        params[0].InitAsConstantBufferView(0);
        params[1].InitAsDescriptorTable(1, &gbufTable, D3D12_SHADER_VISIBILITY_PIXEL);
        params[2].InitAsDescriptorTable(1, &depthTable, D3D12_SHADER_VISIBILITY_PIXEL);
        params[3].InitAsDescriptorTable(1, &shadowTable, D3D12_SHADER_VISIBILITY_PIXEL);
        params[4].InitAsDescriptorTable(1, &iblTable, D3D12_SHADER_VISIBILITY_PIXEL);

        CD3DX12_STATIC_SAMPLER_DESC samplers[3];
        samplers[0] = CD3DX12_STATIC_SAMPLER_DESC(0, D3D12_FILTER_MIN_MAG_MIP_POINT);
        samplers[1] = CD3DX12_STATIC_SAMPLER_DESC(
            1,
            D3D12_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR,
            D3D12_TEXTURE_ADDRESS_MODE_BORDER,
            D3D12_TEXTURE_ADDRESS_MODE_BORDER,
            D3D12_TEXTURE_ADDRESS_MODE_BORDER,
            0.0f,
            16,
            D3D12_COMPARISON_FUNC_LESS_EQUAL,
            D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE
        );
        samplers[2] = CD3DX12_STATIC_SAMPLER_DESC(2, D3D12_FILTER_MIN_MAG_MIP_LINEAR); // Линейный сэмплер для IBL

        CD3DX12_ROOT_SIGNATURE_DESC desc(5, params, 3, samplers,
            D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

        ComPtr<ID3DBlob> serial, err;
        ThrowIfFailed(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &serial, &err));
        ThrowIfFailed(device->CreateRootSignature(0, serial->GetBufferPointer(),
            serial->GetBufferSize(), IID_PPV_ARGS(&mLightingRootSig)));
    }

    {
        CD3DX12_DESCRIPTOR_RANGE texTable;
        texTable.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 0);

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

    {
        CD3DX12_DESCRIPTOR_RANGE texRange;
        texRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);

        CD3DX12_ROOT_PARAMETER params[2];
        params[0].InitAsConstantBufferView(0);
        params[1].InitAsDescriptorTable(1, &texRange, D3D12_SHADER_VISIBILITY_PIXEL);

        auto sampler = CD3DX12_STATIC_SAMPLER_DESC(0, D3D12_FILTER_MIN_MAG_MIP_LINEAR);
        CD3DX12_ROOT_SIGNATURE_DESC desc(2, params, 1, &sampler,
            D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

        ComPtr<ID3DBlob> serial, err;
        ThrowIfFailed(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &serial, &err));
        ThrowIfFailed(device->CreateRootSignature(0, serial->GetBufferPointer(),
            serial->GetBufferSize(), IID_PPV_ARGS(&mPostProcessRootSig)));
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

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.InputLayout = { nullptr, 0 };
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
    psoDesc.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
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
        serial->GetBufferSize(), IID_PPV_ARGS(&mShadowRootSig)));

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
    psoDesc.PS = { mShadowPS->GetBufferPointer(), mShadowPS->GetBufferSize() };

    psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    psoDesc.RasterizerState.DepthBias = 1500;
    psoDesc.RasterizerState.DepthBiasClamp = 0.0f;
    psoDesc.RasterizerState.SlopeScaledDepthBias = 1.5f;

    psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    psoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 0;
    psoDesc.SampleDesc.Count = 1;
    psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;

    ThrowIfFailed(device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&mShadowPSO)));
}

void RenderingSystem::BuildPostProcessPSO(ID3D12Device* device, DXGI_FORMAT backBufferFmt)
{
    mPostProcessVS = d3dUtil::CompileShader(L"Shaders\\postprocess.hlsl", nullptr, "VS", "vs_5_1");
    mPostProcessPS = d3dUtil::CompileShader(L"Shaders\\postprocess.hlsl", nullptr, "PS", "ps_5_1");

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.InputLayout = { nullptr, 0 };
    psoDesc.pRootSignature = mPostProcessRootSig.Get();
    psoDesc.VS = { mPostProcessVS->GetBufferPointer(), mPostProcessVS->GetBufferSize() };
    psoDesc.PS = { mPostProcessPS->GetBufferPointer(), mPostProcessPS->GetBufferSize() };
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

    ThrowIfFailed(device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&mPostProcessPSO)));
}

void RenderingSystem::BuildOffscreenResources(
    ID3D12Device* device,
    UINT width, UINT height,
    ID3D12DescriptorHeap* rtvHeap,
    ID3D12DescriptorHeap* srvHeap)
{
    mOffscreenTex.Reset();

    D3D12_RESOURCE_DESC texDesc = {};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Alignment = 0;
    texDesc.Width = width;
    texDesc.Height = height;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels = 1;
    texDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    texDesc.SampleDesc.Count = 1;
    texDesc.SampleDesc.Quality = 0;
    texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    texDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_CLEAR_VALUE clearVal = {};
    clearVal.Format = texDesc.Format;
    clearVal.Color[0] = 0.0f;
    clearVal.Color[1] = 0.0f;
    clearVal.Color[2] = 0.0f;
    clearVal.Color[3] = 1.0f;

    auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    ThrowIfFailed(device->CreateCommittedResource(
        &heapProps,
        D3D12_HEAP_FLAG_NONE,
        &texDesc,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
        &clearVal,
        IID_PPV_ARGS(&mOffscreenTex)
    ));

    UINT rtvDescSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    mOffscreenRtvHandle = rtvHeap->GetCPUDescriptorHandleForHeapStart();
    mOffscreenRtvHandle.ptr += (size_t)mOffscreenRtvOffset * rtvDescSize;
    device->CreateRenderTargetView(mOffscreenTex.Get(), nullptr, mOffscreenRtvHandle);

    UINT srvDescSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    CD3DX12_CPU_DESCRIPTOR_HANDLE srvCpuHandle(srvHeap->GetCPUDescriptorHandleForHeapStart());
    srvCpuHandle.Offset(mOffscreenSrvOffset, srvDescSize);
    device->CreateShaderResourceView(mOffscreenTex.Get(), nullptr, srvCpuHandle);

    mOffscreenSrvHandle = srvHeap->GetGPUDescriptorHandleForHeapStart();
    mOffscreenSrvHandle.ptr += (size_t)mOffscreenSrvOffset * srvDescSize;
}
