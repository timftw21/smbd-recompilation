#include "audio.h"
#include "performance.h"


#include "file.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <bit>

#include <cmath>
#include <cstring>
#include <cstdio>
#include <format>
#include <stdexcept>

namespace smbd {
namespace {
constexpr int steps[]={7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,34,37,41,45,50,55,60,66,73,80,88,97,
    107,118,130,143,157,173,190,209,230,253,279,307,337,371,408,449,494,544,598,658,724,796,876,963,1060,1166,
    1282,1411,1552,1707,1878,2066,2272,2499,2749,3024,3327,3660,4026,4428,4871,5358,5894,6484,7132,7845,
    8630,9493,10442,11487,12635,13899,15289,16818,18500,20350,22385,24623,27086,29794,32767};
constexpr int indices[]={-1,-1,-1,-1,2,4,6,8,-1,-1,-1,-1,2,4,6,8};
struct StreamLock {
    SDL_AudioStream* stream;
    explicit StreamLock(SDL_AudioStream* value):stream(value) {
        if(stream && !SDL_LockAudioStream(stream)) throw std::runtime_error(SDL_GetError());
    }
    ~StreamLock(){if(stream) SDL_UnlockAudioStream(stream);}
};
void validate(AudioFormat format,bool bus=false) {
    if((format.channels!=1 && format.channels!=2) || format.rate<100 || format.rate>200000)
        throw std::runtime_error("Unsupported audio channel count or sample rate");
    if(format.tag==1) {
        // The SDK's effect-input bus uses 24-bit mixer precision in a
        // four-byte container. Its samples come from mix bins, not PCM bytes.
        const bool mixer=bus && format.bits==24 && format.block_align==format.channels*4;
        if(!mixer && ((format.bits!=8 && format.bits!=16) || format.block_align!=format.channels*format.bits/8))
            throw std::runtime_error("Invalid PCM format");
    } else if(format.tag==0x69) {
        if(format.bits!=4 || format.block_align!=36*format.channels || format.samples_per_block!=64)
            throw std::runtime_error("Invalid Xbox ADPCM format");
    } else throw std::runtime_error("Unsupported audio codec");
}
void decode_adpcm(AudioFormat format,const std::byte* source,std::span<float> result) {
    for(unsigned channel=0;channel<format.channels;++channel) {
        std::int16_t initial;std::memcpy(&initial,source+channel*4,2);
        int predictor=initial,index=std::to_integer<unsigned>(source[channel*4+2]);
        if(index>88) throw std::runtime_error("Invalid Xbox ADPCM step index");
        result[channel]=predictor/32768.0f;
        for(unsigned sample=1;sample<64;++sample) {
            const auto nibble_index=sample-1,byte_index=nibble_index/2;
            const auto offset=format.channels*4+(byte_index/4)*format.channels*4+channel*4+byte_index%4;
            const unsigned packed=std::to_integer<unsigned>(source[offset]);
            const auto nibble=(packed>>((nibble_index&1)*4))&15;
            const int step=steps[index];
            const int difference=(step>>3)+((nibble&1)?step>>2:0)+((nibble&2)?step>>1:0)+((nibble&4)?step:0);
            predictor=std::clamp(predictor+((nibble&8)?-difference:difference),-32768,32767);
            index=std::clamp(index+indices[nibble],0,88);
            result[sample*format.channels+channel]=predictor/32768.0f;
        }
    }
}
}
std::vector<float> decode_audio(AudioFormat format,std::span<const std::byte> data) {
    validate(format);
    if(data.size()%format.block_align) throw std::runtime_error("Audio payload ends inside a sample block");
    if(format.tag==1) {
        std::vector<float> result(data.size()/(format.bits/8));
        for(std::size_t i=0;i<result.size();++i) {
            if(format.bits==8) result[i]=(std::to_integer<int>(data[i])-128)/128.0f;
            else {std::int16_t value;std::memcpy(&value,data.data()+i*2,2);result[i]=value/32768.0f;}
        }
        return result;
    }
    const auto blocks=data.size()/format.block_align;
    std::vector<float> result(blocks*64*format.channels);
    for(std::size_t block=0;block<blocks;++block)
        decode_adpcm(format,data.data()+block*format.block_align,std::span(result).subspan(block*64*format.channels,64*format.channels));
    return result;
}
Audio::Audio(bool output,bool muted) {
    if(!output) return;
    if(!SDL_InitSubSystem(SDL_INIT_AUDIO)) throw std::runtime_error(SDL_GetError());
    const SDL_AudioSpec specification{SDL_AUDIO_F32,2,48000};
    stream_=SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,&specification,
        [](void* context,SDL_AudioStream* stream,int additional,int) {
            auto& audio=*static_cast<Audio*>(context);
            std::array<float,1024> buffer{};
            while(additional>0) {
                const auto frames=std::min<std::size_t>((static_cast<unsigned>(additional)+7)/8,buffer.size()/2);
                const bool recording=audio.recording_.load(std::memory_order_relaxed);
                const auto started=recording?performance_clock():0;
                try {audio.mix_unlocked(std::span(buffer).first(frames*2));}
                catch(const std::exception& error) {
                    audio.failed_=true;std::snprintf(audio.failure_.data(),audio.failure_.size(),"%s",error.what());break;
                }
                const int bytes=static_cast<int>(frames*2*sizeof(float));
                if(!SDL_PutAudioStreamData(stream,buffer.data(),bytes)) {audio.failed_=true;
                    std::snprintf(audio.failure_.data(),audio.failure_.size(),"%s",SDL_GetError());break;}
                additional-=bytes;
                if(recording) {
                    const auto ns=performance_clock()-started;audio.mix_ns_.fetch_add(ns,std::memory_order_relaxed);
                    audio.batches_.fetch_add(1,std::memory_order_relaxed);
                    if(ns>frames*1000000000ULL/48000) audio.overruns_.fetch_add(1,std::memory_order_relaxed);
                }
            }
        },this);
    if(!stream_) {const std::string error=SDL_GetError();SDL_QuitSubSystem(SDL_INIT_AUDIO);throw std::runtime_error(error);}
    if((muted && !SDL_SetAudioStreamGain(stream_,0.0f)) || !SDL_ResumeAudioStreamDevice(stream_)) {
        const std::string error=SDL_GetError();SDL_DestroyAudioStream(stream_);stream_=nullptr;
        SDL_QuitSubSystem(SDL_INIT_AUDIO);throw std::runtime_error(error);
    }
}
Audio::~Audio(){if(stream_){SDL_DestroyAudioStream(stream_);SDL_QuitSubSystem(SDL_INIT_AUDIO);}}
void Audio::output_gain(float gain) {
    if(!std::isfinite(gain) || gain<0 || gain>1) throw std::runtime_error("Audio gain must be 0..1");
    if(stream_ && !SDL_SetAudioStreamGain(stream_,gain)) throw std::runtime_error(SDL_GetError());
}
Audio::Voice& Audio::voice(std::uint32_t id) {
    if(failed_) throw std::runtime_error(failure_.data());
    if(!id || id>voices_.size() || !voices_[id-1].used) throw std::runtime_error("Invalid native audio voice");
    return voices_[id-1];
}
std::uint32_t Audio::create(AudioFormat format,AudioBus bus,std::uint32_t input_bin) {
    if(bus==AudioBus::submix && input_bin==UINT32_MAX) input_bin=31;
    if((bus==AudioBus::none && input_bin!=UINT32_MAX) || (bus!=AudioBus::none && input_bin>=32))
        throw std::runtime_error("Invalid audio input mix bin");
    validate(format,bus!=AudioBus::none);StreamLock lock(stream_);
    for(unsigned i=0;i<voices_.size();++i) if(!voices_[i].used) {
        auto& value=voices_[i];value=Voice{};value.format=format;value.step=format.rate/48000.0;
        value.used=true;value.bus=bus;value.input_bin=input_bin;value.bin_gains.fill(1);return i+1;
    }
    throw std::runtime_error("Native audio voice capacity exhausted");
}
void Audio::release(std::uint32_t id) {
    Voice retired;
    {StreamLock lock(stream_);retired=std::move(voice(id));voices_[id-1]=Voice{};
        for(auto& value:voices_) if(value.route==id) value.route=0;}
}
std::uint32_t Audio::frames(AudioFormat format,std::uint32_t bytes) {
    if(bytes%format.block_align) throw std::runtime_error("Audio region is not block-aligned");
    return bytes/format.block_align*(format.tag==0x69?64:1);
}
void Audio::data(std::uint32_t id,std::span<const std::byte> bytes) {
    AudioFormat format;{StreamLock lock(stream_);const auto& value=voice(id);format=value.format;
        if(value.bus!=AudioBus::none && !bytes.empty()) throw std::runtime_error("Mix buses cannot carry encoded sample data");}
    frames(format,static_cast<std::uint32_t>(bytes.size()));
    std::vector<std::byte> encoded(bytes.begin(),bytes.end());
    StreamLock lock(stream_);auto& value=voice(id);
    value.encoded.swap(encoded);value.source=value.encoded;value.cached_block=UINT32_MAX;value.source_address=0;
    value.byte_count=static_cast<std::uint32_t>(bytes.size());
    value.cursor=0;value.play_start=0;value.play_frames=frames(value.format,value.byte_count);
    value.environment_history={};
    value.loop_start=0;value.loop_frames=value.play_frames;value.playing=false;
}
void Audio::bind(std::uint32_t id,std::span<const std::byte> bytes,std::uint32_t source_address) {
    std::vector<std::byte> retired;
    StreamLock lock(stream_);auto& value=voice(id);
    if(value.bus!=AudioBus::none && !bytes.empty()) throw std::runtime_error("Mix buses cannot bind encoded sample data");
    value.encoded.swap(retired);value.source=bytes;value.cached_block=UINT32_MAX;value.source_address=source_address;
    value.byte_count=static_cast<std::uint32_t>(bytes.size());value.cursor=0;value.play_start=0;
    value.environment_history={};
    value.play_frames=value.byte_count/value.format.block_align*(value.format.tag==0x69?64:1);
    value.loop_start=0;value.loop_frames=value.play_frames;value.playing=false;
}
void Audio::format(std::uint32_t id,AudioFormat format) {
    StreamLock lock(stream_);auto& value=voice(id);validate(format,value.bus!=AudioBus::none);
    if(value.format==format) {value.step=format.rate/48000.0;return;}
    const auto bytes=[&](std::uint32_t count){return count/(value.format.tag==0x69?64:1)*value.format.block_align;};
    // The bound size is memory capacity; only complete codec blocks are playable.
    // Default regions can include allocation padding after the last block.
    const auto count=[&](std::uint32_t length){return length/format.block_align*(format.tag==0x69?64:1);};
    const auto play_start=frames(format,bytes(value.play_start));
    const auto play_frames=count(bytes(value.play_start+value.play_frames))-play_start;
    const auto loop_start=frames(format,bytes(value.loop_start));
    const auto loop_frames=count(bytes(value.loop_start+value.loop_frames))-loop_start;
    const auto cursor_bytes=bytes(static_cast<std::uint32_t>(value.cursor));
    const auto cursor=cursor_bytes/format.block_align*(format.tag==0x69?64:1);
    value.format=format;value.step=format.rate/48000.0;value.cached_block=UINT32_MAX;
    value.environment_history={};
    value.play_start=play_start;value.play_frames=play_frames;value.loop_start=loop_start;value.loop_frames=loop_frames;
    value.cursor=std::min(cursor,play_frames);
}
std::array<float,2> Audio::sample(Voice& value,std::uint32_t frame) {
    const auto& format=value.format;std::array<float,2> result{};
    if(format.tag==0x69) {
        const auto block=frame/64;
        if(value.cached_block!=block) {
            const auto offset=std::size_t(block)*format.block_align;
            if(offset+format.block_align>value.source.size()) throw std::runtime_error("Xbox ADPCM block exceeds the bound buffer");
            try {decode_adpcm(format,value.source.data()+offset,value.block);}
            catch(const std::exception& error) {
                throw std::runtime_error(std::format("{}: voice {}, block {}, channels {}, address {}, header {}, source {} size {}, play {}+{}, loop {}+{}, cursor {}",
                    error.what(),&value-voices_.data()+1,block,format.channels,
                    hex32(value.source_address+static_cast<std::uint32_t>(offset)),hex_bytes(value.source.subspan(offset,format.channels*4)),
                    hex32(value.source_address),value.byte_count,value.play_start,value.play_frames,value.loop_start,value.loop_frames,
                    static_cast<std::uint32_t>(value.cursor)));
            }
            value.cached_block=block;
        }
        for(unsigned channel=0;channel<format.channels;++channel) result[channel]=value.block[(frame%64)*format.channels+channel];
    } else for(unsigned channel=0;channel<format.channels;++channel) {
        const auto offset=(std::size_t(frame)*format.channels+channel)*(format.bits/8);
        if(format.bits==8) result[channel]=(std::to_integer<int>(value.source[offset])-128)/128.0f;
        else {std::int16_t sample;std::memcpy(&sample,value.source.data()+offset,2);result[channel]=sample/32768.0f;}
    }
    return result;
}
void Audio::play(std::uint32_t id,bool loop) {
    StreamLock lock(stream_);auto& value=voice(id);
    if(value.bus==AudioBus::none && !value.play_frames) throw std::runtime_error("Cannot play an empty audio buffer");
    if(value.cursor>=value.play_frames) value.cursor=0;
    value.loop=loop;value.playing=true;value.cached_block=UINT32_MAX;
}
void Audio::stop(std::uint32_t id){StreamLock lock(stream_);voice(id).playing=false;}
std::uint32_t Audio::status(std::uint32_t id){StreamLock lock(stream_);const auto& value=voice(id);return value.playing?(value.loop?5:1):0;}
AudioPosition Audio::positions(std::uint32_t id) {
    StreamLock lock(stream_);const auto& value=voice(id);
    const auto frame=static_cast<std::uint32_t>(value.cursor);
    const auto samples=value.format.tag==0x69?64U:1U;
    const auto bytes=[&](std::uint32_t count){return count/samples*value.format.block_align;};
    const auto play=bytes(frame);
    if(!value.playing || !value.byte_count) return {play,play};
    // The SDK write cursor stays at least one codec
    // block ahead, with separate loop-region and allocation-capacity wrapping.
    const auto ahead=std::max(bytes(32),std::uint32_t(value.format.block_align));
    const auto loop_start=bytes(value.loop_start),loop_size=bytes(value.loop_frames);
    const auto write=value.loop && loop_size && play>=loop_start && play-loop_start<loop_size
        ?loop_start+(play+ahead)%loop_size:(play+ahead)%value.byte_count;
    return {play,write};
}
std::uint32_t Audio::position(std::uint32_t id) {return positions(id).play;}
void Audio::position(std::uint32_t id,std::uint32_t bytes) {
    StreamLock lock(stream_);auto& value=voice(id);
    const auto frame=frames(value.format,bytes);
    if(frame>value.play_frames) throw std::runtime_error("Audio position exceeds the play region");
    value.cursor=frame;value.cached_block=UINT32_MAX;
    value.environment_history={};
}
void Audio::region(std::uint32_t id,bool loop,std::uint32_t start,std::uint32_t bytes) {
    StreamLock lock(stream_);auto& value=voice(id);
    const auto first=frames(value.format,start),count=frames(value.format,bytes);
    const auto total=loop?value.play_frames:value.byte_count/value.format.block_align*(value.format.tag==0x69?64:1);
    if(first>total || count>total-first) throw std::runtime_error("Audio region exceeds its buffer");
    if(loop){value.loop_start=first;value.loop_frames=count?count:total-first;}
    else {value.play_start=first;value.play_frames=count?count:total-first;value.cursor=0;value.loop_start=0;value.loop_frames=value.play_frames;}
}
void Audio::volume(std::uint32_t id,std::int32_t db) {
    if(db>0 || db<-10000) throw std::runtime_error("Audio volume is outside -10000..0");
    StreamLock lock(stream_);voice(id).gain=db==-10000?0:std::pow(10.0f,db/2000.0f);
}
void Audio::headroom(std::uint32_t id,std::uint32_t db) {
    if(db>10000) throw std::runtime_error("Audio headroom is outside 0..10000");
    StreamLock lock(stream_);voice(id).headroom=std::pow(10.0f,-static_cast<float>(db)/2000.0f);
}
void Audio::mixbin_headroom(std::uint32_t bin,std::uint32_t shift) {
    if(bin>=32 || shift>7) throw std::runtime_error("Invalid audio mix-bin headroom");
    StreamLock lock(stream_);bin_headroom_[bin]=shift;
}
void Audio::mixbins(std::uint32_t id,std::span<const MixBin> bins,bool volumes_only) {
    if(bins.size()>(volumes_only?32U:8U)) throw std::runtime_error("Audio mix-bin count exceeds its native capacity");
    for(const auto& bin:bins) if(bin.bin>=32 || bin.volume>0 || bin.volume<-10000)
        throw std::runtime_error("Invalid audio mix-bin assignment or volume");
    StreamLock lock(stream_);auto& value=voice(id);
    if(!volumes_only) value.bin_count=static_cast<std::uint32_t>(bins.size());
    for(unsigned i=0;i<bins.size();++i) {
        const auto& bin=bins[i];
        if(!volumes_only) value.bins[i]=bin.bin;
        value.bin_gains[bin.bin]=bin.volume==-10000?0:std::pow(10.0f,bin.volume/2000.0f);
    }
}
void Audio::frequency(std::uint32_t id,std::uint32_t hz) {
    StreamLock lock(stream_);auto& value=voice(id);
    if(!hz) hz=value.format.rate;
    if(hz<100 || hz>200000) throw std::runtime_error("Audio frequency is outside 100..200000");
    value.step=hz/48000.0;
}
void Audio::route(std::uint32_t id,std::uint32_t destination) {
    StreamLock lock(stream_);auto& value=voice(id);
    for(auto cursor=destination;cursor;cursor=voice(cursor).route) if(cursor==id)
        throw std::runtime_error("Audio routing contains a cycle");
    value.route=destination;
}
void Audio::spatial(std::uint32_t id,const SpatialResult& adjustment) {
    const auto unit=[](float value){return !std::isfinite(value) || value<0 || value>1;};
    if(unit(adjustment.gain) || !std::isfinite(adjustment.pitch) || adjustment.pitch<=0 ||
        std::ranges::any_of(adjustment.pan,unit) || std::ranges::any_of(adjustment.front_back,unit) ||
        std::ranges::any_of(adjustment.environment_pole,[](float value){return !std::isfinite(value) || value<0 || value>=1;}) ||
        std::ranges::any_of(adjustment.environment_gain,[](float value){return !std::isfinite(value) || value<0 || value>std::sqrt(10.0f);}))
        throw std::runtime_error("Invalid native 3D audio adjustment");
    StreamLock lock(stream_);auto& value=voice(id);
    value.spatial_gain=adjustment.gain;value.spatial_pan=adjustment.pan;value.spatial_pitch=adjustment.pitch;
    value.front_back=adjustment.front_back;value.environment_gain=adjustment.environment_gain;value.environment_pole=adjustment.environment_pole;
}
void Audio::mix_voice(Voice& value,std::span<const std::array<float,2>> samples,std::span<float> output,unsigned frame_count) {
    float gain=value.gain*value.headroom*value.spatial_gain;auto pan=value.spatial_pan;const Voice* sink=&value;

    for(auto cursor=value.route;cursor;cursor=voices_[cursor-1].route) {
        const auto& destination=voices_[cursor-1];if(!destination.used){gain=0;break;}
        if(destination.input_bin!=UINT32_MAX) gain*=sink->bin_gains[destination.input_bin];
        gain*=destination.gain*destination.headroom*destination.spatial_gain;
        for(unsigned i=0;i<2;++i) pan[i]*=destination.spatial_pan[i];sink=&destination;
    }
    const auto channels=value.format.channels;
    std::array<float,8> gains{};
    for(unsigned i=0;i<sink->bin_count;++i) {
        const auto bin=sink->bins[i];
        gains[i]=gain*sink->bin_gains[bin]*((bin==0 || bin==4 || bin==6 || bin==8)?pan[0]:(bin==1 || bin==5 || bin==7 || bin==9)?pan[1]:1);
        if(bin>=6 && bin<=9) gains[i]*=value.front_back[bin>=8]*value.environment_gain[0];
        else if(bin==2) gains[i]*=value.environment_gain[0];
        else if(bin==10) {
            gains[i]*=value.environment_gain[1];
            if(gains[i]) throw std::runtime_error("Environmental audio requires an assigned reverb effect");
        }
    }
    for(unsigned frame=0;frame<samples.size();++frame) {
        std::array<std::array<float,2>,2> filtered{};
        for(unsigned path=0;path<2;++path) for(unsigned channel=0;channel<channels;++channel) {
            const auto pole=value.environment_pole[path];
            auto& previous=value.environment_history[path][channel];
            previous=pole?(1-pole)*samples[frame][channel]+pole*previous:samples[frame][channel];filtered[path][channel]=previous;
        }
        for(unsigned bin=0;bin<sink->bin_count;++bin) {
            const auto target=sink->bins[bin];
            const auto& signal=target==10?filtered[1]:(target==2 || (target>=6 && target<=9))?filtered[0]:samples[frame];
            output[target*frame_count+frame]+=signal[bin%channels]*gains[bin];
        }
    }
}
void Audio::mix_bins(std::span<float> output,unsigned frame_count) {
    std::fill(output.begin(),output.end(),0.0f);
    for(auto& value:voices_) {
        if(!value.used || !value.playing || value.bus!=AudioBus::none) continue;
        const auto channels=value.format.channels;
        std::array<std::array<float,2>,32> samples;unsigned count=0;
        for(;count<frame_count;++count) {
            if(value.cursor>=value.play_frames) {
                if(!value.loop || !value.loop_frames){value.playing=false;break;}
                value.cursor=value.loop_start+std::fmod(value.cursor-value.loop_start,static_cast<double>(value.loop_frames));
                value.cached_block=UINT32_MAX;
            }
            if(value.loop && value.cursor>=value.loop_start+value.loop_frames) {
                value.cursor=value.loop_start+std::fmod(value.cursor-value.loop_start,static_cast<double>(value.loop_frames));
                value.cached_block=UINT32_MAX;
            }
            const auto first=static_cast<std::uint32_t>(value.cursor);
            auto second=first+1;
            if(value.loop && second>=value.loop_start+value.loop_frames) second=value.loop_start;
            else second=std::min(second,value.play_frames-1);
            const auto blend=static_cast<float>(value.cursor-first);
            const auto a=sample(value,value.play_start+first),b=sample(value,value.play_start+second);
            for(unsigned channel=0;channel<channels;++channel) samples[count][channel]=a[channel]+(b[channel]-a[channel])*blend;
            value.cursor+=value.step*value.spatial_pitch;
        }
        mix_voice(value,std::span(samples).first(count),output,frame_count);
        if(!value.loop && value.cursor>=value.play_frames){value.cursor=value.play_frames;value.playing=false;}
    }
    for(unsigned bin=0;bin<32;++bin) {
        const auto gain=1.0f/(1U<<bin_headroom_[bin]);
        for(auto& sample:output.subspan(bin*frame_count,frame_count)) sample=std::clamp(sample*gain,-1.0f,1.0f);
    }
}
void Audio::mix_sources(std::span<float> output) {
    std::array<float,32*32> bins{};
    for(std::size_t start=0;start<output.size()/2;start+=32) {
        const auto count=static_cast<unsigned>(std::min<std::size_t>(32,output.size()/2-start));
        mix_bins(std::span(bins).first(count*32),count);
        for(unsigned i=0;i<count;++i) {
            const auto center=bins[2*count+i]*0.70710678f+bins[3*count+i]*0.5f;
            output[(start+i)*2]=std::clamp(bins[i]+center+bins[4*count+i]*0.70710678f,-1.0f,1.0f);
            output[(start+i)*2+1]=std::clamp(bins[count+i]+center+bins[5*count+i]*0.70710678f,-1.0f,1.0f);
        }
        if(std::ranges::any_of(std::span(bins).subspan(count*6,count*26),[](float value){return value!=0;}))
            throw std::runtime_error("Effect send requires a compiled audio image");
    }
}
std::string Audio::failure(){StreamLock lock(stream_);return failed_?failure_.data():std::string{};}
void Audio::mix_unlocked(std::span<float> output) {
    mix_sources(output);frames_processed_.fetch_add(static_cast<std::uint32_t>(output.size()/2),std::memory_order_relaxed);
}
void Audio::mix(std::span<float> output) {
    if(output.size()%2) throw std::runtime_error("Audio mixer requires stereo frames");
    StreamLock lock(stream_);mix_unlocked(output);
}
std::string check_audio() {
    unsigned checks=0;
    const auto require=[&](bool condition){++checks;if(!condition) throw std::runtime_error("Offline audio check failed");};
    const AudioFormat pcm{1,1,2,16,0,48000},stereo{1,2,4,16,0,48000},adpcm{0x69,1,36,4,64,48000};
    const std::array<std::int16_t,4> samples{8192,-8192,16384,-16384};
    const auto bytes=std::as_bytes(std::span(samples));
    const auto decoded=decode_audio(pcm,bytes);require(decoded.size()==4 && decoded[0]==0.25f && decoded[1]==-0.25f);
    require(decode_audio(stereo,bytes)==decoded);
    const std::array<std::byte,3> unsigned_pcm{std::byte{0},std::byte{128},std::byte{255}};
    const auto unsigned_samples=decode_audio({1,1,1,8,0,48000},unsigned_pcm);
    require(unsigned_samples[0]==-1 && unsigned_samples[1]==0 && unsigned_samples[2]==127/128.0f);
    std::array<std::byte,72> block{};const std::int16_t left=1024,right=-1024;
    std::memcpy(block.data(),&left,2);std::memcpy(block.data()+4,&right,2);
    const auto mono=decode_audio(adpcm,std::span(block).first(36));require(mono.size()==64 && mono.front()==1/32.0f);
    const auto two=decode_audio({0x69,2,72,4,64,48000},block);
    require(two.size()==128 && two[0]==1/32.0f && two[1]==-1/32.0f && two[126]==1/32.0f && two[127]==-1/32.0f);
    block[2]=std::byte{89};bool rejected=false;try{decode_audio(adpcm,std::span(block).first(36));}catch(const std::exception&){rejected=true;}require(rejected);
    rejected=false;try{decode_audio(pcm,bytes.first(3));}catch(const std::exception&){rejected=true;}require(rejected);
    Audio audio(false);const AudioFormat bus_format{1,1,4,24,0,48000};
    const auto source=audio.create(pcm),bus=audio.create(bus_format,AudioBus::submix);
    rejected=false;try{audio.bind(bus,bytes);}catch(const std::exception&){rejected=true;}require(rejected);
    const auto changing=audio.create(pcm);audio.data(changing,bytes);audio.format(changing,stereo);
    audio.play(changing,false);std::array<float,4> reformatted{};audio.mix(reformatted);
    require(reformatted==std::array<float,4>{0.25f,-0.25f,0.5f,-0.5f} && audio.position(changing)==8 && !audio.status(changing));
    audio.format(changing,pcm);block[2]=std::byte{0};audio.data(changing,block);
    const AudioFormat stereo_adpcm{0x69,2,72,4,64,48000};audio.format(changing,stereo_adpcm);audio.format(changing,stereo_adpcm);
    audio.play(changing,false);std::array<float,128> recoded{};audio.mix(recoded);
    require(recoded[0]==1/32.0f && recoded[1]==-1/32.0f && recoded[126]==1/32.0f && recoded[127]==-1/32.0f);
    require(audio.position(changing)==72 && !audio.status(changing));audio.release(changing);
    std::array<std::int16_t,2> bound{8192,-8192};const auto dynamic=audio.create(pcm);
    audio.bind(dynamic,std::as_bytes(std::span(bound)));bound[0]=16384;audio.play(dynamic,true);
    std::array<float,4> bound_mix{};audio.mix(bound_mix);
    require(bound_mix==std::array<float,4>{0.5f,0.5f,-0.25f,-0.25f});audio.stop(dynamic);
    std::array<std::byte,74> padded{};std::copy(block.begin(),block.end(),padded.begin());
    audio.bind(dynamic,padded);audio.format(dynamic,stereo_adpcm);audio.play(dynamic,false);audio.mix(recoded);
    require(recoded[0]==1/32.0f && recoded[1]==-1/32.0f && audio.position(dynamic)==72 && !audio.status(dynamic));audio.release(dynamic);
    const auto cursor=audio.create(pcm);const std::array<std::byte,140> silence{};audio.data(cursor,silence);
    audio.region(cursor,false,10,100);audio.play(cursor,false);
    auto positions=audio.positions(cursor);require(positions.play==0 && positions.write==64);
    audio.position(cursor,80);positions=audio.positions(cursor);require(positions.play==80 && positions.write==4);
    audio.region(cursor,true,6,20);audio.position(cursor,10);audio.play(cursor,true);
    positions=audio.positions(cursor);require(positions.play==10 && positions.write==20);
    audio.stop(cursor);positions=audio.positions(cursor);require(positions.play==10 && positions.write==10);audio.release(cursor);
    const auto compressed_cursor=audio.create(stereo_adpcm);const std::array<std::byte,216> compressed_silence{};
    audio.data(compressed_cursor,compressed_silence);audio.play(compressed_cursor,false);
    positions=audio.positions(compressed_cursor);require(positions.play==0 && positions.write==72);audio.release(compressed_cursor);
    audio.data(source,bytes);audio.play(source,false);std::array<float,8> mixed{};audio.mix(mixed);
    require(mixed[0]==0.25f && mixed[1]==0.25f && mixed[6]==-0.5f && audio.status(source)==0 && audio.position(source)==8);
    audio.position(source,0);audio.volume(source,-2000);audio.volume(bus,-2000);audio.route(source,bus);audio.play(source,true);audio.mix(mixed);
    require(std::abs(mixed[0]-0.0025f)<0.000001f && audio.status(source)==5);
    audio.stop(source);audio.mix(mixed);require(std::ranges::all_of(mixed,[](float value){return value==0;}));
    audio.route(source,0);audio.volume(source,0);audio.region(source,true,2,4);audio.position(source,0);audio.play(source,true);
    std::array<float,12> looped{};audio.mix(looped);require(looped[0]==0.25f && looped[2]==-0.25f && looped[4]==0.5f && looped[6]==-0.25f && looped[10]==-0.25f);
    audio.stop(source);audio.region(source,false,0,8);audio.position(source,0);audio.frequency(source,24000);audio.play(source,false);audio.mix(mixed);
    require(mixed[0]==0.25f && mixed[2]==0 && audio.position(source)==4);
    rejected=false;try{audio.route(bus,source);audio.route(source,bus);}catch(const std::exception&){rejected=true;}require(rejected);
    audio.route(bus,0);audio.route(source,0);audio.stop(source);audio.frequency(source,48000);audio.position(source,0);
    const std::array<MixBin,1> right_only{{{1,-2000}}};audio.mixbins(source,right_only);audio.headroom(source,2000);
    audio.play(source,true);audio.mix(mixed);require(mixed[0]==0 && std::abs(mixed[1]-0.0025f)<0.000001f);
    audio.stop(source);audio.position(source,0);audio.headroom(source,0);audio.mixbin_headroom(1,1);audio.play(source,true);audio.mix(mixed);
    require(mixed[0]==0 && std::abs(mixed[1]-0.0125f)<0.000001f);
    const std::array<MixBin,1> mute_right{{{1,-10000}}};audio.mixbins(source,mute_right,true);audio.mix(mixed);
    require(std::ranges::all_of(mixed,[](float value){return value==0;}));
    rejected=false;try{audio.mixbin_headroom(32,0);}catch(const std::exception&){rejected=true;}require(rejected);
    rejected=false;try{const std::array<MixBin,1> invalid{{{1,1}}};audio.mixbins(source,invalid);}catch(const std::exception&){rejected=true;}require(rejected);
    audio.mixbins(source,{});audio.mix(mixed);require(std::ranges::all_of(mixed,[](float value){return value==0;}));
    audio.release(source);rejected=false;try{audio.status(source);}catch(const std::exception&){rejected=true;}require(rejected);
    Audio distance(false);const auto emitter=distance.create(pcm);distance.data(emitter,bytes);distance.play(emitter,true);
    distance.spatial(emitter,{.gain=0.5f});distance.mix(mixed);require(mixed[0]==0.125f && mixed[1]==0.125f);
    distance.position(emitter,0);distance.spatial(emitter,{.pan={0,1}});distance.mix(mixed);require(mixed[0]==0 && mixed[1]==0.25f);
    distance.position(emitter,0);distance.spatial(emitter,{.pitch=0.5});distance.mix(mixed);require(mixed[0]==0.25f && mixed[2]==0);
    rejected=false;try{distance.spatial(emitter,{.gain=-1});}catch(const std::exception&){rejected=true;}require(rejected);
    distance.position(emitter,0);distance.spatial(emitter,{.environment_gain={0.5f,0},.environment_pole={0.5f,0}});
    const std::array<MixBin,1> center_only{{{2,0}}};distance.mixbins(emitter,center_only);distance.mix(mixed);
    require(std::abs(mixed[0]-0.25f*0.5f*0.5f*0.70710678f)<0.000001f && mixed[0]==mixed[1]);
    distance.position(emitter,0);distance.mix(mixed);require(std::abs(mixed[0]-0.04419417f)<0.000001f);
    return "{\"format\":\"smbd-audio-check-v1\",\"passed\":true,\"game_booted\":false,\"audio_device_opened\":false,\"cases\":"+std::to_string(checks)+"}";
}
}
