#include "antialiasing.h"
#include <climits>
#include "shaders.h"
#include <cstring>
#include <initializer_list>
#include <format>
#include <stdexcept>

namespace smbd {
namespace {
void checked_aa(HRESULT status,const char* operation) {
    if(FAILED(status)) throw std::runtime_error(std::format("{} failed (0x{:08X})",operation,static_cast<unsigned>(status)));
}
}
FrameAntialiasing::FrameAntialiasing(ID3D11Device* device,ID3D11DeviceContext* context,unsigned width,unsigned height,unsigned quality,std::uint64_t& compilations)
    :device_(device),context_(context),width_(width),height_(height) {
    if(!width || !height || quality<1 || quality>3) throw std::runtime_error("Invalid antialiasing dimensions or quality.");
    const auto vertex=shader_binary(ShaderKind::display_vertex),pixel=shader_binary(ShaderKind::fxaa);
    compilations+=unsigned(vertex.compiled)+unsigned(pixel.compiled);
    checked_aa(device->CreateVertexShader(vertex.bytes.data(),vertex.bytes.size(),nullptr,&vertex_),"AA vertex shader");
    checked_aa(device->CreatePixelShader(pixel.bytes.data(),pixel.bytes.size(),nullptr,&pixel_),"FXAA shader");
    D3D11_TEXTURE2D_DESC texture{};texture.Width=width;texture.Height=height;texture.MipLevels=texture.ArraySize=texture.SampleDesc.Count=1;
    texture.Format=DXGI_FORMAT_R8G8B8A8_UNORM;texture.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET;
    checked_aa(device->CreateTexture2D(&texture,nullptr,&image_),"AA image");
    checked_aa(device->CreateShaderResourceView(image_.Get(),nullptr,&view_),"AA texture view");
    checked_aa(device->CreateRenderTargetView(image_.Get(),nullptr,&target_),"AA target view");
    const Float4 parameters{1.0f/width,1.0f/height,float(quality),0};
    D3D11_BUFFER_DESC buffer{};buffer.ByteWidth=sizeof(parameters);buffer.Usage=D3D11_USAGE_IMMUTABLE;buffer.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA initial{parameters.data()};
    checked_aa(device->CreateBuffer(&buffer,&initial,&constants_),"AA parameters");
    D3D11_SAMPLER_DESC sampler{};sampler.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;sampler.MaxLOD=D3D11_FLOAT32_MAX;sampler.ComparisonFunc=D3D11_COMPARISON_ALWAYS;
    checked_aa(device->CreateSamplerState(&sampler,&sampler_),"AA sampler");
    D3D11_RASTERIZER_DESC raster{};raster.FillMode=D3D11_FILL_SOLID;raster.CullMode=D3D11_CULL_NONE;raster.DepthClipEnable=TRUE;
    checked_aa(device->CreateRasterizerState(&raster,&raster_),"AA rasterizer");
    D3D11_DEPTH_STENCIL_DESC depth{};depth.DepthFunc=D3D11_COMPARISON_ALWAYS;
    depth.FrontFace=depth.BackFace={D3D11_STENCIL_OP_KEEP,D3D11_STENCIL_OP_KEEP,D3D11_STENCIL_OP_KEEP,D3D11_COMPARISON_ALWAYS};
    checked_aa(device->CreateDepthStencilState(&depth,&depth_),"AA depth state");
}
void FrameAntialiasing::publish(ID3D11ShaderResourceView* source) {
    auto target=target_.Get();context_->OMSetRenderTargets(1,&target,nullptr);
    const D3D11_VIEWPORT viewport{0,0,float(width_),float(height_),0,1};context_->RSSetViewports(1,&viewport);
    context_->RSSetState(raster_.Get());context_->OMSetDepthStencilState(depth_.Get(),0);context_->OMSetBlendState(nullptr,nullptr,UINT_MAX);
    context_->IASetInputLayout(nullptr);context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->VSSetShader(vertex_.Get(),nullptr,0);context_->PSSetShader(pixel_.Get(),nullptr,0);
    auto sampler=sampler_.Get();auto constants=constants_.Get();context_->PSSetSamplers(0,1,&sampler);context_->PSSetConstantBuffers(0,1,&constants);
    context_->PSSetShaderResources(0,1,&source);context_->Draw(3,0);
    ID3D11ShaderResourceView* empty=nullptr;context_->PSSetShaderResources(0,1,&empty);context_->OMSetRenderTargets(0,nullptr,nullptr);
    ready_=true;
}
Image FrameAntialiasing::readback() {
    if(!ready_) throw std::runtime_error("Publish a frame before reading its antialiasing result.");
    D3D11_TEXTURE2D_DESC description{};image_->GetDesc(&description);
    description.BindFlags=0;description.Usage=D3D11_USAGE_STAGING;description.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    Ptr<ID3D11Texture2D> staging;checked_aa(device_->CreateTexture2D(&description,nullptr,&staging),"AA diagnostic storage");
    context_->CopyResource(staging.Get(),image_.Get());D3D11_MAPPED_SUBRESOURCE mapped{};
    checked_aa(context_->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped),"AA diagnostic read");
    Image result{width_,height_,std::vector<std::byte>(bytes())};
    for(unsigned row=0;row<height_;++row) std::memcpy(result.rgba.data()+std::size_t(row)*width_*4,static_cast<const std::byte*>(mapped.pData)+std::size_t(row)*mapped.RowPitch,width_*4);
    context_->Unmap(staging.Get(),0);return result;
}
MovieUpscaler::MovieUpscaler(ID3D11Device* device,ID3D11DeviceContext* context,std::uint64_t& compilations):device_(device),context_(context) {
    const auto vertex=shader_binary(ShaderKind::display_vertex),chroma=shader_binary(ShaderKind::movie_chroma);
    const auto upscale=shader_binary(ShaderKind::movie_upscale),sharpen=shader_binary(ShaderKind::movie_sharpen);
    compilations+=unsigned(vertex.compiled)+unsigned(chroma.compiled)+unsigned(upscale.compiled)+unsigned(sharpen.compiled);
    checked_aa(device->CreatePixelShader(chroma.bytes.data(),chroma.bytes.size(),nullptr,&chroma_pass_),"Movie colour plane shader");
    checked_aa(device->CreateVertexShader(vertex.bytes.data(),vertex.bytes.size(),nullptr,&vertex_),"Movie vertex shader");
    checked_aa(device->CreatePixelShader(upscale.bytes.data(),upscale.bytes.size(),nullptr,&upscale_),"Movie upscaling shader");
    checked_aa(device->CreatePixelShader(sharpen.bytes.data(),sharpen.bytes.size(),nullptr,&sharpen_),"Movie sharpening shader");
    D3D11_BUFFER_DESC buffer{};buffer.ByteWidth=32;buffer.Usage=D3D11_USAGE_DYNAMIC;
    buffer.BindFlags=D3D11_BIND_CONSTANT_BUFFER;buffer.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
    checked_aa(device->CreateBuffer(&buffer,nullptr,&constants_),"Movie upscaling parameters");
    D3D11_RASTERIZER_DESC raster{};raster.FillMode=D3D11_FILL_SOLID;raster.CullMode=D3D11_CULL_NONE;raster.DepthClipEnable=TRUE;
    checked_aa(device->CreateRasterizerState(&raster,&raster_),"Movie upscaling rasterizer");
    D3D11_DEPTH_STENCIL_DESC depth{};depth.DepthFunc=D3D11_COMPARISON_ALWAYS;
    depth.FrontFace=depth.BackFace={D3D11_STENCIL_OP_KEEP,D3D11_STENCIL_OP_KEEP,D3D11_STENCIL_OP_KEEP,D3D11_COMPARISON_ALWAYS};
    checked_aa(device->CreateDepthStencilState(&depth,&depth_),"Movie upscaling depth state");
}
MovieUpscaler::Image MovieUpscaler::create(unsigned width,unsigned height,DXGI_FORMAT format) const {
    Image image{{},{},{},width,height,format==DXGI_FORMAT_R16G16B16A16_FLOAT?8U:4U};
    D3D11_TEXTURE2D_DESC texture{};texture.Width=width;texture.Height=height;texture.MipLevels=texture.ArraySize=texture.SampleDesc.Count=1;
    texture.Format=format;texture.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET;
    checked_aa(device_->CreateTexture2D(&texture,nullptr,&image.texture),"Upscaled movie image");
    checked_aa(device_->CreateShaderResourceView(image.texture.Get(),nullptr,&image.view),"Upscaled movie view");
    checked_aa(device_->CreateRenderTargetView(image.texture.Get(),nullptr,&image.target),"Upscaled movie target");
    return image;
}
void MovieUpscaler::upscale(const D3D11_SUBRESOURCE_DATA& pixels,DXGI_FORMAT format,unsigned width,unsigned height,unsigned scale,const Image& output) {
    if(!width || !height || scale<2 || scale>4 || output.width!=width*scale || output.height!=height*scale)
        throw std::runtime_error("Invalid movie upscaling dimensions.");
    // Decoded frames replace one persistent source image; only the internal
    // resolution or movie size reallocates the transient filtering storage.
    if(!source_ || source_width_!=width || source_height_!=height || source_format_!=format) {
        source_view_.Reset();source_.Reset();source_width_=source_height_=scale_=0;
        D3D11_TEXTURE2D_DESC texture{};texture.Width=width;texture.Height=height;texture.MipLevels=texture.ArraySize=texture.SampleDesc.Count=1;
        texture.Format=format;texture.Usage=D3D11_USAGE_DEFAULT;texture.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        checked_aa(device_->CreateTexture2D(&texture,nullptr,&source_),"Movie source image");
        checked_aa(device_->CreateShaderResourceView(source_.Get(),nullptr,&source_view_),"Movie source view");
        chroma_={};chroma_=create((width+1)/2,(height+1)/2,DXGI_FORMAT_R16G16B16A16_FLOAT);
        source_width_=width;source_height_=height;source_format_=format;
    }
    if(intermediate_.width!=output.width || intermediate_.height!=output.height) {intermediate_={};intermediate_=create(output.width,output.height);}
    if(scale!=scale_) {
        struct Parameters {float ratio[2];int last[2];float sharpness,padding[3];};
        // Half a stop below RCAS's maximum strength suits compressed video.
        const Parameters parameters{{1.0f/scale,1.0f/scale},{int(width)-1,int(height)-1},0.70710678f,{}};
        D3D11_MAPPED_SUBRESOURCE mapped{};checked_aa(context_->Map(constants_.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&mapped),"Movie upscaling parameters");
        std::memcpy(mapped.pData,&parameters,sizeof(parameters));context_->Unmap(constants_.Get(),0);scale_=scale;
    }
    context_->UpdateSubresource(source_.Get(),0,nullptr,pixels.pSysMem,pixels.SysMemPitch,0);
    context_->RSSetState(raster_.Get());context_->OMSetDepthStencilState(depth_.Get(),0);context_->OMSetBlendState(nullptr,nullptr,UINT_MAX);
    context_->IASetInputLayout(nullptr);context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->VSSetShader(vertex_.Get(),nullptr,0);
    auto constants=constants_.Get();context_->PSSetConstantBuffers(0,1,&constants);
    ID3D11ShaderResourceView* const empty[2]{};
    const auto pass=[&](ID3D11PixelShader* shader,std::initializer_list<ID3D11ShaderResourceView*> inputs,const Image& target) {
        context_->PSSetShaderResources(0,2,empty);auto view=target.target.Get();context_->OMSetRenderTargets(1,&view,nullptr);
        const D3D11_VIEWPORT viewport{0,0,float(target.width),float(target.height),0,1};context_->RSSetViewports(1,&viewport);
        context_->PSSetShader(shader,nullptr,0);context_->PSSetShaderResources(0,UINT(inputs.size()),inputs.begin());context_->Draw(3,0);
    };
    pass(chroma_pass_.Get(),{source_view_.Get()},chroma_);
    pass(upscale_.Get(),{source_view_.Get(),chroma_.view.Get()},intermediate_);
    pass(sharpen_.Get(),{intermediate_.view.Get()},output);
    context_->PSSetShaderResources(0,2,empty);context_->OMSetRenderTargets(0,nullptr,nullptr);
}
MultisampleCopy::MultisampleCopy(ID3D11Device* device,ID3D11DeviceContext* context,std::uint64_t& compilations):context_(context) {
    const auto vertex=shader_binary(ShaderKind::display_vertex),pixel=shader_binary(ShaderKind::copy_multisample),stencil=shader_binary(ShaderKind::copy_multisample_stencil);
    compilations+=unsigned(vertex.compiled)+unsigned(pixel.compiled)+unsigned(stencil.compiled);
    checked_aa(device->CreateVertexShader(vertex.bytes.data(),vertex.bytes.size(),nullptr,&vertex_),"MSAA transfer vertex shader");
    checked_aa(device->CreatePixelShader(pixel.bytes.data(),pixel.bytes.size(),nullptr,&pixel_),"MSAA colour/depth transfer shader");
    checked_aa(device->CreatePixelShader(stencil.bytes.data(),stencil.bytes.size(),nullptr,&stencil_),"MSAA stencil transfer shader");
    const auto color=shader_binary(ShaderKind::copy_multisample_color);compilations+=unsigned(color.compiled);
    checked_aa(device->CreatePixelShader(color.bytes.data(),color.bytes.size(),nullptr,&color_),"MSAA colour transfer shader");
    for(unsigned i=0;i<resample_.size();++i) {
        const auto shader=shader_binary(static_cast<ShaderKind>(unsigned(ShaderKind::resample_target)+i));
        compilations+=unsigned(shader.compiled);
        checked_aa(device->CreatePixelShader(shader.bytes.data(),shader.bytes.size(),nullptr,&resample_[i]),"Surface resampling shader");
    }
    D3D11_BUFFER_DESC offset{};offset.ByteWidth=16;offset.Usage=D3D11_USAGE_DYNAMIC;
    offset.BindFlags=D3D11_BIND_CONSTANT_BUFFER;offset.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
    checked_aa(device->CreateBuffer(&offset,nullptr,&offset_),"MSAA copy offset");
    D3D11_RASTERIZER_DESC raster{};raster.FillMode=D3D11_FILL_SOLID;raster.CullMode=D3D11_CULL_NONE;raster.DepthClipEnable=raster.MultisampleEnable=TRUE;
    checked_aa(device->CreateRasterizerState(&raster,&raster_),"MSAA transfer rasterizer");
    D3D11_DEPTH_STENCIL_DESC depth{};depth.DepthEnable=TRUE;depth.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;depth.DepthFunc=D3D11_COMPARISON_ALWAYS;
    depth.FrontFace=depth.BackFace={D3D11_STENCIL_OP_KEEP,D3D11_STENCIL_OP_KEEP,D3D11_STENCIL_OP_KEEP,D3D11_COMPARISON_ALWAYS};
    checked_aa(device->CreateDepthStencilState(&depth,&depth_),"MSAA depth transfer state");
    depth.DepthEnable=FALSE;depth.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ZERO;depth.StencilEnable=TRUE;depth.StencilReadMask=255;
    {auto disabled=depth;disabled.StencilEnable=FALSE;
        checked_aa(device->CreateDepthStencilState(&disabled,&no_depth_),"MSAA colour transfer depth state");}
    depth.FrontFace=depth.BackFace={D3D11_STENCIL_OP_REPLACE,D3D11_STENCIL_OP_REPLACE,D3D11_STENCIL_OP_REPLACE,D3D11_COMPARISON_ALWAYS};
    for(unsigned bit=0;bit<8;++bit) {
        depth.StencilWriteMask=static_cast<UINT8>(1U<<bit);
        checked_aa(device->CreateDepthStencilState(&depth,&stencil_states_[bit]),"MSAA stencil transfer state");
        const std::array<unsigned,4> mask{1U<<bit,0,0,0};D3D11_BUFFER_DESC buffer{};buffer.ByteWidth=sizeof(mask);
        buffer.Usage=D3D11_USAGE_IMMUTABLE;buffer.BindFlags=D3D11_BIND_CONSTANT_BUFFER;const D3D11_SUBRESOURCE_DATA data{mask.data()};
        checked_aa(device->CreateBuffer(&buffer,&data,&stencil_masks_[bit]),"MSAA stencil transfer mask");
    }
}
void MultisampleCopy::color(ID3D11ShaderResourceView* source,ID3D11RenderTargetView* target,
    unsigned sx,unsigned sy,unsigned dx,unsigned dy,unsigned width,unsigned height) {
    const std::array<int,4> offset{int(sx)-int(dx),int(sy)-int(dy),0,0};
    D3D11_MAPPED_SUBRESOURCE mapped{};checked_aa(context_->Map(offset_.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&mapped),"MSAA copy parameters");
    std::memcpy(mapped.pData,offset.data(),sizeof(offset));context_->Unmap(offset_.Get(),0);
    const D3D11_VIEWPORT viewport{float(dx),float(dy),float(width),float(height),0,1};context_->RSSetViewports(1,&viewport);
    context_->RSSetState(raster_.Get());context_->OMSetBlendState(nullptr,nullptr,UINT_MAX);
    context_->IASetInputLayout(nullptr);context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->VSSetShader(vertex_.Get(),nullptr,0);context_->PSSetShader(color_.Get(),nullptr,0);
    context_->OMSetRenderTargets(1,&target,nullptr);context_->OMSetDepthStencilState(no_depth_.Get(),0);
    auto buffer=offset_.Get();context_->PSSetConstantBuffers(0,1,&buffer);context_->PSSetShaderResources(0,1,&source);context_->Draw(3,0);
    ID3D11ShaderResourceView* empty=nullptr;context_->PSSetShaderResources(0,1,&empty);context_->OMSetRenderTargets(0,nullptr,nullptr);
}
void MultisampleCopy::resample(ID3D11ShaderResourceView* color,ID3D11ShaderResourceView* depth,ID3D11ShaderResourceView* stencil,
    ID3D11RenderTargetView* target,ID3D11DepthStencilView* destination,unsigned width,unsigned height,
    unsigned source_scale,unsigned destination_scale,unsigned source_samples,unsigned destination_samples) {
    struct Parameters {float x,y;unsigned source,destination;};
    const float ratio=float(source_scale)/destination_scale;
    const Parameters parameters{ratio,ratio,source_samples,destination_samples};
    D3D11_MAPPED_SUBRESOURCE mapped{};
    checked_aa(context_->Map(offset_.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&mapped),"Surface resampling parameters");
    std::memcpy(mapped.pData,&parameters,sizeof(parameters));context_->Unmap(offset_.Get(),0);
    const D3D11_VIEWPORT viewport{0,0,float(width*destination_scale),float(height*destination_scale),0,1};
    context_->RSSetViewports(1,&viewport);context_->RSSetState(raster_.Get());
    context_->OMSetBlendState(nullptr,nullptr,UINT_MAX);context_->IASetInputLayout(nullptr);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->VSSetShader(vertex_.Get(),nullptr,0);context_->PSSetShader(resample_[source_samples>1?2:0].Get(),nullptr,0);
    context_->OMSetRenderTargets(1,&target,destination);context_->OMSetDepthStencilState(destination?depth_.Get():no_depth_.Get(),0);
    ID3D11ShaderResourceView* sources[]={color,depth,stencil};context_->PSSetShaderResources(0,3,sources);
    auto buffer=offset_.Get();context_->PSSetConstantBuffers(0,1,&buffer);context_->Draw(3,0);
    if(stencil) {
        context_->ClearDepthStencilView(destination,D3D11_CLEAR_STENCIL,1,0);
        context_->OMSetRenderTargets(0,nullptr,destination);context_->PSSetShader(resample_[source_samples>1?3:1].Get(),nullptr,0);
        for(unsigned bit=0;bit<8;++bit) {
            context_->OMSetDepthStencilState(stencil_states_[bit].Get(),1U<<bit);
            auto mask=stencil_masks_[bit].Get();context_->PSSetConstantBuffers(1,1,&mask);context_->Draw(3,0);
        }
    }
    ID3D11ShaderResourceView* empty[3]{};context_->PSSetShaderResources(0,3,empty);
    context_->OMSetRenderTargets(0,nullptr,nullptr);
}
void MultisampleCopy::grow(ID3D11ShaderResourceView* color,ID3D11ShaderResourceView* depth,ID3D11ShaderResourceView* stencil,
                           ID3D11RenderTargetView* target,ID3D11DepthStencilView* destination,unsigned width,unsigned height) {
    const D3D11_VIEWPORT viewport{0,0,float(width),float(height),0,1};context_->RSSetViewports(1,&viewport);
    context_->RSSetState(raster_.Get());context_->OMSetBlendState(nullptr,nullptr,UINT_MAX);
    context_->IASetInputLayout(nullptr);context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->VSSetShader(vertex_.Get(),nullptr,0);context_->PSSetShader(pixel_.Get(),nullptr,0);
    context_->OMSetRenderTargets(1,&target,destination);context_->OMSetDepthStencilState(destination?depth_.Get():no_depth_.Get(),0);
    ID3D11ShaderResourceView* sources[]={color,depth,stencil};context_->PSSetShaderResources(0,3,sources);context_->Draw(3,0);
    if(stencil) {
        context_->OMSetRenderTargets(0,nullptr,destination);context_->PSSetShader(stencil_.Get(),nullptr,0);
        for(unsigned bit=0;bit<8;++bit) {
            context_->OMSetDepthStencilState(stencil_states_[bit].Get(),1U<<bit);
            auto buffer=stencil_masks_[bit].Get();context_->PSSetConstantBuffers(0,1,&buffer);context_->Draw(3,0);
        }
    }
    ID3D11ShaderResourceView* empty[3]{};context_->PSSetShaderResources(0,3,empty);context_->OMSetRenderTargets(0,nullptr,nullptr);
}
}
