// Shaders/lighting.hlsl
Texture2D          gAlbedo   : register(t0);
Texture2D          gNormal   : register(t1);
Texture2D          gSpecular : register(t2);
Texture2D<float>   gDepth    : register(t3);
Texture2DArray     gShadowMap: register(t4); // CSM shadow map array

SamplerState           gsamPoint  : register(s0);
SamplerComparisonState gsamShadow : register(s1); // Сэмплер сравнения для PCF

#define kMaxLights 64
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

    // CSM
    float4x4  gLightViewProj[3];
    float4    gCascadeEndDepths; 
};

struct VertexOut { float4 PosH : SV_POSITION; float2 TexC : TEXCOORD; };

// ГЕНЕРАЦИЯ ПОЛНОЭКРАННОГО ТРЕУГОЛЬНИКА БЕЗ ВЕРШИННОГО БУФЕРА
VertexOut VS(uint vertexID : SV_VertexID)
{
    VertexOut vout;
    // vertexID = 0 -> TexC = (0, 0), PosH = (-1,  1)
    // vertexID = 1 -> TexC = (2, 0), PosH = ( 3,  1)
    // vertexID = 2 -> TexC = (0, 2), PosH = (-1, -3)
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

float3 CalcDiffuse(float3 normal, float3 lightDir, float3 lightColor, bool twoSided)
{
    float NdotL = dot(normal, lightDir);
    NdotL = twoSided ? abs(NdotL) : max(0.0f, NdotL);
    return lightColor * NdotL;
}

float3 CalcSpecular(float3 normal, float3 lightDir, float3 toEye,
                    float3 lightColor, float shininess)
{
    float3 halfVec = normalize(lightDir + toEye);
    float NdotH = max(0.0f, dot(normal, halfVec));
    return lightColor * pow(NdotH, shininess);
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
    float4 specData = gSpecular.Sample(gsamPoint, pin.TexC);

    if (max(albedo.r, max(albedo.g, albedo.b)) < 0.03f)
        albedo.rgb = float3(0.55f, 0.55f, 0.55f);

    float3 specColor = specData.rgb;
    float  roughness = specData.a;
    float  shininess = max(1.0f, (1.0f - roughness) * 128.0f);

    float3 toEye     = normalize(gEyePosW - posW);
    float3 totalLight = albedo.rgb * 0.08f; 

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
            atten = shadowFactor; 
        }
        else if (light.Type == LIGHT_POINT)
        {
            float3 toLight = light.Position - posW;
            float  dist    = length(toLight);
            if (dist > light.Range) continue;
            lightDir = toLight / max(dist, 1e-5f);
            atten    = CalcAttenuation(dist, light.Range);
        }
        else if (light.Type == LIGHT_SPOT)
        {
            float3 toLight   = light.Position - posW;
            float  dist      = length(toLight);
            if (dist > light.Range) continue;
            lightDir         = toLight / max(dist, 1e-5f);
            atten            = CalcAttenuation(dist, light.Range);
            float cosAngle   = dot(-lightDir, normalize(light.Direction));
            float cosOuter   = cos(light.SpotAngle);
            float spotFactor = smoothstep(cosOuter, cosOuter + 0.05f, cosAngle);
            atten *= spotFactor;
        }

        bool twoSided = (light.Type == LIGHT_POINT);
        diffuse  = CalcDiffuse(normal, lightDir, light.Color, twoSided) * atten;
        specular = CalcSpecular(normal, lightDir, toEye, specColor * light.Color, shininess) * atten;
        totalLight += (diffuse * albedo.rgb) + specular;
    }

    return float4(totalLight, albedo.a);
}