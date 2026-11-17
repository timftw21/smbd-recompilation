#include "shaders.h"
#include <d3dcommon.h>
#ifndef _WIN32
#include "gui_shaders.h"
#endif
#include <atomic>
#include <format>
#include <stdexcept>

namespace smbd {
namespace {
class ShaderBlob final : public ID3DBlob {
    Bytes bytes_;std::atomic<ULONG> references_=1;
public:
    explicit ShaderBlob(Bytes bytes):bytes_(bytes){}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** result) override {
        if(!result) return E_POINTER;
        *result=nullptr;
        if(iid!=__uuidof(IUnknown) && iid!=__uuidof(ID3DBlob)) return E_NOINTERFACE;
        *result=this;AddRef();return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {return references_.fetch_add(1,std::memory_order_relaxed)+1;}
    ULONG STDMETHODCALLTYPE Release() override {
        const auto count=references_.fetch_sub(1,std::memory_order_acq_rel)-1;if(!count) delete this;return count;
    }
    void* STDMETHODCALLTYPE GetBufferPointer() override {return const_cast<std::byte*>(bytes_.data());}
    SIZE_T STDMETHODCALLTYPE GetBufferSize() override {return bytes_.size();}
};
// Shared by the HUD upscaler's compute passes; mirrored by InterfaceJob.
constexpr std::string_view interface_job_source=R"(
cbuffer Job:register(b0) {
 int2 image;int2 origin;int2 extent;int2 halo;int2 interior;int2 slots;uint mode;uint factor;int2 target;
};
)";
// One layer and group of 16 output channels: weights[(tap*inputs+input)*4+k]
// holds outputs k*4..k*4+3; the final linear layer stores unit slopes. Each
// batch slot (one tile's colour or alpha pass) owns 16 consecutive slices.
constexpr std::string_view interface_layer_source=R"(
cbuffer Layer:register(b1) {float4 weights[2304];float4 bias[4];float4 slope[4];uint4 layer;};
RWTexture2DArray<float4> features_out:register(u0);
float4 prelu(float4 a,float4 s) {return max(a,0)+min(a,0)*s;}
void store(int2 q,uint slot,float4 a0,float4 a1,float4 a2,float4 a3) {
 uint s=slot*16+layer.x;
 features_out[uint3(q,s)]=prelu(a0,slope[0]);features_out[uint3(q,s+1)]=prelu(a1,slope[1]);
 features_out[uint3(q,s+2)]=prelu(a2,slope[2]);features_out[uint3(q,s+3)]=prelu(a3,slope[3]);
}
)";
constexpr std::string_view combiner_source=R"(
struct P {float4 p:SV_Position;float4 c:COLOR0;float4 s:COLOR1;
 float4 t0:TEXCOORD0;float4 t1:TEXCOORD1;float4 t2:TEXCOORD2;float4 t3:TEXCOORD3;float fog:TEXCOORD4;};
cbuffer Factors:register(b0) {
 float4 factor0[8];float4 factor1[8];float4 final0;float4 final1;float4 fog;float4 alpha;
 uint4 combiners[8];uint4 stages[4];uint4 controls;
 float4 bump_matrices[4];
 uint4 gaussian_scale;
 float4 sample_bounds[4];uint4 bounded_samples;
};
SamplerState sm0:register(s0);SamplerState sm1:register(s1);SamplerState sm2:register(s2);SamplerState sm3:register(s3);
Texture2D tx0:register(t0);Texture2D tx1:register(t1);Texture2D tx2:register(t2);Texture2D tx3:register(t3);
TextureCube cube0:register(t4);TextureCube cube1:register(t5);TextureCube cube2:register(t6);TextureCube cube3:register(t7);
Texture3D vol0:register(t8);Texture3D vol1:register(t9);Texture3D vol2:register(t10);Texture3D vol3:register(t11);
float4 finish(float4 result) {
 float a=round(saturate(result.a)*255);uint func=controls.w;
 bool accepted=func==0x207 || (func==0x201 && a<alpha.x) || (func==0x202 && a==alpha.x) ||
  (func==0x203 && a<=alpha.x) || (func==0x204 && a>alpha.x) || (func==0x205 && a!=alpha.x) || (func==0x206 && a>=alpha.x);
 if(!accepted) discard;return result;
}
float4 sample2d(Texture2D image,SamplerState sm,float2 uv,uint scale) {
 if(!scale) return image.Sample(sm,uv);
 uint width,height;image.GetDimensions(width,height);
 float2 texel=float(scale)/float2(width,height);float4 value=0;
 // Separable 3x3 Gaussian: [1 2 1] in each axis, normalized by 16.
 [unroll] for(int y=-1;y<=1;++y) {
  [unroll] for(int x=-1;x<=1;++x)
   value+=image.SampleLevel(sm,uv+float2(x,y)*texel,0)*(x==0?2:1)*(y==0?2:1);
 }
 return value/16;
}
struct R {float4 fog,c,s,t0,t1,t2,t3,r0,r1,sum,ef;};
float4 mapped(float4 v,uint word) {
 uint m=word>>5;
 if(m==0) return max(v,0);if(m==1) return 1-saturate(v);
 if(m==2) return 2*max(v,0)-1;if(m==3) return 1-2*max(v,0);
 if(m==4) return max(v,0)-0.5;if(m==5) return 0.5-max(v,0);
 return m==6?v:-v;
}
float4 source(R r,uint word,uint stage,bool final) {
 uint reg=word&15;
 if(reg==1) return final?final0:factor0[(controls.x&0x1000)?stage:0];
 if(reg==2) return final?final1:factor1[(controls.x&0x10000)?stage:0];
 if(reg==3) return r.fog;if(reg==4) return r.c;if(reg==5) return r.s;
 if(reg==8) return r.t0;if(reg==9) return r.t1;if(reg==10) return r.t2;if(reg==11) return r.t3;
 if(reg==12) return r.r0;if(reg==13) return r.r1;if(reg==14) return r.sum;if(reg==15) return r.ef;
 return 0;
}
float3 rgb(R r,uint word,uint stage,bool final) {
 float4 v=source(r,word,stage,final);return mapped((word&16)?v.aaaa:v,word).rgb;
}
float alp(R r,uint word,uint stage,bool final) {
 float4 v=source(r,word,stage,final);return mapped((word&16)?v.aaaa:v.bbbb,word).x;
}
float4 outmap(float4 v,uint flags) {
 uint m=flags&0x38;if(m==8) return v-0.5;if(m==16) return v*2;
 if(m==24) return (v-0.5)*2;if(m==32) return v*4;if(m==48) return v*0.5;return v;
}
void write_rgb(inout R r,uint destination,float3 v) {
 v=clamp(v,-1,1);if(destination==4) r.c.rgb=v;if(destination==5) r.s.rgb=v;
 if(destination==8) r.t0.rgb=v;if(destination==9) r.t1.rgb=v;if(destination==10) r.t2.rgb=v;if(destination==11) r.t3.rgb=v;
 if(destination==12) r.r0.rgb=v;if(destination==13) r.r1.rgb=v;
}
void write_alpha(inout R r,uint destination,float v) {
 v=clamp(v,-1,1);if(destination==4) r.c.a=v;if(destination==5) r.s.a=v;
 if(destination==8) r.t0.a=v;if(destination==9) r.t1.a=v;if(destination==10) r.t2.a=v;if(destination==11) r.t3.a=v;
 if(destination==12) r.r0.a=v;if(destination==13) r.r1.a=v;
}
// Texture inputs are signature registers and cannot be indexed dynamically in
// shader model 5. Expand the four fixed slots before the compiler sees them.
#define TEXTURE(N) { \
 uint mode=stages[N].x;float4 coord=p.t##N; \
 if(mode!=0) { \
  float4 sampled=0;bool enabled=true; \
  if(mode==4) sampled=coord; \
  else if(mode==5) { \
   uint clip=stages[N].z; \
   if((clip&1)?coord.x>=0:coord.x<0) discard;if((clip&2)?coord.y>=0:coord.y<0) discard; \
   if((clip&4)?coord.z>=0:coord.z<0) discard;if((clip&8)?coord.w>=0:coord.w<0) discard; \
  } else if(mode==3) sampled=cube##N.Sample(sm##N,coord.xyz); \
  else if(mode==2) sampled=vol##N.Sample(sm##N,coord.xyz/coord.w); \
  else { \
   float2 uv=coord.xy/coord.w;    if(bounded_samples[N]) uv=clamp(uv,sample_bounds[N].xy,sample_bounds[N].zw); \
   if(mode==6 || mode==15 || mode==16) { \
    uint previous=stages[N].y;enabled=stages[previous].x!=0 && stages[previous].x!=5; \
    float4 value=source(r,8+previous,0,false); \
    if(mode==6) { \
     float2 bytes=value.bg*255;float2 offset=(bytes-float2(bytes.x>=128?256:0,bytes.y>=128?256:0))/127; \
     float4 m=bump_matrices[N];uv=coord.xy+float2(dot(m.xz,offset),dot(m.yw,offset)); \
    } else uv=mode==15?value.ar:value.gb; \
   } \
   if(enabled) sampled=sample2d(tx##N,sm##N,uv,gaussian_scale[N]); \
  } \
  r.t##N=sampled;if(enabled && mode!=4 && mode!=5 && stages[N].w && sampled.a==0) discard; \
 } \
}
float4 main(P p):SV_Target {
 R r=(R)0;r.fog=float4(fog.rgb,saturate(p.fog));r.c=p.c;r.s=p.s;
 r.t0=r.t1=r.t2=r.t3=float4(0,0,0,1);
 TEXTURE(0) TEXTURE(1) TEXTURE(2) TEXTURE(3)
 r.r0.a=stages[0].x?r.t0.a:1;
 // A separate build-time shader for each count removes unused stages.
 [unroll] for(uint stage=0;stage<COMBINER_COUNT;++stage) {
  uint4 words=combiners[stage];uint cf=words.z>>12,af=words.w>>12;
  float3 c0=rgb(r,(words.x>>24)&255,stage,false),c1=rgb(r,(words.x>>16)&255,stage,false);
  float3 c2=rgb(r,(words.x>>8)&255,stage,false),c3=rgb(r,words.x&255,stage,false);
  float a0=alp(r,(words.y>>24)&255,stage,false),a1=alp(r,(words.y>>16)&255,stage,false);
  float a2=alp(r,(words.y>>8)&255,stage,false),a3=alp(r,words.y&255,stage,false);
  float3 cab=(cf&2)?dot(c0,c1).xxx:c0*c1,ccd=(cf&1)?dot(c2,c3).xxx:c2*c3;
  float aab=a0*a1,acd=a2*a3;
  bool mux=(controls.x&0x100)?r.r0.a>=0.5:(uint(r.r0.a*255)&1)!=0;
  float3 cs=(cf&4)?(mux?ccd:cab):cab+ccd;float asum=(af&4)?(mux?acd:aab):aab+acd;
  write_rgb(r,(words.z>>4)&15,outmap(float4(cab,0),cf).rgb);
  if(cf&0x80) write_alpha(r,(words.z>>4)&15,outmap(float4(cab,0),cf).b);
  write_rgb(r,words.z&15,outmap(float4(ccd,0),cf).rgb);
  if(cf&0x40) write_alpha(r,words.z&15,outmap(float4(ccd,0),cf).b);
  write_rgb(r,(words.z>>8)&15,outmap(float4(cs,0),cf).rgb);
  write_alpha(r,(words.w>>4)&15,outmap(aab.xxxx,af).x);
  write_alpha(r,words.w&15,outmap(acd.xxxx,af).x);
  write_alpha(r,(words.w>>8)&15,outmap(asum.xxxx,af).x);
 }
 float4 result=r.r0;
 if(controls.y || controls.z) {
  uint flags=controls.z;
  float3 sum=((flags&0x40)?1-r.s.rgb:r.s.rgb)+((flags&0x20)?1-r.r0.rgb:r.r0.rgb);
  r.sum=float4((flags&0x80)?saturate(sum):sum,0);
  r.ef=float4(rgb(r,(controls.z>>24)&255,0,true)*rgb(r,(controls.z>>16)&255,0,true),0);
  result=float4(rgb(r,controls.y&255,0,true)+lerp(rgb(r,(controls.y>>8)&255,0,true),
   rgb(r,(controls.y>>16)&255,0,true),rgb(r,(controls.y>>24)&255,0,true)),alp(r,(controls.z>>8)&255,0,true));
 }
 return finish(result);
})";
}
ID3DBlob* shader_blob(ShaderKind kind) {return new ShaderBlob(shader_binary(kind).bytes);}
const char* shader_profile(ShaderKind kind) {
    if(kind==ShaderKind::launch || (kind>=ShaderKind::interface_classify && kind<=ShaderKind::interface_filter)) return "cs_5_0";
    if(kind==ShaderKind::clear_pixel || kind==ShaderKind::display_pixel || kind==ShaderKind::gui_pixel ||
       (kind>=ShaderKind::combiner0 && kind<=ShaderKind::combiner8) ||
       (kind>=ShaderKind::color && kind<=ShaderKind::modulate3d) ||
       (kind>=ShaderKind::fxaa && kind<=ShaderKind::movie_chroma)) return "ps_5_0";
    return "vs_5_0";
}
std::string shader_source(ShaderKind kind) {
#ifndef _WIN32
    if(kind==ShaderKind::gui_vertex) return std::string(gui_vertex_source);
    if(kind==ShaderKind::gui_pixel) return std::string(gui_pixel_source);
#endif
    if(kind>=ShaderKind::resample_target && kind<=ShaderKind::resample_multisample_stencil) {
        const bool multisample=kind>=ShaderKind::resample_multisample;
        const bool stencil=kind==ShaderKind::resample_stencil || kind==ShaderKind::resample_multisample_stencil;
        std::string source="cbuffer C:register(b0){float2 ratio;uint source_samples;uint destination_samples;}\n"
            "cbuffer Mask:register(b1){uint bit_mask;uint3 padding;}\n";
        if(stencil) source+=multisample?"Texture2DMS<uint2> image:register(t2);\n":"Texture2D<uint2> image:register(t2);\n";
        else source+=multisample?"Texture2DMS<float4> colour:register(t0);Texture2DMS<float> depth:register(t1);\n":
            "Texture2D<float4> colour:register(t0);Texture2D<float> depth:register(t1);\n";
        if(!stencil) source+="struct Output {float4 colour:SV_Target;float depth:SV_Depth;};\n";
        source+=stencil?"void main(":"Output main(";
        source+="float4 position:SV_Position,uint sample:SV_SampleIndex){int2 p=int2(position.xy*ratio);";
        const auto index=multisample?"p,min(sample,source_samples-1)":"int3(p,0)";
        if(stencil) source+="if((image.Load("+std::string(index)+").y&bit_mask)==0) discard;";
        else {
            source+="Output o;o.colour=colour.Load("+std::string(index)+");o.depth=depth.Load("+index+");";
            if(multisample) source+="if(destination_samples==1){o.colour=0;for(uint i=0;i<source_samples;++i)o.colour+=colour.Load(p,i)/source_samples;}";
            source+="return o;";
        }
        return source+"}";
    }
    if(kind>=ShaderKind::color && kind<=ShaderKind::modulate3d) {
        // Exact reductions of pass-through and single-stage multiplication.
        // Keep the shared bindings/alpha comparison, with no register decoding.
        std::string source(combiner_source.substr(0,combiner_source.find("struct R")));
        if(kind==ShaderKind::color)
            return source+"float4 main(P p):SV_Target {return finish((controls.x&255)?saturate(p.c):max(p.c,0));}";
        const auto sample=kind==ShaderKind::modulate2d?
            "sample2d(tx0,sm0,bounded_samples.x?clamp(p.t0.xy/p.t0.w,sample_bounds[0].xy,sample_bounds[0].zw):p.t0.xy/p.t0.w,gaussian_scale.x)":
            kind==ShaderKind::modulate_cube?"cube0.Sample(sm0,p.t0.xyz)":"vol0.Sample(sm0,p.t0.xyz/p.t0.w)";
        return source+std::format("float4 main(P p):SV_Target {{float4 t={};if(stages[0].w && t.a==0) discard;return finish(saturate(max(p.c,0)*t));}}",sample);
    }
    if(kind>=ShaderKind::combiner0 && kind<=ShaderKind::combiner8)
        return std::format("#define COMBINER_COUNT {}\n",unsigned(kind)-unsigned(ShaderKind::combiner0))+std::string(combiner_source);
    if(kind>=ShaderKind::fixed0 && kind<=ShaderKind::fixed6) return fixed_transform_source(unsigned(kind)-unsigned(ShaderKind::fixed0));
    switch(kind) {
    case ShaderKind::passthrough:return R"(
struct V {float4 p:POSITION;float4 c:COLOR0;float4 s:COLOR1;
 float4 t0:TEXCOORD0;float4 t1:TEXCOORD1;float4 t2:TEXCOORD2;float4 t3:TEXCOORD3;float fog:TEXCOORD4;};
struct P {float4 p:SV_Position;float4 c:COLOR0;float4 s:COLOR1;
 float4 t0:TEXCOORD0;float4 t1:TEXCOORD1;float4 t2:TEXCOORD2;float4 t3:TEXCOORD3;float fog:TEXCOORD4;};
P main(V v) {P p;p.p=v.p;p.c=v.c;p.s=v.s;p.t0=v.t0;p.t1=v.t1;p.t2=v.t2;p.t3=v.t3;p.fog=v.fog;return p;})";
    case ShaderKind::clear_vertex:return R"(
cbuffer C:register(b0){float4 rgba;float4 parameters;}
float4 main(uint id:SV_VertexID):SV_Position {float2 p=float2((id<<1)&2,id&2);return float4(p*float2(2,-2)+float2(-1,1),parameters.x,1);})";
    case ShaderKind::clear_pixel:return "cbuffer C:register(b0){float4 rgba;float4 parameters;}\nfloat4 main():SV_Target{return rgba;}";
    case ShaderKind::display_vertex:return R"(
struct P {float4 p:SV_Position;float2 uv:TEXCOORD0;};
P main(uint id:SV_VertexID) {P o;o.uv=float2((id<<1)&2,id&2);o.p=float4(o.uv*float2(2,-2)+float2(-1,1),0,1);return o;})";
    case ShaderKind::display_pixel:return R"(
Texture2D image:register(t0);SamplerState filter:register(s0);
float4 main(float4 p:SV_Position,float2 uv:TEXCOORD0):SV_Target{return image.Sample(filter,uv);})";
    case ShaderKind::copy_multisample_color:return R"(
Texture2DMS<float4> source_colour:register(t0);
cbuffer C:register(b0){int2 offset;int2 padding;}
float4 main(float4 position:SV_Position,uint sample:SV_SampleIndex):SV_Target {
 return source_colour.Load(int2(position.xy)+offset,sample);
})";
    case ShaderKind::copy_multisample:return R"(
Texture2DMS<float4> source_colour:register(t0);Texture2DMS<float> source_depth:register(t1);
struct Output {float4 colour:SV_Target;float depth:SV_Depth;};
Output main(float4 position:SV_Position,uint sample:SV_SampleIndex) {
 Output result;result.colour=source_colour.Load(int2(position.xy),sample);
 result.depth=source_depth.Load(int2(position.xy),sample);return result;
})";
    case ShaderKind::copy_multisample_stencil:return R"(
Texture2DMS<uint2> source_stencil:register(t2);cbuffer C:register(b0){uint bit_mask;uint3 padding;}
void main(float4 position:SV_Position,uint sample:SV_SampleIndex) {
 if((source_stencil.Load(int2(position.xy),sample).y&bit_mask)==0) discard;
})";
    case ShaderKind::fxaa:return R"(
Texture2D image:register(t0);SamplerState filter:register(s0);
cbuffer C:register(b0){float2 texel;float quality;float spare;}
float luminance(float2 uv) {return dot(image.SampleLevel(filter,uv,0).rgb,float3(0.299,0.587,0.114));}
float4 main(float4 position:SV_Position,float2 uv:TEXCOORD0):SV_Target {
 float4 centre=image.SampleLevel(filter,uv,0);
 float m=dot(centre.rgb,float3(0.299,0.587,0.114));
 float n=luminance(uv-float2(0,texel.y)),s=luminance(uv+float2(0,texel.y));
 float w=luminance(uv-float2(texel.x,0)),e=luminance(uv+float2(texel.x,0));
 float low=min(m,min(min(n,s),min(w,e))),high=max(m,max(max(n,s),max(w,e)));
 float contrast=high-low;
 float threshold=quality<1.5?0.25:quality<2.5?0.166667:0.125;
 float minimum=quality<1.5?0.083333:quality<2.5?0.0625:0.03125;
 if(contrast<max(minimum,high*threshold)) return centre;
 float nw=luminance(uv-texel),ne=luminance(uv+float2(texel.x,-texel.y));
 float sw=luminance(uv+float2(-texel.x,texel.y)),se=luminance(uv+texel);
 float horizontal=abs(nw+sw-2*w)+2*abs(n+s-2*m)+abs(ne+se-2*e);
 float vertical=abs(nw+ne-2*n)+2*abs(w+e-2*m)+abs(sw+se-2*s);
 bool along_x=horizontal>=vertical;
 float side_a=along_x?n:w,side_b=along_x?s:e;
 float gradient_a=abs(side_a-m),gradient_b=abs(side_b-m);
 bool first=gradient_a>=gradient_b;
 float average=0.5*(m+(first?side_a:side_b));
 float limit=0.25*max(gradient_a,gradient_b);
 float2 normal=along_x?float2(0,texel.y):float2(texel.x,0);
 if(first) normal=-normal;
 float2 tangent=along_x?float2(texel.x,0):float2(0,texel.y);
 float2 origin=uv+normal*0.5,negative=origin-tangent,positive=origin+tangent;
 float difference_a=luminance(negative)-average,difference_b=luminance(positive)-average;
 bool done_a=abs(difference_a)>=limit,done_b=abs(difference_b)>=limit;
 uint steps=quality<1.5?4:quality<2.5?8:12;
 [loop] for(uint step=1;step<steps && (!done_a || !done_b);++step) {
  if(!done_a) {negative-=tangent;difference_a=luminance(negative)-average;done_a=abs(difference_a)>=limit;}
  if(!done_b) {positive+=tangent;difference_b=luminance(positive)-average;done_b=abs(difference_b)>=limit;}
 }
 float distance_a=along_x?uv.x-negative.x:uv.y-negative.y;
 float distance_b=along_x?positive.x-uv.x:positive.y-uv.y;
 bool nearer_a=distance_a<distance_b;
 float edge=0.5-min(distance_a,distance_b)/(distance_a+distance_b);
 if(((nearer_a?difference_a:difference_b)<0)==(m<average)) edge=0;
 float local=(2*(n+s+w+e)+nw+ne+sw+se)/12;
 float subpixel=saturate(abs(local-m)/contrast);
 subpixel=subpixel*subpixel*(3-2*subpixel);subpixel*=subpixel;
 subpixel*=quality<1.5?0.25:quality<2.5?0.5:0.75;
 float2 shifted=uv+normal*max(edge,subpixel);
 return float4(image.SampleLevel(filter,shifted,0).rgb,centre.a);
})";
    // FMV enlargement follows the published FSR 1 EASU/RCAS design (see
    // THIRD_PARTY_NOTICES.txt): a 12-texel edge-steered Lanczos-2 window,
    // clamped to the four nearest source texels to prevent ringing, then a
    // limited contrast-adaptive sharpen that attenuates itself on noise.
    //
    // Sofdec stores colour at half resolution and its decoder repeats each
    // sample over an aligned 2x2 block. movie_chroma recovers that plane with a
    // per-block confidence; the upscale pass then interpolates it, weighted by
    // similarity to the enlarged luma (joint bilateral upsampling), so colour
    // edges follow the full-resolution image instead of 2-pixel steps.
    case ShaderKind::movie_chroma:return R"(
Texture2D<float4> image:register(t0);
cbuffer C:register(b0){float2 ratio;int2 last;float sharpness;float3 padding;}
static const float3 weights=float3(0.299,0.587,0.114);
float4 main(float4 position:SV_Position,float2 uv:TEXCOORD0):SV_Target {
 int2 p=int2(position.xy)*2;
 float3 c[4]={image.Load(int3(p,0)).rgb,image.Load(int3(min(p+int2(1,0),last),0)).rgb,
  image.Load(int3(min(p+int2(0,1),last),0)).rgb,image.Load(int3(min(p+int2(1,1),last),0)).rgb};
 float2 sum=0,low=1e9,high=-1e9;float brightness=0;
 [unroll] for(int i=0;i<4;++i) {
  float y=dot(c[i],weights);float2 d=float2(c[i].b-y,c[i].r-y);
  sum+=d;low=min(low,d);high=max(high,d);brightness+=y;
 }
 // Repeated samples agree within rounding; clipped or full-resolution colour
 // does not, and keeps the edge-adaptive result.
 float spread=max(high.x-low.x,high.y-low.y)*255;
 return float4(sum*0.25,brightness*0.25,saturate((10-spread)*0.25));
})";
    case ShaderKind::movie_upscale:return R"(
Texture2D<float4> image:register(t0);Texture2D<float4> chroma:register(t1);
cbuffer C:register(b0){float2 ratio;int2 last;float sharpness;float3 padding;}
static const float3 weights=float3(0.299,0.587,0.114);
float4 texel(int2 p) {return image.Load(int3(clamp(p,int2(0,0),last),0));}
float luma(float4 c) {return c.b*0.5+(c.r*0.5+c.g);}
// Bilinearly weighted edge direction and strength around one centre texel.
void analyse(inout float2 direction,inout float strength,float weight,float above,float left,float centre,float right,float below) {
 float dx=right-left,lx=max(abs(right-centre),abs(centre-left));
 lx=saturate(abs(dx)/max(lx,1.0/65536));direction.x+=dx*weight;strength+=lx*lx*weight;
 float dy=below-above,ly=max(abs(below-centre),abs(centre-above));
 ly=saturate(abs(dy)/max(ly,1.0/65536));direction.y+=dy*weight;strength+=ly*ly*weight;
}
void tap(inout float4 sum,inout float total,float2 offset,float2 direction,float2 stretch,float lobe,float clip,float4 colour) {
 float2 v=float2(dot(offset,direction),dot(offset,float2(-direction.y,direction.x)))*stretch;
 float d2=min(dot(v,v),clip);
 // Lanczos-2 approximation without trigonometry: base * window.
 float base=0.4*d2-1,window=lobe*d2-1;base*=base;window*=window;
 float weight=(1.5625*base-0.5625)*window;
 sum+=colour*weight;total+=weight;
}
float4 main(float4 position:SV_Position,float2 uv:TEXCOORD0):SV_Target {
 float2 pp=position.xy*ratio-0.5,fp=floor(pp);pp-=fp;int2 f=int2(fp);
 //    b c
 //  e F g h
 //  i j k l
 //    n o
 float4 b=texel(f+int2(0,-1)),c=texel(f+int2(1,-1));
 float4 e=texel(f+int2(-1,0)),F=texel(f),g=texel(f+int2(1,0)),h=texel(f+int2(2,0));
 float4 i=texel(f+int2(-1,1)),j=texel(f+int2(0,1)),k=texel(f+int2(1,1)),l=texel(f+int2(2,1));
 float4 n=texel(f+int2(0,2)),o=texel(f+int2(1,2));
 float bL=luma(b),cL=luma(c),eL=luma(e),fL=luma(F),gL=luma(g),hL=luma(h);
 float iL=luma(i),jL=luma(j),kL=luma(k),lL=luma(l),nL=luma(n),oL=luma(o);
 float2 direction=0;float strength=0;
 analyse(direction,strength,(1-pp.x)*(1-pp.y),bL,eL,fL,gL,jL);
 analyse(direction,strength,pp.x*(1-pp.y),cL,fL,gL,hL,kL);
 analyse(direction,strength,(1-pp.x)*pp.y,fL,iL,jL,kL,nL);
 analyse(direction,strength,pp.x*pp.y,gL,jL,kL,lL,oL);
 float magnitude=dot(direction,direction);
 if(magnitude<1.0/32768) direction=float2(1,0);else direction*=rsqrt(magnitude);
 strength*=0.5;strength*=strength;
 // Lengthen the kernel along edges and narrow it across them.
 float diagonal=1/max(abs(direction.x),abs(direction.y));
 float2 stretch=float2(1+(diagonal-1)*strength,1-0.5*strength);
 float lobe=0.5+((1.0/4-0.04)-0.5)*strength,clip=1/lobe;
 float4 sum=0;float total=0;
 tap(sum,total,float2(0,-1)-pp,direction,stretch,lobe,clip,b);
 tap(sum,total,float2(1,-1)-pp,direction,stretch,lobe,clip,c);
 tap(sum,total,float2(-1,1)-pp,direction,stretch,lobe,clip,i);
 tap(sum,total,float2(0,1)-pp,direction,stretch,lobe,clip,j);
 tap(sum,total,float2(0,0)-pp,direction,stretch,lobe,clip,F);
 tap(sum,total,float2(-1,0)-pp,direction,stretch,lobe,clip,e);
 tap(sum,total,float2(1,1)-pp,direction,stretch,lobe,clip,k);
 tap(sum,total,float2(2,1)-pp,direction,stretch,lobe,clip,l);
 tap(sum,total,float2(2,0)-pp,direction,stretch,lobe,clip,h);
 tap(sum,total,float2(1,0)-pp,direction,stretch,lobe,clip,g);
 tap(sum,total,float2(1,2)-pp,direction,stretch,lobe,clip,o);
 tap(sum,total,float2(0,2)-pp,direction,stretch,lobe,clip,n);
 float4 low=min(min(F,g),min(j,k)),high=max(max(F,g),max(j,k));
 float4 colour=clamp(total>1.0/65536?sum/total:F,low,high);
 // Colour from the four nearest half-resolution samples (centred on each
 // 2x2 block), preferring those whose brightness matches this pixel.
 uint width,height;chroma.GetDimensions(width,height);int2 chroma_last=int2(width,height)-1;
 float2 q=(position.xy*ratio-1)*0.5,qf=floor(q),t=q-qf;int2 qi=int2(qf);
 float target=dot(colour.rgb,weights),confidence=1,weight_sum=0;float2 difference=0;
 [unroll] for(int y=0;y<2;++y) [unroll] for(int x=0;x<2;++x) {
  float4 block=chroma.Load(int3(clamp(qi+int2(x,y),int2(0,0),chroma_last),0));
  float spatial=(x?t.x:1-t.x)*(y?t.y:1-t.y),range=(block.z-target)*10;
  float weight=spatial*(0.001+exp(-range*range));
  difference+=block.xy*weight;weight_sum+=weight;confidence=min(confidence,block.w);
 }
 difference/=weight_sum;
 float3 rebuilt=float3(target+difference.y,0,target+difference.x);
 rebuilt.g=(target-weights.r*rebuilt.r-weights.b*rebuilt.b)/weights.g;
 colour.rgb=lerp(colour.rgb,saturate(rebuilt),confidence);
 return colour;
})";
    case ShaderKind::movie_sharpen:return R"(
Texture2D<float4> image:register(t0);
cbuffer C:register(b0){float2 ratio;int2 source_last;float sharpness;float3 padding;}
float luma(float3 c) {return c.b*0.5+(c.r*0.5+c.g);}
float3 texel(int2 p,int2 last) {return image.Load(int3(clamp(p,int2(0,0),last),0)).rgb;}
float4 main(float4 position:SV_Position,float2 uv:TEXCOORD0):SV_Target {
 // This pass runs at output resolution; clamp to the enlarged image.
 uint width,height;image.GetDimensions(width,height);int2 last=int2(width,height)-1;
 int2 p=int2(position.xy);
 //    b
 //  d e f
 //    h
 float4 centre=image.Load(int3(p,0));float3 e=centre.rgb;
 float3 b=texel(p+int2(0,-1),last),d=texel(p+int2(-1,0),last),f=texel(p+int2(1,0),last),h=texel(p+int2(0,1),last);
 float bL=luma(b),dL=luma(d),eL=luma(e),fL=luma(f),hL=luma(h);
 // Reduce sharpening where the centre differs from its ring like noise.
 float range=max(max(max(bL,dL),max(eL,fL)),hL)-min(min(min(bL,dL),min(eL,fL)),hL);
 float noise=saturate(abs(0.25*(bL+dL+fL+hL)-eL)/max(range,1.0/65536));
 noise=1-0.5*noise;
 // The largest negative lobe that keeps every channel within [0,1].
 float3 low=min(min(b,d),min(f,h)),high=max(max(b,d),max(f,h));
 float3 hit_low=low/max(4*high,1.0/65536);
 float3 hit_high=(1-high)/min(4*low-4,-1.0/65536);
 float3 lobes=max(-hit_low,hit_high);
 float lobe=max(-0.1875,min(max(lobes.r,max(lobes.g,lobes.b)),0))*sharpness*noise;
 return float4((lobe*(b+d+f+h)+e)/(4*lobe+1),centre.a);
})";
    // HUD/menu texture enlargement (Real-ESRGAN realesr-animevideov3, see
    // upscaler_model.h). Each texture runs once, in bounded steps between
    // frames. Convolutions cover a tile plus a 12-texel halo; positions outside
    // the halo read zero like PyTorch's padding, and halo texels beyond the
    // texture repeat its edge. Halo 12 changes tile interiors by at most 2/255.
    case ShaderKind::interface_classify:return std::string(interface_job_source)+R"(
Texture2D<float4> source:register(t0);
// Typed R32_UINT UAV: the InterlockedMax intrinsic on an element compiles
// with fxc and vkd3d-shader alike (vkd3d lacks the raw-buffer method).
RWBuffer<uint> statistics:register(u0);
groupshared uint group_edge,group_alpha;
float4 premultiplied(int2 p) {float4 v=source.Load(int3(p,0));return float4(v.rgb*v.a,v.a);}
[numthreads(8,8,1)] void main(uint3 id:SV_DispatchThreadID,uint index:SV_GroupIndex) {
 if(!index) {group_edge=0;group_alpha=0;}
 GroupMemoryBarrierWithGroupSync();
 int2 p=int2(id.xy);
 if(all(p<image)) {
  // The largest premultiplied step between neighbours separates outlined
  // artwork from glows and gradients, which keep the filter's softness.
  float4 v=premultiplied(p);float step=0;
  if(p.x+1<image.x) {float4 e=abs(premultiplied(p+int2(1,0))-v);step=max(step,max(max(e.r,e.g),max(e.b,e.a)));}
  if(p.y+1<image.y) {float4 e=abs(premultiplied(p+int2(0,1))-v);step=max(step,max(max(e.r,e.g),max(e.b,e.a)));}
  uint ignored;
  InterlockedMax(group_edge,uint(round(saturate(step)*65535)),ignored);
  if(v.a<254.5/255) InterlockedMax(group_alpha,1u,ignored);
 }
 GroupMemoryBarrierWithGroupSync();
 if(!index) {InterlockedMax(statistics[0],group_edge);InterlockedMax(statistics[1],group_alpha);}
})";
    case ShaderKind::interface_bleed:return std::string(interface_job_source)+R"(
Texture2D<float4> previous:register(t0);
RWTexture2D<float4> next:register(u0);
// Spread visible colours into fully transparent texels, so the network sees
// the artwork's own colour at alpha edges instead of arbitrary hidden RGB.
[numthreads(8,8,1)] void main(uint3 id:SV_DispatchThreadID) {
 int2 p=int2(id.xy);if(any(p>=image)) return;
 float4 v=previous.Load(int3(p,0));
 if(v.a>0) {next[p]=float4(v.rgb,1);return;}
 float3 sum=0;float count=0;
 [unroll] for(int y=-1;y<=1;++y) [unroll] for(int x=-1;x<=1;++x) {
  int2 q=p+int2(x,y);
  if(all(q>=0) && all(q<image)) {float4 n=previous.Load(int3(q,0));if(n.a>0) {sum+=n.rgb;count+=1;}}
 }
 next[p]=count>0?float4(sum/count,1):float4(0,0,0,0);
})";
    case ShaderKind::interface_conv_first:return std::string(interface_job_source)+std::string(interface_layer_source)+R"(
Texture2D<float4> image_in:register(t0);
float3 input_texel(int2 p) {
 if(any(p<0) || any(p>=extent)) return 0;
 float4 v=image_in.Load(int3(clamp(origin+p,int2(0,0),image-1),0));
 return mode?v.aaa:v.rgb;
}
[numthreads(8,8,1)] void main(uint3 id:SV_DispatchThreadID) {
 int2 q=int2(id.xy);if(any(q>=extent)) return;
 float4 a0=bias[0],a1=bias[1],a2=bias[2],a3=bias[3];
 [unroll] for(int t=0;t<9;++t) {
  float3 x=input_texel(q+int2(t%3-1,t/3-1));
  [unroll] for(int c=0;c<3;++c) {
   a0+=x[c]*weights[(t*3+c)*4];a1+=x[c]*weights[(t*3+c)*4+1];a2+=x[c]*weights[(t*3+c)*4+2];a3+=x[c]*weights[(t*3+c)*4+3];
  }
 }
 store(q,uint(slots.x),a0,a1,a2,a3);
})";
    case ShaderKind::interface_conv:return std::string(interface_job_source)+std::string(interface_layer_source)+R"(
Texture2DArray<float4> features:register(t0);
cbuffer Batch:register(b2) {int4 extents[8];};
// Z selects the batch slot; every slot shares this layer's weights.
[numthreads(8,8,1)] void main(uint3 id:SV_DispatchThreadID) {
 uint slot=id.z;int2 bound=extents[slot].xy;
 int2 q=int2(id.xy);if(any(q>=bound)) return;
 float4 a0=bias[0],a1=bias[1],a2=bias[2],a3=bias[3];
 [unroll] for(int t=0;t<9;++t) {
  int2 p=q+int2(t%3-1,t/3-1);
  float inside=all(p>=0) && all(p<bound)?1:0;
  [unroll] for(int s=0;s<16;++s) {
   float4 x=features.Load(int4(p,slot*16+s,0))*inside;
   [unroll] for(int k=0;k<4;++k) {
    const int w=(t*64+s*4+k)*4;
    a0+=x[k]*weights[w];a1+=x[k]*weights[w+1];a2+=x[k]*weights[w+2];a3+=x[k]*weights[w+3];
   }
  }
 }
 store(q,slot,a0,a1,a2,a3);
})";
    case ShaderKind::interface_compose:return std::string(interface_job_source)+R"(
Texture2D<float4> colour_in:register(t0);
Texture2D<float4> source:register(t1);
Texture2DArray<float4> net:register(t2);
RWTexture2D<unorm float4> output:register(u0);
// One x4 network sample: pixel shuffle channel c*16+i*4+j is slice c*4+i of
// the colour (slots.x) or alpha (slots.y) pass, component j, added to the
// nearest-upsampled input and clamped to [0,1].
float4 network(int2 s) {
 int2 e=(s>>2)+halo;int i=s.y&3,j=s.x&3;
 int2 texel=clamp(origin+e,int2(0,0),image-1);
 float3 base=colour_in.Load(int3(texel,0)).rgb;float alpha=source.Load(int3(texel,0)).a;
 float4 result;
 [unroll] for(int c=0;c<3;++c) result[c]=saturate(net.Load(int4(e,slots.x*16+c*4+i,0))[j]+base[c]);
 if(mode) {
  float a=0;
  [unroll] for(int c=0;c<3;++c) a+=saturate(net.Load(int4(e,slots.y*16+c*4+i,0))[j]+alpha);
  result.a=a/3;
 } else result.a=alpha;
 return result;
}
[numthreads(8,8,1)] void main(uint3 id:SV_DispatchThreadID) {
 int2 o=int2(id.xy);if(any(o>=interior*int(factor))) return;
 float4 result;
 if(factor==4) result=network(o);
 else {
  // Average the x4 samples covering this output texel's area, weighting
  // colour by coverage so transparent texels cannot darken visible edges.
  float2 low=float2(o)*4/factor,high=float2(o+1)*4/factor;
  float4 sum=0;float3 plain=0;float total=0;
  for(int y=int(floor(low.y));y<int(ceil(high.y));++y) {
   float wy=min(high.y,y+1)-max(low.y,y);
   for(int x=int(floor(low.x));x<int(ceil(high.x));++x) {
    float w=wy*(min(high.x,x+1)-max(low.x,x));float4 v=network(int2(x,y));
    sum+=float4(v.rgb*v.a,v.a)*w;plain+=v.rgb*w;total+=w;
   }
  }
  result=sum/total;result.rgb=result.a>1.0/1024?result.rgb/result.a:plain/total;
 }
 output[target+o]=saturate(result);
})";
    case ShaderKind::interface_filter:return std::string(interface_job_source)+R"(
Texture2D<float4> source:register(t0);
RWTexture2D<unorm float4> output:register(u0);
float4 texel(int2 p) {float4 v=source.Load(int3(clamp(p,int2(0,0),image-1),0));return float4(v.rgb*v.a,v.a);}
float catmull_rom(float x) {x=abs(x);return x<1?(1.5*x-2.5)*x*x+1:x<2?((-0.5*x+2.5)*x-4)*x+2:0;}
// Fallback for soft or tiny textures: premultiplied Catmull-Rom, clamped to
// the four nearest texels so it cannot ring or invent detail.
[numthreads(8,8,1)] void main(uint3 id:SV_DispatchThreadID) {
 int2 o=int2(id.xy);if(any(o>=image*int(factor))) return;
 float2 u=(float2(o)+0.5)/factor-0.5,cell=floor(u),f=u-cell;int2 b=int2(cell);
 float4 sum=0;float total=0;
 [unroll] for(int y=-1;y<=2;++y) [unroll] for(int x=-1;x<=2;++x) {
  float w=catmull_rom(f.x-x)*catmull_rom(f.y-y);sum+=texel(b+int2(x,y))*w;total+=w;
 }
 float4 n0=texel(b),n1=texel(b+int2(1,0)),n2=texel(b+int2(0,1)),n3=texel(b+int2(1,1));
 float4 result=clamp(sum/total,min(min(n0,n1),min(n2,n3)),max(max(n0,n1),max(n2,n3)));
 if(result.a>1.0/1024) result.rgb/=result.a;
 else {float4 v=source.Load(int3(clamp(int2(round(u)),int2(0,0),image-1),0));result=float4(v.rgb,0);}
 output[o]=saturate(result);
})";
    default:throw std::runtime_error("Shader source requires a vertex program or the pinned GUI backend");
    }
}
}
