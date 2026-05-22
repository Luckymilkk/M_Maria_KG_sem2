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
    float3    gEyePosW;
    float     pad1;
    float4x4  gInvViewProj;
    float4x4  gInvView;
    float4x4  gInvProj;
    float4x4  gView;

    float4x4  gCascadeShadowTransform[4];
    float4    gCascadeSplits;
    float4    gShadowParams; // x=bias
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
    float4 clipPos;
    clipPos.x = texC.x * 2.0f - 1.0f;
    clipPos.y = (1.0f - texC.y) * 2.0f - 1.0f;
    clipPos.z = depth;
    clipPos.w = 1.0f;
    float4 worldPos = mul(clipPos, gInvViewProj);
    return worldPos.xyz / worldPos.w;
}

float CalcShadowFactor(float3 posW, int cascadeIdx)
{
    float4 shadowPosH = mul(float4(posW, 1.0f), gCascadeShadowTransform[cascadeIdx]);
    shadowPosH.xyz /= shadowPosH.w;

    float2 uv = shadowPosH.xy * float2(0.5f, -0.5f) + 0.5f;
    float depth = shadowPosH.z - gShadowParams.x; 

    // PCF 3x3
    float percentLit = 0.0f;
    const float dx = 1.0f / 2048.0f;

    [unroll]
    for(int y = -1; y <= 1; ++y) {
        [unroll]
        for(int x = -1; x <= 1; ++x) {
            percentLit += gShadowMap.SampleCmpLevelZero(gsamShadow, 
                           float3(uv + float2(x, y) * dx, (float)cascadeIdx), depth);
        }
    }
    return percentLit / 9.0f;
}

float4 PS(VertexOut pin) : SV_Target
{
    float depth = gDepth.SampleLevel(gsamPoint, pin.TexC, 0);
    if (depth >= 1.0f) return float4(0.0f, 0.0f, 0.0f, 1.0f);

    float3 posW = ReconstructWorldPos(pin.TexC, depth);
    float4 albedo = gAlbedo.Sample(gsamPoint, pin.TexC);
    float3 normal = normalize(gNormal.Sample(gsamPoint, pin.TexC).xyz);
    float4 specData = gSpecular.Sample(gsamPoint, pin.TexC);

    float3 specColor = specData.rgb;
    float  roughness = specData.a;
    float  shininess = max(1.0f, (1.0f - roughness) * 128.0f);
    float3 toEye = normalize(gEyePosW - posW);

    // Выбор каскада
    float viewZ = mul(float4(posW, 1.0f), gView).z;
    int cascadeIdx = 0;
    if (viewZ > gCascadeSplits.x) cascadeIdx = 1;
    
    float shadow = CalcShadowFactor(posW, cascadeIdx);

    float3 totalLight = albedo.rgb * 0.1f; // Ambient

    for (int i = 0; i < gNumLights; ++i)
    {
        LightData light = gLights[i];
        float3 lightDir;
        float atten = 1.0f;

        if (light.Type == LIGHT_DIRECTIONAL) {
            lightDir = normalize(-light.Direction);
            atten = shadow; 
        } 
        else if (light.Type == LIGHT_POINT) {
            float3 toL = light.Position - posW;
            float d = length(toL);
            if (d > light.Range) continue;
            lightDir = toL / d;
            atten = pow(saturate(1.0f - d/light.Range), 2.0f);
        } 
        else { // SPOT
            float3 toL = light.Position - posW;
            float d = length(toL);
            if (d > light.Range) continue;
            lightDir = toL / d;
            atten = pow(saturate(1.0f - d/light.Range), 2.0f);
            float cosAngle = dot(-lightDir, normalize(light.Direction));
            atten *= smoothstep(cos(light.SpotAngle), cos(light.SpotAngle) + 0.05f, cosAngle);
        }

        // Blinn-Phong
        float3 halfVec = normalize(lightDir + toEye);
        float diff = max(dot(normal, lightDir), 0.0f);
        float spec = pow(max(dot(normal, halfVec), 0.0f), shininess);

        totalLight += atten * (diff * albedo.rgb + spec * specColor) * light.Color;
    }

    return float4(totalLight, albedo.a);
}