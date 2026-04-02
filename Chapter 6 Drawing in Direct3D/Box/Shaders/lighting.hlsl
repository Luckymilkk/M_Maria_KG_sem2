// lighting.hlsl
// Lighting pass: читаем G-buffer и считаем финальный цвет.
//
// G-buffer раскладка:
//   t0: Albedo   (diffuse цвет)
//   t1: Normal  
//   t2: Specular (RGB=specular, A=roughness)
//   t3: Depth buffer (для восстановления world position)
//
// World position восстанавливается из depth buffer по экранным координатам
// через обратную матрицу ViewProj — без хранения позиции в GBuffer.

Texture2D          gAlbedo   : register(t0);
Texture2D          gNormal   : register(t1);
Texture2D          gSpecular : register(t2);
Texture2D<float>   gDepth    : register(t3);

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
    float4x4  gInvViewProj;
    float4x4  gInvView;
    float4x4  gInvProj;
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


float3 ReconstructWorldPos(float2 texC, float depth)
{
    float x = texC.x * 2.0f - 1.0f;
    float y = (1.0f - texC.y) * 2.0f - 1.0f;
    
    float4 ndcPos  = float4(x, y, depth, 1.0f);
    float4 viewPos = mul(ndcPos, gInvProj);
    viewPos        = viewPos / viewPos.w;
    float4 worldPos = mul(gInvView, viewPos);
    return worldPos.xyz;
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
    float depth = gDepth.Load(int3(pin.PosH.xy, 0));

    if (depth >= 1.0f)
        return float4(0.0f, 0.0f, 0.0f, 1.0f);

    float3 posW   = ReconstructWorldPos(pin.TexC, depth);

    float4 albedo   = gAlbedo.Sample(gsamPoint, pin.TexC);
    float3 normal   = normalize(gNormal.Sample(gsamPoint, pin.TexC).xyz);
    float4 specData = gSpecular.Sample(gsamPoint, pin.TexC);

    float3 specColor = specData.rgb;
    float  roughness = specData.a;
    float  shininess = max(1.0f, (1.0f - roughness) * 128.0f);

    float3 toEye     = normalize(gEyePosW - posW);
    float3 totalLight = albedo.rgb * 0.1f;

    for (int i = 0; i < gNumLights; ++i)
    {
        LightData light = gLights[i];
        float3 diffuse  = 0.0f;
        float3 specular = 0.0f;
        float3 lightDir = 0.0f;
        float  atten    = 1.0f;

        if (light.Type == LIGHT_DIRECTIONAL)
        {
            lightDir = normalize(-light.Direction);
        }
        else if (light.Type == LIGHT_POINT)
        {
            float3 toLight = light.Position - posW;
            float  dist    = length(toLight);
            if (dist > light.Range) continue;
            lightDir = toLight / dist;
            atten    = CalcAttenuation(dist, light.Range);
        }
        else if (light.Type == LIGHT_SPOT)
        {
            float3 toLight = light.Position - posW;
            float  dist    = length(toLight);
            if (dist > light.Range) continue;
            lightDir     = toLight / dist;
            atten        = CalcAttenuation(dist, light.Range);
            float cosAngle  = dot(-lightDir, normalize(light.Direction));
            float cosOuter  = cos(light.SpotAngle);
            float spotFactor = smoothstep(cosOuter, cosOuter + 0.05f, cosAngle);
            atten *= spotFactor;
        }

        diffuse  = CalcDiffuse(normal, lightDir, light.Color) * atten;
        specular = CalcSpecular(normal, lightDir, toEye, specColor * light.Color, shininess) * atten;
        totalLight += (diffuse * albedo.rgb) + specular;
    }

    return float4(totalLight, albedo.a);
}