#pragma once
namespace viewer {
inline constexpr char shader[] = R"(
cbuffer Params : register(b0) {
    row_major float4x4 transform;
    float4 centerRadius, eyeMode, viewport, scroll, options, pointColor;
};
struct Input { float3 position:POSITION; float3 normal:NORMAL; float3 uv:TEXCOORD0; float4 color:COLOR; float hasUV:TEXCOORD1; };
struct Output { float4 position:SV_POSITION; float3 normal:NORMAL; float3 uv:TEXCOORD0; float4 color:COLOR; float3 world:TEXCOORD1; float hasUV:TEXCOORD2; float2 pointCoord:TEXCOORD3; };
Output VS(Input v) {
    Output o=(Output)0;
    float3 p=(v.position-centerRadius.xyz)/centerRadius.w;
    o.position=mul(float4(viewport.z>0.5?float3(v.uv.xy,0):p,1),transform);
    if(viewport.z>0.5 && v.hasUV<0.5) o.position=float4(3,3,3,1);
    o.normal=v.normal; o.uv=v.uv; o.color=v.color; o.world=p; o.hasUV=v.hasUV;
    return o;
}
[maxvertexcount(4)]
void GS(point Output input[1],inout TriangleStream<Output> stream) {
    float2 corners[4]={float2(-1,-1),float2(-1,1),float2(1,-1),float2(1,1)};
    for(int i=0;i<4;i++) {
        Output o=input[0]; o.pointCoord=corners[i];
        o.position.xy+=corners[i]*options.z/viewport.xy*o.position.w;
        o.position.z-=0.0001*o.position.w;
        stream.Append(o);
    }
}
float4 PS(Output v):SV_TARGET {
    if(options.w>0.5 && dot(v.pointCoord,v.pointCoord)>1) discard;
    float3 color;
    if(options.w>1.5) color=pointColor.rgb;
    else if(eyeMode.w<0.5) {
        float3 n=normalize(v.normal);
        float light=0.42+0.24*abs(dot(n,normalize(float3(0.4,0.8,0.6))));
        float fresnel=pow(1-abs(dot(n,normalize(eyeMode.xyz-v.world))),3);
        color=(light+0.22*fresnel).xxx;
        if(viewport.z>0.5) color=float3(0.72,0.75,0.8);
    } else if(eyeMode.w<1.5) {
        if(v.hasUV<0.5) color=float3(1,0,1);
        else {
            float2 uv=(v.uv.xy+scroll.xy*scroll.z)*scroll.w;
            float2 grid=abs(frac(uv-0.5)-0.5)/max(fwidth(uv),0.001);
            float gridLine=1-saturate(min(grid.x,grid.y));
            float checker=fmod(abs(floor(uv.x)+floor(uv.y)),2);
            color=lerp(lerp(float3(0.15,0.24,0.32),float3(0.42,0.65,0.73),checker),float3(0.9,0.96,1),gridLine);
        }
    } else color=v.color.rgb;
    return float4(color,options.x);
}
)";
}
