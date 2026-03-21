// lighting.hlsl
// Lighting pass: читаем G-buffer и считаем финальный цвет.
//
// G-buffer раскладка:
//   t0: Albedo   (diffuse цвет)
//   t1: Normal  
//   t2: Specular (RGB=specular, A=roughness)
//
// Позиция восстанавливается из depth buffer по экранным координатам.

Texture2D gAlbedo   : register(t0);
Texture2D gNormal   : register(t1);
Texture2D gSpecular : register(t2);

SamplerState gsamPoint : register(s0);

#define kMaxLights 16
#define LIGHT_DIRECTIONAL 0
#define LIGHT_POINT       1
#define LIGHT_SPOT        2

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

float CalcAttenuation(float distance, float range)
{
    float falloff = saturate(1.0f - (distance / range));
    return falloff * falloff;
}

float3 CalcDiffuse(float3 normal, float3 lightDir, float3 lightColor)
{
    float NdotL = max(0.0f, dot(normal, lightDir));
    return lightColor * NdotL;
}

float3 CalcSpecular(float3 normal, float3 lightDir, float3 toEye,
                    float3 lightColor, float shininess)
{
    float3 halfVec = normalize(lightDir + toEye);
    float NdotH = max(0.0f, dot(normal, halfVec));
    return lightColor * pow(NdotH, shininess);
}

float4 PS(VertexOut pin) : SV_Target
{
    float4 albedo   = gAlbedo.Sample(gsamPoint, pin.TexC);
    float3 normal   = normalize(gNormal.Sample(gsamPoint, pin.TexC).xyz);
    float4 specData = gSpecular.Sample(gsamPoint, pin.TexC);

    float3 specColor = specData.rgb;
    float  roughness = specData.a;
    // Shininess: чем меньше roughness тем острее блик
    float  shininess = max(1.0f, (1.0f - roughness) * 128.0f);

    // Направление к камере(смотрит на центр сцены)
    float3 toEye = normalize(gEyePosW);

    float3 ambient    = albedo.rgb * 0.05f;
    float3 totalLight = ambient;

    for (int i = 0; i < gNumLights; ++i)
    {
        LightData light = gLights[i];
        float3 diffuse  = 0.0f;
        float3 specular = 0.0f;

        if (light.Type == LIGHT_DIRECTIONAL)
        {
            float3 lightDir = normalize(-light.Direction);
            diffuse  = CalcDiffuse(normal, lightDir, light.Color);
            specular = CalcSpecular(normal, lightDir, toEye, specColor * light.Color, shininess);
        }
        else if (light.Type == LIGHT_POINT)
        {
            // Без точной позиции используем направление источника от центра
            float3 lightDir = normalize(light.Position);
            float  dist     = length(light.Position);
            float  atten    = CalcAttenuation(dist, light.Range);

            diffuse  = CalcDiffuse(normal, lightDir, light.Color) * atten;
            specular = CalcSpecular(normal, lightDir, toEye, specColor * light.Color, shininess) * atten;
        }
        else if (light.Type == LIGHT_SPOT)
        {
            float3 lightDir  = normalize(light.Position);
            float  dist      = length(light.Position);
            float  atten     = CalcAttenuation(dist, light.Range);
            float  cosAngle  = dot(-lightDir, normalize(light.Direction));
            float  cosOuter  = cos(light.SpotAngle);
            float  spotFactor = smoothstep(cosOuter, cosOuter + 0.1f, cosAngle);

            diffuse  = CalcDiffuse(normal, lightDir, light.Color) * atten * spotFactor;
            specular = CalcSpecular(normal, lightDir, toEye, specColor * light.Color, shininess) * atten * spotFactor;
        }

        totalLight += diffuse * albedo.rgb + specular;
    }

    return float4(totalLight, albedo.a);
}

