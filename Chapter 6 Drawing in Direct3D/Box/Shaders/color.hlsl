//***************************************************************************************
// color.hlsl by Frank Luna (C) 2015 All Rights Reserved.
//
// Transforms and colors geometry.
//***************************************************************************************

Texture2D gDiffuseMap[10] : register(t0); // Поддержка до 10 текстур сразу
SamplerState gsamLinear   : register(s0);

cbuffer cbPerObject : register(b0) {
    float4x4 gWorldViewProj;
    float4x4 gTexTransform;
    uint gMaterialIndex; // Выбор текстуры по индексу
};

struct VertexIn {
    float3 PosL    : POSITION;
    float3 NormalL : NORMAL;
    float2 TexC    : TEXCOORD;
};

struct VertexOut {
    float4 PosH    : SV_POSITION;
    float2 TexC    : TEXCOORD;
};

VertexOut VS(VertexIn vin) {
    VertexOut vout;
    vout.PosH = mul(float4(vin.PosL, 1.0f), gWorldViewProj);
    // Применяем анимацию и тайлинг к UV координатам
    vout.TexC = mul(float4(vin.TexC, 0.0f, 1.0f), gTexTransform).xy;
    return vout;
}

float4 PS(VertexOut pin) : SV_Target {
    // Сэмплируем нужную текстуру из массива
    return gDiffuseMap[gMaterialIndex].Sample(gsamLinear, pin.TexC);
}


