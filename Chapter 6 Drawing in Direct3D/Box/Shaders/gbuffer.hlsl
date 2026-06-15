// Shaders/gbuffer.hlsl
Texture2D    gAlbedoMap   : register(t0);
Texture2D    gNormalMap    : register(t1);
Texture2D    gMetallicMap  : register(t2);
Texture2D    gRoughnessMap : register(t3);
SamplerState gsamLinear    : register(s0);

cbuffer cbPerObject : register(b0)
{
    float4x4 gWorldViewProj;
    float4x4 gWorld;
    float4x4 gWorldInvTranspose;
    float     gTime;
    float3    gPad;
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

    if (gPad.x == 2.0f)
    {
        float3 worldCenter = float3(gWorld[3][0], gWorld[3][1], gWorld[3][2]);
        float4 centerH = mul(float4(0.0f, 0.0f, 0.0f, 1.0f), gWorldViewProj);
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
    float4 Specular : SV_Target2; // R = Metallic, G = Roughness, B = AO
};

float3 NormalSampleToWorldSpace(float3 normalMapSample, float3 unitNormalW, float3 tangentW)
{
    float3 normalT = 2.0f * normalMapSample - 1.0f;

    // Создаем ортонормированный базис TBN
    float3 N = unitNormalW;
    float3 T = normalize(tangentW - dot(tangentW, N) * N);
    float3 B = cross(N, T);

    float3x3 TBN = float3x3(T, B, N);

    return normalize(mul(normalT, TBN));
}

PSOutput PS(VertexOut pin)
{
    PSOutput output;

    float4 albedo = gAlbedoMap.Sample(gsamLinear, pin.TexC);
    clip(albedo.a - 0.1f);

    if (max(albedo.r, max(albedo.g, albedo.b)) < 0.03f)
        albedo.rgb = float3(0.6f, 0.6f, 0.6f);
    
    output.Albedo = albedo;

    // Карта нормалей (Рельеф)
    float3 normalMapSample = gNormalMap.Sample(gsamLinear, pin.TexC).rgb;
    float3 bumpedNormalW = NormalSampleToWorldSpace(normalMapSample, normalize(pin.NormalW), normalize(pin.TangentW));
    output.Normal = float4(bumpedNormalW, 0.0f);

    // Считывание параметров PBR
    float metallic  = gMetallicMap.Sample(gsamLinear, pin.TexC).r;
    float roughness = gRoughnessMap.Sample(gsamLinear, pin.TexC).r;

    output.Specular = float4(metallic, roughness, 1.0f, 1.0f); // R=Metallic, G=Roughness, B=AO (default 1.0)
    return output;
}
