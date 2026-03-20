// lighting.hlsl
// Lighting pass: читаем G-buffer и считаем финальный цвет пикселя от всех источников.

// Три текстуры G-buffer (заполнены geometry pass)
Texture2D gAlbedo   : register(t0);
Texture2D gNormal   : register(t1);
Texture2D gWorldPos : register(t2);

SamplerState gsamPoint : register(s0);

#define kMaxLights 16

struct LightData
{
    float3 Position;
    float  Range;
    float3 Direction;
    float  SpotAngle;
    float3 Color;
    int    Type;
};

cbuffer cbLighting : register(b0)
{
    LightData gLights[kMaxLights];
    int       gNumLights;
    float     pad0;
    float     pad1;
    float     pad2;
    float3    gEyePosW;
    float     pad3;
};

struct VertexIn  { float3 PosL : POSITION; float2 TexC : TEXCOORD; };
struct VertexOut { float4 PosH : SV_POSITION; float2 TexC : TEXCOORD; };

VertexOut VS(VertexIn vin)
{
    VertexOut vout;
    vout.PosH = float4(vin.PosL, 1.0f);
    vout.TexC = vin.TexC;
    return vout;
}

float4 PS(VertexOut pin) : SV_Target
{
    // Просто выводим albedo без освещения
    float4 albedo = gAlbedo.Sample(gsamPoint, pin.TexC);
    return albedo;
}