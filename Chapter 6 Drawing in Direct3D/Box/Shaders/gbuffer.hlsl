// gbuffer.hlsl
// ������� geometry pass (Sponza, ������): ������ albedo (t0).
// ������� � G-buffer � �� ������������ ��������� �������� (��� ������ �������� � heap).
// Normal map + displacement � ��. Shaders/tessellation.hlsl.

Texture2D    gDiffuseMap : register(t0);
SamplerState gsamLinear  : register(s0);

cbuffer cbPerObject : register(b0)
{
    float4x4 gWorldViewProj;
    float4x4 gWorld;
    float4x4 gWorldInvTranspose;
    float     gTime;
    float3    pad;
};

struct VertexIn
{
    float3 PosL    : POSITION;
    float3 NormalL : NORMAL;
    float2 TexC    : TEXCOORD;
    float3 TangentL: TANGENT;   // ��������� tangent
};

struct VertexOut
{
    float4 PosH    : SV_POSITION;
    float3 PosW    : POSITION;
    float3 NormalW : NORMAL;
    float2 TexC    : TEXCOORD;
    float3 TangentW: TANGENT; // �� ������������ � PS (��������� ��� ����� VB)
};

VertexOut VS(VertexIn vin)
{
    VertexOut vout;
    vout.PosH    = mul(float4(vin.PosL, 1.0f), gWorldViewProj);
    vout.PosW    = mul(float4(vin.PosL, 1.0f), gWorld).xyz;
    vout.NormalW = mul(vin.NormalL, (float3x3)gWorldInvTranspose);
    vout.TangentW= mul(vin.TangentL, (float3x3)gWorld);
    vout.TexC    = vin.TexC;
    return vout;
}

struct PSOutput
{
    float4 Albedo   : SV_Target0;
    float4 Normal   : SV_Target1;
    float4 Specular : SV_Target2;
};

PSOutput PS(VertexOut pin)
{
    PSOutput output;

    // Albedo
    float4 albedo = gDiffuseMap.Sample(gsamLinear, pin.TexC);
    if (max(albedo.r, max(albedo.g, albedo.b)) < 0.03f)
        albedo.rgb = float3(0.6f, 0.6f, 0.6f);
    output.Albedo = albedo;

    float3 N = pin.NormalW;
    float nl = length(N);
    N = (nl > 1e-5f) ? (N / nl) : float3(0.0f, 1.0f, 0.0f);
    output.Normal = float4(N, 0.0f);

    output.Specular = float4(0.5f, 0.5f, 0.5f, 0.5f);
    return output;
}
