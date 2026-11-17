#pragma once
#include "render.h"
#include <d3d11.h>
#include "com_ptr.h"

namespace smbd {
// Spatial AA of completed game images. Game feedback uses the original targets.
class FrameAntialiasing {
public:
    FrameAntialiasing(ID3D11Device*,ID3D11DeviceContext*,unsigned width,unsigned height,unsigned quality,std::uint64_t& compilations);
    void publish(ID3D11ShaderResourceView* source);
    ID3D11Texture2D* image() const {return image_.Get();}
    ID3D11ShaderResourceView* view() const {return view_.Get();}
    std::size_t bytes() const {return std::size_t(width_)*height_*4;}
    Image readback();
private:
    template<class T> using Ptr=Microsoft::WRL::ComPtr<T>;
    ID3D11Device* device_;
    ID3D11DeviceContext* context_;
    unsigned width_,height_;
    bool ready_=false;
    Ptr<ID3D11Texture2D> image_;
    Ptr<ID3D11ShaderResourceView> view_;
    Ptr<ID3D11RenderTargetView> target_;
    Ptr<ID3D11VertexShader> vertex_;
    Ptr<ID3D11PixelShader> pixel_;
    Ptr<ID3D11SamplerState> sampler_;
    Ptr<ID3D11Buffer> constants_;
    Ptr<ID3D11RasterizerState> raster_;
    Ptr<ID3D11DepthStencilState> depth_;
};

// Real-time FMV enlargement to the internal resolution. Each decoded movie frame
// is filtered once when uploaded, never per draw; the result keeps the source's
// normalized texture coordinates, so original draws sample it unchanged.
class MovieUpscaler {
public:
    template<class T> using Ptr=Microsoft::WRL::ComPtr<T>;
    struct Image {
        Ptr<ID3D11Texture2D> texture;
        Ptr<ID3D11ShaderResourceView> view;
        Ptr<ID3D11RenderTargetView> target;
        unsigned width=0,height=0,pixel_bytes=4;
        std::size_t bytes() const {return std::size_t(width)*height*pixel_bytes;}
    };
    MovieUpscaler(ID3D11Device*,ID3D11DeviceContext*,std::uint64_t& compilations);
    Image create(unsigned width,unsigned height,DXGI_FORMAT=DXGI_FORMAT_R8G8B8A8_UNORM) const;
    // Leaves no shader resources or render targets bound.
    void upscale(const D3D11_SUBRESOURCE_DATA& pixels,DXGI_FORMAT,unsigned width,unsigned height,unsigned scale,const Image& output);
    std::size_t bytes() const {return intermediate_.bytes()+chroma_.bytes()+std::size_t(source_width_)*source_height_*4;}
private:
    ID3D11Device* device_;
    ID3D11DeviceContext* context_;
    Image intermediate_,chroma_; // Enlarged colour; half-resolution colour difference plane.
    Ptr<ID3D11Texture2D> source_;
    Ptr<ID3D11ShaderResourceView> source_view_;
    unsigned source_width_=0,source_height_=0,scale_=0;
    DXGI_FORMAT source_format_=DXGI_FORMAT_UNKNOWN;
    Ptr<ID3D11VertexShader> vertex_;
    Ptr<ID3D11PixelShader> chroma_pass_,upscale_,sharpen_;
    Ptr<ID3D11Buffer> constants_;
    Ptr<ID3D11RasterizerState> raster_;
    Ptr<ID3D11DepthStencilState> depth_;
};

// Grow multisample colour/depth/stencil without discarding individual samples.
// D3D11 forbids ordinary partial copies of multisampled resources.
class MultisampleCopy {
public:
    MultisampleCopy(ID3D11Device*,ID3D11DeviceContext*,std::uint64_t& compilations);
    void grow(ID3D11ShaderResourceView* color,ID3D11ShaderResourceView* depth,ID3D11ShaderResourceView* stencil,
              ID3D11RenderTargetView*,ID3D11DepthStencilView*,unsigned width,unsigned height);
    void color(ID3D11ShaderResourceView*,ID3D11RenderTargetView*,unsigned sx,unsigned sy,unsigned dx,unsigned dy,unsigned width,unsigned height);
    void resample(ID3D11ShaderResourceView* color,ID3D11ShaderResourceView* depth,ID3D11ShaderResourceView* stencil,
                  ID3D11RenderTargetView*,ID3D11DepthStencilView*,unsigned width,unsigned height,
                  unsigned source_scale,unsigned destination_scale,unsigned source_samples,unsigned destination_samples);
private:
    template<class T> using Ptr=Microsoft::WRL::ComPtr<T>;
    ID3D11DeviceContext* context_;
    Ptr<ID3D11VertexShader> vertex_;
    Ptr<ID3D11PixelShader> pixel_,stencil_,color_;
    std::array<Ptr<ID3D11PixelShader>,4> resample_;
    Ptr<ID3D11Buffer> offset_;
    Ptr<ID3D11DepthStencilState> no_depth_;
    Ptr<ID3D11RasterizerState> raster_;
    Ptr<ID3D11DepthStencilState> depth_;
    std::array<Ptr<ID3D11DepthStencilState>,8> stencil_states_;
    std::array<Ptr<ID3D11Buffer>,8> stencil_masks_;
};
}
