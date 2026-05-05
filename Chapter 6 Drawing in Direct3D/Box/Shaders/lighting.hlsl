// lighting.hlsl
// Lighting pass: читаем G-buffer и считаем финальный цвет.
// World position восстанавливается из depth buffer через InvView и InvProj.

Texture2D          gAlbedo   : register(t0);
Texture2D          gNormal   : register(t1);
Texture2D          gSpecular : register(t2);
Texture2D<float>   gDepth    : register(t3);
Texture2DArray<float> gShadowMap : register(t4);

SamplerState gsamPoint : register(s0);
SamplerComparisonState gsamShadow : register(s1);

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
    float4x4  gView;

    float4x4  gCascadeShadowTransform[4];
    float4    gCascadeSplits;
    float4    gShadowParams; // x=bias, y=texelSize, z=pcfRadius
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
    // UV -> NDC (D3D): x [-1..1], y [1..-1], z [0..1].
    float x = texC.x * 2.0f - 1.0f;
    float y = (1.0f - texC.y) * 2.0f - 1.0f;

    float4 clipPos = float4(x, y, depth, 1.0f);

    // clip -> view
    float4 viewPos = mul(clipPos, gInvProj);
    float invViewW = (abs(viewPos.w) > 1e-6f) ? (1.0f / viewPos.w) : 0.0f;
    viewPos *= invViewW;

    // view -> world
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

float ComputeShadowPCF(float3 posW, int cascadeIdx)
{
    float4 shadowPosH = mul(float4(posW, 1.0f), gCascadeShadowTransform[cascadeIdx]);
    shadowPosH.xyz /= max(shadowPosH.w, 1e-6f);

    float2 uv = shadowPosH.xy * float2(0.5f, -0.5f) + 0.5f;
    float depth = shadowPosH.z - gShadowParams.x;

    if (uv.x <= 0.0f || uv.x >= 1.0f || uv.y <= 0.0f || uv.y >= 1.0f || depth <= 0.0f || depth >= 1.0f)
        return 1.0f;

    float texel = gShadowParams.y;
    float sum = 0.0f;

    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            float2 o = float2(x, y) * texel;
            sum += gShadowMap.SampleCmpLevelZero(gsamShadow, float3(uv + o, cascadeIdx), depth);
        }
    }

    return sum / 9.0f;
}

float4 PS(VertexOut pin) : SV_Target
{
    // Depth читаем по UV (point sample), чтобы корректно совпадать с G-buffer UV.
    float depth = gDepth.SampleLevel(gsamPoint, pin.TexC, 0);

    if (depth >= 1.0f)
        return float4(0.0f, 0.0f, 0.0f, 1.0f);

    float3 posW = ReconstructWorldPos(pin.TexC, depth);

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
    float viewDepth  = mul(float4(posW, 1.0f), gView).z;
    float shadowNear = ComputeShadowPCF(posW, 0);
    float shadowFar  = ComputeShadowPCF(posW, 1);
    float blendStart = gCascadeSplits.x * 0.90f;
    float blendEnd   = gCascadeSplits.x * 1.10f;
    float tCascade   = saturate((viewDepth - blendStart) / max(blendEnd - blendStart, 1e-4f));
    float shadowTerm = lerp(shadowNear, shadowFar, tCascade);
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
            atten = shadowTerm;
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