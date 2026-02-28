//***************************************************************************************
// color.hlsl by Frank Luna (C) 2015 All Rights Reserved.
//
// Transforms and colors geometry.
//***************************************************************************************

// Массив текстур
Texture2D gDiffuseMap[10] : register(t0);
SamplerState gsamLinear   : register(s0);

// Константный буфер (должен строго совпадать с C++)
cbuffer cbPerObject : register(b0)
{
    float4x4 gWorldViewProj;
    float4x4 gTexTransform;
    float gBlendFactor;   // Наш коэффициент смешивания
    float3 cbPad;         // Пропускаем место, которое мы заполнили в C++
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
    float2 TexC    : TEXCOORD;
};

VertexOut VS(VertexIn vin)
{
    VertexOut vout;
    
    // Трансформируем позицию в гомогенные координаты отсечения
    vout.PosH = mul(float4(vin.PosL, 1.0f), gWorldViewProj);
    
    // Трансформируем текстурные координаты
    vout.TexC = mul(float4(vin.TexC, 0.0f, 1.0f), gTexTransform).xy;
    
    return vout;
}

float4 PS(VertexOut pin) : SV_Target
{
    // Сэмплируем 3 текстуры
    float4 tex1 = gDiffuseMap[0].Sample(gsamLinear, pin.TexC);
    float4 tex2 = gDiffuseMap[1].Sample(gsamLinear, pin.TexC);
    float4 tex3 = gDiffuseMap[2].Sample(gsamLinear, pin.TexC);

    float4 finalColor;

    // Плавная интерполяция
    if (gBlendFactor <= 1.0f)
    {
        finalColor = lerp(tex1, tex2, gBlendFactor);
    }
    else
    {
        finalColor = lerp(tex2, tex3, gBlendFactor - 1.0f);
    }

    return finalColor;
}




