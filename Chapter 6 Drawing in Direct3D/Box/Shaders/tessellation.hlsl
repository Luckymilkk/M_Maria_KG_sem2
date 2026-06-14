// Shaders/tessellation.hlsl
Texture2D    gDiffuseMap     : register(t0);
Texture2D    gNormalMap      : register(t1);
Texture2D    gDisplaceMap    : register(t2);
SamplerState gsamLinear      : register(s0);

cbuffer cbPerObject : register(b0)
{
    float4x4 gWorldViewProj;
    float4x4 gWorld;
    float4x4 gWorldInvTranspose;
    float     gTime;
    float     gIsAnimated;
    float2    gPad2;
};

cbuffer cbTessellation : register(b1)
{
    float3   gEyePosW;
    float    gDisplaceScale;
    float    gMinTessDist;
    float    gMaxTessDist;
    float    gMinTess;
    float    gMaxTess;
};

struct VertexIn
{
    float3 PosL    : POSITION;
    float3 NormalL : NORMAL;
    float2 TexC    : TEXCOORD;
    float3 TangentL: TANGENT;
};

struct VS_OUT
{
    float3 PosW    : POSITION;
    float3 NormalW : NORMAL;
    float2 TexC    : TEXCOORD;
    float3 TangentW: TANGENT;
};

VS_OUT VS(VertexIn vin)
{
    VS_OUT vout;
    vout.PosW    = mul(float4(vin.PosL, 1.0f), gWorld).xyz;
    vout.NormalW = mul(vin.NormalL, (float3x3)gWorldInvTranspose);
    vout.TangentW= mul(vin.TangentL, (float3x3)gWorld);
    vout.TexC    = vin.TexC;
    return vout;
}

struct HS_TESS_FACTORS
{
    float Edges[3]  : SV_TessFactor;
    float Inside    : SV_InsideTessFactor;
};

struct HS_OUT
{
    float3 PosW    : POSITION;
    float3 NormalW : NORMAL;
    float2 TexC    : TEXCOORD;
    float3 TangentW: TANGENT;
};

float CalcTessFactor(float3 midPointW)
{
    float d = distance(midPointW, gEyePosW);
    float t = saturate((d - gMinTessDist) / max(gMaxTessDist - gMinTessDist, 0.001f));
    return lerp(16.0f, 1.0f, t);
}

HS_TESS_FACTORS ConstantsHS(InputPatch<VS_OUT, 3> patch, uint PatchID : SV_PrimitiveID)
{
    HS_TESS_FACTORS Out;
    float3 e0 = 0.5f * (patch[1].PosW + patch[2].PosW);
    float3 e1 = 0.5f * (patch[2].PosW + patch[0].PosW);
    float3 e2 = 0.5f * (patch[0].PosW + patch[1].PosW);
    float3 centre = (patch[0].PosW + patch[1].PosW + patch[2].PosW) / 3.0f;

    Out.Edges[0] = CalcTessFactor(e0);
    Out.Edges[1] = CalcTessFactor(e1);
    Out.Edges[2] = CalcTessFactor(e2);
    Out.Inside   = CalcTessFactor(centre);
    return Out;
}

[domain("tri")]
[partitioning("fractional_odd")]
[outputtopology("triangle_cw")]
[outputcontrolpoints(3)]
[patchconstantfunc("ConstantsHS")]
[maxtessfactor(64.0)]
HS_OUT HS(InputPatch<VS_OUT, 3> patch, uint uCPID : SV_OutputControlPointID)
{
    HS_OUT Out;
    Out.PosW    = patch[uCPID].PosW;
    Out.NormalW = patch[uCPID].NormalW;
    Out.TexC    = patch[uCPID].TexC;
    Out.TangentW= patch[uCPID].TangentW;
    return Out;
}

struct DS_OUT
{
    float4 PosH    : SV_POSITION;
    float3 PosW    : POSITION;
    float3 NormalW : NORMAL;
    float2 TexC    : TEXCOORD;
    float3 TangentW: TANGENT;
};

[domain("tri")]
DS_OUT DS(HS_TESS_FACTORS input, float3 bary : SV_DomainLocation, const OutputPatch<HS_OUT, 3> tri)
{
    DS_OUT Out;

    float3 posW    = bary.x * tri[0].PosW    + bary.y * tri[1].PosW    + bary.z * tri[2].PosW;
    float3 normalW = bary.x * tri[0].NormalW + bary.y * tri[1].NormalW + bary.z * tri[2].NormalW;
    float2 texC    = bary.x * tri[0].TexC    + bary.y * tri[1].TexC    + bary.z * tri[2].TexC;
    float3 tangW   = bary.x * tri[0].TangentW+ bary.y * tri[1].TangentW+ bary.z * tri[2].TangentW;

    normalW = normalize(normalW);

    float mask = gDisplaceMap.SampleLevel(gsamLinear, texC, 0).r;
    float hDir = (mask * 5.0f) - 1.0f;

    float freq = 5.0f;
    float pulse = (sin(gTime * 2.0f + (texC.x * freq) + (texC.y * freq)) + 1.0f) * 0.5f;

    float offset = hDir * pulse * gDisplaceScale;

    posW += normalW * offset;

    Out.PosW     = posW;
    Out.PosH     = mul(float4(posW, 1.0f), gWorldViewProj);
    Out.NormalW  = normalW;
    Out.TexC     = texC;
    Out.TangentW = normalize(tangW);

    return Out;
}

struct PSOutput
{
    float4 Albedo   : SV_Target0;
    float4 Normal   : SV_Target1;
    float4 Specular : SV_Target2;
};

PSOutput PS(DS_OUT pin)
{
     PSOutput output;

    if (gIsAnimated > 0.5f) 
    {
        float mask = gDisplaceMap.Sample(gsamLinear, pin.TexC).r;
        output.Albedo = float4(mask, 0.0f, 0.0f, 1.0f);
        output.Normal = float4(0, 1, 0, 0); 
        output.Specular = float4(0.0f, 0.9f, 1.0f, 1.0f); // Диэлектрическая шершавая плоскость
    }
    else 
    {
        output.Albedo = gDiffuseMap.Sample(gsamLinear, pin.TexC);
        output.Normal = float4(pin.NormalW, 0.0f);
        output.Specular = float4(0.0f, 0.3f, 1.0f, 1.0f); // Диэлектрическая гладкая поверхность
    }

    return output;
}