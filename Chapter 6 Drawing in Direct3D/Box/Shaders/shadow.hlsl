// Shaders/shadow.hlsl

Texture2D    gDiffuseMap : register(t0);
SamplerState gsamLinear  : register(s0);

cbuffer cbShadowPerObject : register(b0)
{
    float4x4 gWorldViewProj;
    float    gUseAlphaTest;
    float3   gPad;
};

struct VertexIn
{
    float3 PosL    : POSITION;
    float3 NormalL : NORMAL;
    float2 TexC    : TEXCOORD;
    float3 TangentL: TANGENT;
};

struct VertexOut
{
    float4 PosH : SV_POSITION;
    float2 TexC : TEXCOORD;
};

VertexOut VS(VertexIn vin)
{
    VertexOut vout;
    vout.PosH = mul(float4(vin.PosL, 1.0f), gWorldViewProj);
    vout.TexC = vin.TexC;
    return vout;
}

void PS(VertexOut pin)
{
    if (gUseAlphaTest > 0.5f)
    {
        float4 albedo = gDiffuseMap.Sample(gsamLinear, pin.TexC);
        
        // Отбрасываем пиксель, если уровень прозрачности ниже порога
        clip(albedo.a - 0.1f);
    }
}
