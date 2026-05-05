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

// Returns shadow factor [0..1] (0=fully shadowed, 1=fully lit) using 3x3 PCF.
// Returns -1.0 if the point lies outside this cascade's shadow map, so the
// caller can fall back to the next cascade.
float ComputeShadowPCF(float3 posW, int cascadeIdx)
{
    float4 shadowPosH = mul(float4(posW, 1.0f), gCascadeShadowTransform[cascadeIdx]);
    // Perspective divide (orthographic proj keeps w==1, but be safe).
    shadowPosH.xyz /= max(shadowPosH.w, 1e-6f);

    // NDC -> UV.  D3D depth is already [0,1].
    float2 uv    = shadowPosH.xy * float2(0.5f, -0.5f) + 0.5f;
    float  depth = shadowPosH.z - gShadowParams.x; // apply constant bias

    // If the point falls outside the cascade's projection, signal "no coverage".
    // Use a small inner margin (0.02) so PCF taps near the border don't bleed
    // outside the texture (the border sampler returns 1.0 = lit there).
    const float kMargin = 0.02f;
    if (any(uv < kMargin) || any(uv > 1.0f - kMargin) || depth <= 0.0f || depth >= 1.0f)
        return -1.0f;

    float texel = gShadowParams.y;
    float sum   = 0.0f;

    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            float2 o = float2(x, y) * texel;
            sum += gShadowMap.SampleCmpLevelZero(gsamShadow, float3(uv + o, (float)cascadeIdx), depth);
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

    // -----------------------------------------------------------------------
    // Cascade shadow selection.
    // gCascadeSplits.xy = [split0, split1] (view-space Z boundaries).
    // We iterate from the nearest cascade outward and use the first one that
    // covers this pixel.  At the boundary between cascade 0 and 1 we blend
    // over a 10% overlap zone to hide the transition seam.
    // -----------------------------------------------------------------------
    float shadowTerm = 1.0f;
    if (viewDepth > 0.0f)
    {
        // Try cascade 0 first.
        float s0 = ComputeShadowPCF(posW, 0);

        if (s0 >= 0.0f)
        {
            // Point is inside cascade 0.
            // Optionally blend with cascade 1 near the far boundary of cascade 0
            // to avoid a hard pop when crossing to the next cascade.
            float blendStart = gCascadeSplits.x * 0.85f;
            float blendEnd   = gCascadeSplits.x;
            if (viewDepth > blendStart)
            {
                float s1 = ComputeShadowPCF(posW, 1);
                float t  = saturate((viewDepth - blendStart) / max(blendEnd - blendStart, 1e-4f));
                // If cascade 1 didn't cover the point either, fall back to s0 only.
                shadowTerm = (s1 >= 0.0f) ? lerp(s0, s1, t) : s0;
            }
            else
            {
                shadowTerm = s0;
            }
        }
        else
        {
            // Outside cascade 0 — try cascade 1.
            float s1 = ComputeShadowPCF(posW, 1);
            shadowTerm = (s1 >= 0.0f) ? s1 : 1.0f; // beyond all cascades → fully lit
        }
    }
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