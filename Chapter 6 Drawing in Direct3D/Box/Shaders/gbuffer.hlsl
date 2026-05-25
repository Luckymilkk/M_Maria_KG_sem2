// gbuffer.hlsl

// Текстуры и семплеры (убедитесь, что они объявлены)
Texture2D    gDiffuseMap : register(t0);
SamplerState gsamLinear  : register(s0);

cbuffer cbPerObject : register(b0)
{
    float4x4 gWorldViewProj;
    float4x4 gWorld;
    float4x4 gWorldInvTranspose;
    float     gTime;
    float3    gPad; // Мы используем gPad.x как флаг (0 - обычный, 2 - билборд)
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
    float4 PosH    : SV_POSITION;
    float3 PosW    : POSITION;
    float3 NormalW : NORMAL;
    float2 TexC    : TEXCOORD;
    float3 TangentW: TANGENT;
};

VertexOut VS(VertexIn vin)
{
    VertexOut vout;

    //БИЛБОРД
    if (gPad.x == 2.0f)
    {

        float3 worldCenter = float3(gWorld[3][0], gWorld[3][1], gWorld[3][2]);

        // проецируем центр объекта
        float4 centerH = mul(float4(0.0f, 0.0f, 0.0f, 1.0f), gWorldViewProj);

        // домножая на W, чтобы сохранить перспективу.
        float2 size = float2(vin.PosL.x, vin.PosL.y);
        centerH.xy += size * 0.5f * centerH.w; 

        vout.PosH = centerH;
        vout.PosW = worldCenter;
        vout.NormalW = float3(0, 0, -1); 
    }
    else
    {
        vout.PosH = mul(float4(vin.PosL, 1.0f), gWorldViewProj);
        vout.PosW = mul(float4(vin.PosL, 1.0f), gWorld).xyz;
        vout.NormalW = mul(vin.NormalL, (float3x3)gWorldInvTranspose);
    }

    vout.TangentW = mul(vin.TangentL, (float3x3)gWorld);
    vout.TexC = vin.TexC;
    return vout;
}

struct PSOutput
{
    float4 Albedo   : SV_Target0;
    float4 Normal   : SV_Target1;
    float4 Specular : SV_Target2;
};

PSOutput PS(VertexOut pin)
{
    PSOutput output;

    float4 albedo = gDiffuseMap.Sample(gsamLinear, pin.TexC);

    // Безусловная отсечка прозрачных пикселей (альфа-тест)
    clip(albedo.a - 0.1f);

    if (max(albedo.r, max(albedo.g, albedo.b)) < 0.03f)
        albedo.rgb = float3(0.6f, 0.6f, 0.6f);
    
    output.Albedo = albedo;

    float3 N = normalize(pin.NormalW);
    output.Normal = float4(N, 0.0f);

    output.Specular = float4(0.5f, 0.5f, 0.5f, 0.5f);
    return output;
}
