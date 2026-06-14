// Shaders/lighting.hlsl
Texture2D          gAlbedo   : register(t0);
Texture2D          gNormal   : register(t1);
Texture2D          gSpecular : register(t2); // R = Metallic, G = Roughness, B = AO
Texture2D<float>   gDepth    : register(t3);
Texture2DArray     gShadowMap: register(t4); // CSM shadow map array

// Текстуры для IBL
TextureCube        gIrradianceMap : register(t5);
TextureCube        gPrefilterMap  : register(t6);
Texture2D          gBRDF_LUT      : register(t7);

SamplerState           gsamPoint  : register(s0);
SamplerComparisonState gsamShadow : register(s1);
SamplerState           gsamLinear : register(s2); // Линейный сэмплер для IBL

#define kMaxLights 64
#define LIGHT_DIRECTIONAL 0
#define LIGHT_POINT       1
#define LIGHT_SPOT        2

static const float PI = 3.14159265359f;

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

    // CSM
    float4x4  gLightViewProj[3];
    float4    gCascadeEndDepths; 
};

struct VertexOut { float4 PosH : SV_POSITION; float2 TexC : TEXCOORD; };

VertexOut VS(uint vertexID : SV_VertexID)
{
    VertexOut vout;
    vout.TexC = float2((vertexID << 1) & 2, vertexID & 2);
    vout.PosH = float4(vout.TexC * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    return vout;
}

float3 ReconstructWorldPos(float2 texC, float depth)
{
    float x = texC.x * 2.0f - 1.0f;
    float y = (1.0f - texC.y) * 2.0f - 1.0f;
    float4 clipPos = float4(x, y, depth, 1.0f);
    float4 viewPos = mul(clipPos, gInvProj);
    float invViewW = (abs(viewPos.w) > 1e-6f) ? (1.0f / viewPos.w) : 0.0f;
    viewPos *= invViewW;
    float4 worldPos = mul(float4(viewPos.xyz, 1.0f), gInvView);
    return worldPos.xyz;
}

float CalcAttenuation(float distance, float range)
{
    float falloff = saturate(1.0f - (distance / range));
    return falloff * falloff;
}

// Функции PBR
float3 fresnelSchlick(float cosTheta, float3 F0)
{
    return F0 + (1.0f - F0) * pow(clamp(1.0f - cosTheta, 0.0f, 1.0f), 5.0f);
}

float3 fresnelSchlickRoughness(float cosTheta, float3 F0, float roughness)
{
    return F0 + (max(float3(1.0f - roughness, 1.0f - roughness, 1.0f - roughness), F0) - F0) * pow(clamp(1.0f - cosTheta, 0.0f, 1.0f), 5.0f);
}

float DistributionGGX(float3 N, float3 H, float roughness)
{
    float a = roughness * roughness;
    float a2 = a * a;
    float NdotH = max(dot(N, H), 0.0f);
    float NdotH2 = NdotH * NdotH;
    float num = a2;
    float denom = (NdotH2 * (a2 - 1.0f) + 1.0f);
    denom = PI * denom * denom;
    return num / max(denom, 0.0001f);
}

float GeometrySchlickGGX(float NdotV, float roughness)
{
    float r = (roughness + 1.0f);
    float k = (r * r) / 8.0f;
    float num = NdotV;
    float denom = NdotV * (1.0f - k) + k;
    return num / denom;
}

float GeometrySmith(float3 N, float3 V, float3 L, float roughness)
{
    float NdotV = max(dot(N, V), 0.0f);
    float NdotL = max(dot(N, L), 0.0f);
    float ggx2 = GeometrySchlickGGX(NdotV, roughness);
    float ggx1 = GeometrySchlickGGX(NdotL, roughness);
    return ggx1 * ggx2;
}

float SampleShadowMap(float3 projCoords, int cascadeIndex)
{
    float shadow = 0.0f;
    float2 texelSize = 1.0f / 2048.0f;
    [unroll]
    for (int x = -1; x <= 1; ++x)
    {
        [unroll]
        for (int y = -1; y <= 1; ++y)
        {
            float2 offset = float2(x, y) * texelSize;
            shadow += gShadowMap.SampleCmpLevelZero(
                gsamShadow, 
                float3(projCoords.xy + offset, (float)cascadeIndex), 
                projCoords.z - 0.0001f 
            );
        }
    }
    return shadow / 9.0f;
}

float4 PS(VertexOut pin) : SV_Target
{
    float depth = gDepth.SampleLevel(gsamPoint, pin.TexC, 0);

    if (depth >= 1.0f)
        return float4(0.0f, 0.0f, 0.0f, 1.0f);

    float3 posW = ReconstructWorldPos(pin.TexC, depth);
    float viewDepth = length(posW - gEyePosW);

    int cascadeIndex = 0;
    if (viewDepth > gCascadeEndDepths.x) cascadeIndex = 1;
    if (viewDepth > gCascadeEndDepths.y) cascadeIndex = 2;

    float4 lightSpacePos = mul(float4(posW, 1.0f), gLightViewProj[cascadeIndex]);
    float3 projCoords = lightSpacePos.xyz / lightSpacePos.w;
    projCoords.x = projCoords.x * 0.5f + 0.5f;
    projCoords.y = -projCoords.y * 0.5f + 0.5f;

    float shadowFactor = 1.0f;
    if (projCoords.z >= 0.0f && projCoords.z <= 1.0f)
    {
        shadowFactor = SampleShadowMap(projCoords, cascadeIndex);
    }

    float4 albedo   = gAlbedo.Sample(gsamPoint, pin.TexC);
    float3 nTex     = gNormal.Sample(gsamPoint, pin.TexC).xyz;
    float3 normal   = (dot(nTex, nTex) > 1e-6f) ? normalize(nTex) : float3(0.0f, 1.0f, 0.0f);
    
    // Свойства материала из G-Buffer
    float4 specData  = gSpecular.Sample(gsamPoint, pin.TexC);
    float  metallic  = specData.r;
    float  roughness = max(specData.g, 0.04f); // избегаем деления на ноль при нулевой шероховатости
    float  ao        = specData.b;

    float3 V = normalize(gEyePosW - posW);
    float3 R = reflect(-V, normal);

    // Базовое отражение F0 (0.04 для диэлектриков, albedo для металлов)
    float3 F0 = float3(0.04f, 0.04f, 0.04f);
    F0 = lerp(F0, albedo.rgb, metallic);

    float3 totalLight = float3(0.0f, 0.0f, 0.0f);

    // 1. Аналитические источники света (Direct Lighting)
    for (int i = 0; i < gNumLights; ++i)
    {
        LightData light = gLights[i];
        float3 L = 0.0f;
        float  atten = 1.0f;

        if (light.Type == LIGHT_DIRECTIONAL)
        {
            L = normalize(-light.Direction);
            atten = shadowFactor; 
        }
        else if (light.Type == LIGHT_POINT)
        {
            float3 toLight = light.Position - posW;
            float  dist    = length(toLight);
            if (dist > light.Range) continue;
            L = toLight / max(dist, 1e-5f);
            atten    = CalcAttenuation(dist, light.Range);
        }
        else if (light.Type == LIGHT_SPOT)
        {
            float3 toLight   = light.Position - posW;
            float  dist      = length(toLight);
            if (dist > light.Range) continue;
            L                = toLight / max(dist, 1e-5f);
            atten            = CalcAttenuation(dist, light.Range);
            float cosAngle   = dot(-L, normalize(light.Direction));
            float cosOuter   = cos(light.SpotAngle);
            float spotFactor = smoothstep(cosOuter, cosOuter + 0.05f, cosAngle);
            atten *= spotFactor;
        }

        float3 H = normalize(V + L);
        float NdotL = max(dot(normal, L), 0.0f);
        float NdotV = max(dot(normal, V), 0.0f);

        float D = DistributionGGX(normal, H, roughness);
        float G = GeometrySmith(normal, V, L, roughness);
        float3 F = fresnelSchlick(max(dot(H, V), 0.0f), F0);

        float3 numerator = D * G * F;
        float denominator = 4.0f * NdotV * NdotL + 0.0001f;
        float3 specular = numerator / denominator;

        float3 kS = F;
        float3 kD = float3(1.0f, 1.0f, 1.0f) - kS;
        kD *= 1.0f - metallic;

        float3 radiance = light.Color * atten;
        totalLight += (kD * albedo.rgb / PI + specular) * radiance * NdotL;
    }

    // 2. Расчет Image-Based Lighting (IBL)
    float3 F_ibl = fresnelSchlickRoughness(max(dot(normal, V), 0.0f), F0, roughness);
    float3 kS_ibl = F_ibl;
    float3 kD_ibl = 1.0f - kS_ibl;
    kD_ibl *= 1.0f - metallic;

    // Диффузная часть IBL (Irradiance)
    float3 irradiance = gIrradianceMap.Sample(gsamLinear, normal).rgb;
    float3 diffuseIBL = irradiance * albedo.rgb;

    // Отраженная часть IBL (Prefilter + BRDF Integration LUT)
    const float MAX_REFLECTION_LOD = 4.0f; // Mip-уровни предрассчитанного кубмапа
    float3 prefilteredColor = gPrefilterMap.SampleLevel(gsamLinear, R, roughness * MAX_REFLECTION_LOD).rgb;
    float2 envBRDF  = gBRDF_LUT.Sample(gsamLinear, float2(max(dot(normal, V), 0.0f), roughness)).rg;
    float3 specularIBL = prefilteredColor * (F_ibl * envBRDF.x + envBRDF.y);

    // Общее окружающее освещение с учетом самозатенения (AO)
    float3 ambient = (kD_ibl * diffuseIBL + specularIBL) * ao;
    totalLight += ambient;

    return float4(totalLight, albedo.a);
}