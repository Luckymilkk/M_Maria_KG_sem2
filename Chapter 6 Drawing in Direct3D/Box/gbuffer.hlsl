// gbuffer.hlsl
// Geometry pass: трансформируем геометрию и записываем данные в три RT'а G-buffer.

cbuffer cbPerObject : register(b0)
{
    float4x4 gWorldViewProj;
    float4x4 gWorld;
    float4x4 gWorldInvTranspose;
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
    float4 Albedo   : SV_Target0;
    float4 Normal   : SV_Target1;
    float4 WorldPos : SV_Target2;
};

PSOutput PS(VertexOut pin)
{
    PSOutput output;
    output.Albedo   = float4(0.5f, 0.5f, 0.5f, 1.0f);
    output.Normal   = float4(normalize(pin.NormalW), 0.0f);
    output.WorldPos = float4(pin.PosW, 1.0f);
    return output;
}
