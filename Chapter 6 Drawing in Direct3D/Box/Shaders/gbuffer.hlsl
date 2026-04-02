// gbuffer.hlsl
// Geometry pass: записываем данные в G-buffer.
// RT0: Albedo (diffuse цвет)
// RT1: Normal 
// RT2: Specular (RGB) + Roughness (A)
// Позиция восстанавливается из depth buffer в lighting pass

Texture2D    gDiffuseMap : register(t0);
SamplerState gsamLinear  : register(s0);

cbuffer cbPerObject : register(b0)
{
    float4x4 gWorldViewProj;
    float4x4 gWorld;
    float4x4 gWorldInvTranspose;
    float     gTime; 
    float3    pad;
};

struct VertexIn
{
    float3 PosL    : POSITION;
    float3 NormalL : NORMAL;
    float2 TexC    : TEXCOORD;
};

struct VertexOut
{
    float4 PosH    : SV_POSITION;
    float3 PosW    : POSITION;
    float3 NormalW : NORMAL;
    float2 TexC    : TEXCOORD;
};

VertexOut VS(VertexIn vin)
{
    VertexOut vout;
    vout.PosH    = mul(float4(vin.PosL, 1.0f), gWorldViewProj);
    vout.PosW    = mul(float4(vin.PosL, 1.0f), gWorld).xyz;
    vout.NormalW = mul(vin.NormalL, (float3x3)gWorldInvTranspose);
    vout.TexC    = vin.TexC;
    return vout;
}

struct PSOutput
{
    float4 Albedo   : SV_Target0; // RT0: diffuse цвет
    float4 Normal   : SV_Target1; // RT1: нормаль в world space
    float4 Specular : SV_Target2; // RT2: specular (RGB) + roughness (A)
};

PSOutput PS(VertexOut pin)
{
    PSOutput output;

    output.Albedo = gDiffuseMap.Sample(gsamLinear, pin.TexC);   
    output.Normal = float4(normalize(pin.NormalW), 0.0f);
    output.Specular = float4(0.5f, 0.5f, 0.5f, 0.5f);

    return output;
}

