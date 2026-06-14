// Shaders/postprocess.hlsl
Texture2D gLightOutput : register(t0);
SamplerState gsamLinear : register(s0);

cbuffer cbPostProcess : register(b0)
{
    int gEnableThermal;
    int gEnableChromatic;
    int gEnableLensFlare;
    float gPad0;
    float2 gLightScreenPos;
    float gLightVisible;
    float gTime;
};

struct VertexOut { float4 PosH : SV_POSITION; float2 TexC : TEXCOORD; };

VertexOut VS(uint vertexID : SV_VertexID)
{
    VertexOut vout;
    vout.TexC = float2((vertexID << 1) & 2, vertexID & 2);
    vout.PosH = float4(vout.TexC * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    return vout;
}

float3 GetThermalColor(float intensity)
{
    float3 blue   = float3(0.0f, 0.0f, 0.4f);
    float3 purple = float3(0.4f, 0.0f, 0.6f);
    float3 red    = float3(0.9f, 0.1f, 0.0f);
    float3 yellow = float3(1.0f, 0.9f, 0.0f);
    float3 white  = float3(1.0f, 1.0f, 1.0f);

    if (intensity < 0.25f)
        return lerp(blue, purple, intensity / 0.25f);
    else if (intensity < 0.5f)
        return lerp(purple, red, (intensity - 0.25f) / 0.25f);
    else if (intensity < 0.75f)
        return lerp(red, yellow, (intensity - 0.5f) / 0.25f);
    else
        return lerp(yellow, white, (intensity - 0.75f) / 0.25f);
}

float4 PS(VertexOut pin) : SV_Target
{
    float2 uv = pin.TexC;
    float3 color = float3(0.0f, 0.0f, 0.0f);

    if (gEnableChromatic > 0)
    {
        float2 toCenter = uv - 0.5f;
        float dist = length(toCenter);
        float2 dir = (dist > 1e-5f) ? normalize(toCenter) : float2(1.0f, 0.0f);
        
        float baseAmount = 0.004f; 
        float radialAmount = 0.038f * pow(dist, 1.5f); 
        float amount = baseAmount + radialAmount;
        
        float r = gLightOutput.Sample(gsamLinear, uv - dir * amount).r;
        float g = gLightOutput.Sample(gsamLinear, uv).g;
        float b = gLightOutput.Sample(gsamLinear, uv + dir * amount).b;
        
        color = float3(r, g, b);
    }
    else
    {
        color = gLightOutput.Sample(gsamLinear, uv).rgb;
    }

    if (gEnableThermal > 0)
    {
        float lum = dot(color, float3(0.299f, 0.587f, 0.114f));
        color = GetThermalColor(lum);
    }

    if (gEnableLensFlare > 0 && gLightVisible > 0.5f)
    {
        float2 lightPos = gLightScreenPos;
        float2 center = float2(0.5f, 0.5f);
        float2 lightVec = lightPos - center;

        float2 flarePos[6];
        flarePos[0] = lightPos; 
        flarePos[1] = center - lightVec * 0.3f;
        flarePos[2] = center - lightVec * 0.6f;
        flarePos[3] = center + lightVec * 0.2f;
        flarePos[4] = center - lightVec * 0.9f;
        flarePos[5] = center + lightVec * 0.5f;

        float flareSize[6] = { 0.22f, 0.08f, 0.14f, 0.04f, 0.28f, 0.06f };
        float3 flareColors[6] = {
            float3(1.0f, 0.95f, 0.8f),  
            float3(0.2f, 0.5f, 1.0f),   
            float3(0.8f, 0.2f, 0.6f),   
            float3(0.3f, 0.9f, 0.4f),   
            float3(0.9f, 0.4f, 0.1f),   
            float3(0.2f, 0.8f, 0.9f)    
        };

        float3 flares = float3(0.0f, 0.0f, 0.0f);
        for (int i = 0; i < 6; ++i)
        {
            float d = distance(uv, flarePos[i]);
            float intensity = smoothstep(flareSize[i], 0.0f, d);
            flares += intensity * flareColors[i] * 0.35f;
        }
        
        color += flares;
    }

    color = color / (color + float3(1.0f, 1.0f, 1.0f));

    color = pow(abs(color), 1.0f / 2.2f);

    return float4(color, 1.0f);
}