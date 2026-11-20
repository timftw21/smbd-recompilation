#include "rolling.h"
#include "disc.h"
#include "file.h"
#include "xbe.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <fstream>
#include <numbers>
#include <stdexcept>

namespace smbd {
namespace {
constexpr std::uint32_t output_rate=48000;
// MusyX ramps each volume change across one 5 ms DSP frame. The GameCube
// macro's key-off release is not on the Deluxe disc; 30 ms avoids clicks.
constexpr std::uint32_t ramp_frames=output_rate/200,release_frames=output_rate*3/100;
struct Recording {std::uint32_t entry,samples;};
// allse.scp maps SND_SE01_RUN01..03 (effects 248, 273, 274) to these
// one-based allse.acx entries. Each loops whole: the GameCube dumps of these
// effects have the same full lengths, and every ADX entry loops its file.
constexpr std::array<Recording,ClassicRolling::layers> recordings{{
    {111,6393},   // RUN01: steady hiss, 145 ms.
    {2,12682},    // RUN02: rumble swell from and back to silence, 288 ms.
    {118,17584},  // RUN03: clatter, 399 ms.
}};
std::uint32_t be32(std::span<const std::byte> data,std::size_t offset) {
    if(offset+4>data.size()) throw std::runtime_error("Truncated rolling sound bank data");
    std::uint32_t value=0;
    for(unsigned i=0;i<4;++i) value=value<<8|std::to_integer<std::uint32_t>(data[offset+i]);
    return value;
}
// Decodes a whole-file ADX loop as a looping decoder plays it: the predictor
// carries its history from the last samples into the first block, so the
// seam is the encoder's own continuation rather than a restart from zero.
std::vector<std::int16_t> decode_adx_loop(std::span<const std::byte> adx,std::uint32_t samples) {
    const auto byte=[&](std::size_t offset) {
        if(offset>=adx.size()) throw std::runtime_error("Truncated rolling ADX recording");
        return std::to_integer<int>(adx[offset]);
    };
    const auto be16=[&](std::size_t offset){return byte(offset)<<8|byte(offset+1);};
    // Mono 4-bit ADX, version 4, 500 Hz predictor cutoff, unencrypted.
    if(be16(0)!=0x8000 || byte(4)!=3 || byte(5)!=18 || byte(6)!=4 || byte(7)!=1 || be32(adx,8)!=ClassicRolling::rate ||
       be32(adx,12)!=samples || be16(16)!=500 || byte(18)!=4 || byte(19)!=0 || samples<2)
        throw std::runtime_error("Unexpected rolling recording in allse.acx");
    const std::size_t data=static_cast<std::size_t>(be16(2))+4;
    if(data+std::size_t(samples+31)/32*18>adx.size()) throw std::runtime_error("Truncated rolling ADX recording");
    const double a=std::numbers::sqrt2-std::cos(2*std::numbers::pi*500/ClassicRolling::rate),b=std::numbers::sqrt2-1;
    const double c=(a-std::sqrt((a+b)*(a-b)))/b;
    const int first=static_cast<int>(std::lrint(c*2*4096)),second=static_cast<int>(std::lrint(-c*c*4096));
    std::vector<std::int16_t> pcm(samples);
    int previous=0,older=0;
    for(unsigned pass=0;pass<2;++pass) for(std::uint32_t i=0;i<samples;++i) {
        const auto block=data+std::size_t(i/32)*18;
        const auto packed=byte(block+2+(i%32)/2);
        int nibble=(i&1)?packed&15:packed>>4;
        if(nibble>7) nibble-=16;
        const int sample=std::clamp(nibble*be16(block)+((first*previous+second*older)>>12),-32768,32767);
        older=previous;previous=sample;pcm[i]=static_cast<std::int16_t>(sample);
    }
    return pcm;
}
std::uint32_t frequency(double doppler) {
    // SMB1's MusyX (synth.s 800ECBC8) scales the voice pitch by doppler/8192.
    return static_cast<std::uint32_t>(std::clamp(std::lround(ClassicRolling::rate*doppler/8192),100L,200000L));
}
}

double rolling_curve(std::span<const std::byte> points,float x) {
    struct Point {std::int32_t x;float y,in,out;};
    const auto point=[&](std::size_t index) {
        if((index+1)*sizeof(Point)>points.size()) throw std::runtime_error("Rolling curve exceeds its table");
        Point value;std::memcpy(&value,points.data()+index*sizeof(Point),sizeof(Point));return value;
    };
    const auto first=point(0);
    if(static_cast<float>(first.x)>x) return first.y;
    std::size_t current=1;auto end=point(1);
    if(x>static_cast<float>(end.x)) for(;;) {
        if(end.x==0xFFFF) {--current;break;}
        end=point(++current);
        if(!(x>static_cast<float>(end.x))) break;
    }
    if(!current) throw std::runtime_error("Rolling curve has no segment");
    const auto start=point(current-1);end=point(current);
    const double width=static_cast<float>(end.x-start.x),t=(x-static_cast<float>(start.x))/width,t2=t*t,t3=t2*t;
    return ((t3-2*t2+t)*start.out+(t3-t2)*end.in)*width+(2*t3-3*t2+1)*start.y+(3*t2-2*t3)*end.y;
}

ClassicRolling::ClassicRolling(Disc& disc) {
    const auto bank=disc.lookup("media/snd/mkb/allse.acx");
    if(!bank || bank->directory()) throw std::runtime_error("The game disc has no media/snd/mkb/allse.acx");
    std::array<std::byte,8> header{};disc.read(*bank,0,header);
    if(be32(header,0) || be32(header,4)!=183) throw std::runtime_error("Unexpected allse.acx directory");
    for(unsigned layer=0;layer<layers;++layer) {
        const auto& recording=recordings[layer];
        std::array<std::byte,8> entry{};disc.read(*bank,8ULL*recording.entry,entry);
        const auto offset=be32(entry,0),size=be32(entry,4);
        if(size<64 || size>0x10000 || offset>bank->size || size>bank->size-offset) throw std::runtime_error("Invalid allse.acx entry");
        std::vector<std::byte> adx(size);disc.read(*bank,offset,adx);
        pcm_[layer]=decode_adx_loop(adx,recording.samples);
    }
}
float ClassicRolling::gain(unsigned volume,int request) {
    // E91E9..E9216 boosts the caller's volume by 1.5 before the layer curves;
    // the GameCube controller (sound.c func_8002CA5C) uses it unscaled.
    const auto boosted=static_cast<int>(std::min(127.0f,static_cast<float>(request)*1.5f));
    if(request<=0 || boosted<=0 || !volume) return 0;
    const double original=std::min(127.0,volume*static_cast<double>(request)/boosted);
    // MusyX's default volume table (hw_volconv.c musyx_vol_tab) follows
    // (v/127)^2.15 within 0.6 dB from 16 to 127.
    return static_cast<float>(std::pow(original/127,2.15));
}
std::uint32_t ClassicRolling::voice(Audio& audio,unsigned layer,std::uint32_t& id) {
    if(id) return id;
    id=audio.create({1,1,2,16,0,rate});
    const auto& pcm=pcm_[layer];
    audio.bind(id,std::as_bytes(std::span(pcm)));
    audio.region(id,true,0,static_cast<std::uint32_t>(pcm.size())*2);
    // CRI's driver (2CEE40) sends a centred mono effect to the centre bin at
    // full volume, matching MusyX's -3 dB per side for a centred voice.
    constexpr std::array<MixBin,1> centre{{{2,0}}};
    audio.headroom(id,0);audio.mixbins(id,centre);
    return id;
}
void ClassicRolling::start(Audio& audio,unsigned player,unsigned layer,unsigned volume,int request,double doppler) {
    auto& slot=voices_.at(player).at(layer);
    // A key-off may still be releasing the previous voice. MusyX starts a new
    // voice beside it, so alternate between two.
    if(const auto previous=slot.voices[slot.current];previous && audio.status(previous)) audio.ramp(previous,0,release_frames,true);
    slot.current^=1;
    const auto id=voice(audio,layer,slot.voices[slot.current]);
    audio.stop(id);audio.position(id,0);audio.ramp(id,0,0);
    audio.frequency(id,frequency(doppler));audio.play(id,true);
    audio.ramp(id,gain(volume,request),ramp_frames);
    active_[player]=true;
}
void ClassicRolling::update(Audio& audio,unsigned player,unsigned layer,unsigned volume,int request,double doppler) {
    if(!active(player)) return;
    const auto& slot=voices_.at(player).at(layer);
    const auto id=slot.voices[slot.current];
    if(!id) return;
    audio.ramp(id,gain(volume,request),ramp_frames);audio.frequency(id,frequency(doppler));
}
void ClassicRolling::release(Audio& audio,unsigned player) {
    if(!active(player)) return;
    for(const auto& slot:voices_[player]) if(const auto id=slot.voices[slot.current]) audio.ramp(id,0,release_frames,true);
    active_[player]=false;
}

std::string check_rolling_sound(Xbe& image,const std::filesystem::path& disc_path,const std::filesystem::path& output) {
    if(!image.supported()) throw std::runtime_error("Rolling sound check requires the verified executable");
    Disc disc(disc_path);
    if(disc.sha256(disc.find("default.xbe"))!=image.sha256) throw std::runtime_error("Disc executable does not match the verified target");
    // 581620 lists the three per-player layers as sound-table types 52..54;
    // 579960 maps those types to the effects allse.scp assigns to RUN01..03.
    const auto read32=[&](std::uint32_t address){const auto bytes=image.data(address,4);std::uint32_t value;std::memcpy(&value,bytes.data(),4);return value;};
    for(unsigned layer=0;layer<3;++layer) {
        const auto type=read32(0x581624+8*layer);
        const auto effect=read32(0x579960+16*type)&0xFFFF;
        if(type!=0x52+layer || effect!=std::array{248U,273U,274U}[layer]) throw std::runtime_error("Original rolling layer table differs");
    }
    std::array<std::vector<std::byte>,3> curves;
    for(unsigned layer=0;layer<3;++layer) curves[layer]=image.data(read32(0x581740+4*layer),32);
    const auto doppler_curve=image.data(0x58174C,32);
    if(rolling_curve(doppler_curve,0)!=4608 || rolling_curve(doppler_curve,127)!=15615) throw std::runtime_error("Original rolling doppler curve differs");

    ClassicRolling rolling(disc);
    std::string layers;
    bool passed=true;
    for(unsigned layer=0;layer<3;++layer) {
        const auto samples=rolling.samples(layer);constexpr std::size_t start=0;
        std::vector<double> steps;
        for(std::size_t i=start+1;i<samples.size();++i) steps.push_back(std::abs(double(samples[i])-samples[i-1]));
        const auto seam=std::abs(double(samples[start])-samples.back());
        std::ranges::sort(steps);
        const auto typical=steps[steps.size()*999/1000];
        const auto window=[&](std::size_t first,std::size_t count) {
            double sum=0;for(std::size_t i=0;i<count;++i){const double value=samples[start+(first+i)%(samples.size()-start)];sum+=value*value;}
            return 10*std::log10(sum/count+1e-9);
        };
        // Report the 5 ms level change across the seam beside the recording's
        // own window-to-window changes. Whole-file loops keep the recordings' own
        // swells, so only the waveform step gates the result.
        const auto loop=samples.size()-start,span=static_cast<std::size_t>(ClassicRolling::rate/200);
        const auto level_change=window(0,span)-window(loop-span,span);
        std::vector<double> changes;
        for(std::size_t position=span;position+span<=loop;position+=span/4)
            changes.push_back(std::abs(window(position,span)-window(position-span,span)));
        std::ranges::sort(changes);
        const auto typical_change=changes[changes.size()*99/100];
        passed&=seam<=typical;
        layers+=std::format("{}{{\"entry\":{},\"loop_start\":{},\"loop_frames\":{},\"seam_step\":{:.0f},\"step_99_9\":{:.0f},"
            "\"seam_level_change_db\":{:.2f},\"level_change_99_db\":{:.2f}}}",
            layer?",":"",recordings[layer].entry,start,loop,seam,typical,level_change,typical_change);
    }

    // Drive the voices like 8B907: speed 0..127 and volume 0..68 from km/h,
    // sent every eighth tick while rolling and at once on loss of contact.
    Audio audio(false);
    std::vector<float> mixed;
    std::array<float,800*2> frame{};
    bool started=false;double low_doppler=1e9,high_doppler=0;
    const auto request=[](double kmh,bool contact)->std::pair<unsigned,int> {
        if(!contact || kmh<=10) return {0,0};
        return {static_cast<unsigned>(std::min((kmh-10)*1.5,127.0)),static_cast<int>(static_cast<float>(std::min((kmh-10)*15,80.0))*0.85f)};
    };
    const auto controller=[&](unsigned speed,int volume) {
        const auto boosted=static_cast<int>(std::min(127.0f,static_cast<float>(volume)*1.5f));
        if(!boosted || !speed) {if(started) rolling.release(audio,0);started=false;return;}
        const auto doppler=rolling_curve(doppler_curve,static_cast<float>(speed));
        low_doppler=std::min(low_doppler,doppler);high_doppler=std::max(high_doppler,doppler);
        for(unsigned layer=0;layer<3;++layer) {
            const auto level=static_cast<unsigned>(rolling_curve(curves[layer],static_cast<float>(speed))*static_cast<float>(boosted*(1/127.0f)));
            if(started) rolling.update(audio,0,layer,level,volume,doppler);
            else rolling.start(audio,0,layer,level,volume,doppler);
        }
        started=true;
    };
    struct Phase {double seconds,from,to;bool contact;};
    constexpr std::array phases{Phase{0.5,0,0,true},Phase{4,10,100,true},Phase{0.15,100,100,false},Phase{1.35,100,100,true},
        Phase{2,100,15,true},Phase{0.5,15,15,false}};
    std::uint32_t tick=0;std::vector<std::pair<std::size_t,std::size_t>> bounds;
    for(const auto& phase:phases) {
        const auto first=mixed.size()/2;const auto ticks=static_cast<unsigned>(phase.seconds*60);
        for(unsigned i=0;i<ticks;++i,++tick) {
            const auto [speed,volume]=request(phase.from+(phase.to-phase.from)*i/ticks,phase.contact);
            if(!(tick&7) || !speed) controller(speed,volume);
            audio.mix(frame);mixed.insert(mixed.end(),frame.begin(),frame.end());
        }
        bounds.emplace_back(first,mixed.size()/2);
    }
    const auto rms=[&](std::size_t first,std::size_t end) {
        double sum=0;for(auto i=first*2;i<end*2;++i) sum+=double(mixed[i])*mixed[i];
        return sum?10*std::log10(sum/((end-first)*2)):-1000.0;
    };
    float peak=0;std::size_t clipped=0;
    for(const auto value:mixed) {peak=std::max(peak,std::abs(value));clipped+=std::abs(value)>=1;}
    // The last 0.5 s has no contact; after the release it must be silent.
    const auto release_end=bounds.back().first+release_frames+32;
    const bool silent_start=rms(bounds[0].first,bounds[0].second)==-1000,silent_end=rms(release_end,bounds.back().second)==-1000;
    const auto slow=rms(bounds[1].first,bounds[1].first+48000),fast=rms(bounds[3].first,bounds[3].second);
    passed&=clipped==0 && silent_start && silent_end && fast>slow && low_doppler<5000 && high_doppler>15000;

    std::filesystem::create_directories(output);
    const auto wav=output/"classic-rolling.wav";
    std::ofstream file(wav,std::ios::binary|std::ios::trunc);
    const auto put=[&](std::uint32_t value,unsigned bytes){for(unsigned i=0;i<bytes;++i) file.put(static_cast<char>(value>>(8*i)));};
    const auto data=static_cast<std::uint32_t>(mixed.size()*2);
    file.write("RIFF",4);put(36+data,4);file.write("WAVEfmt ",8);put(16,4);put(1,2);put(2,2);put(output_rate,4);put(output_rate*4,4);put(4,2);put(16,2);
    file.write("data",4);put(data,4);
    for(const auto value:mixed) put(static_cast<std::uint16_t>(static_cast<std::int16_t>(std::lround(std::clamp(value,-1.0f,1.0f)*32767))),2);
    if(!file) throw std::runtime_error("Cannot write the rolling sound render");
    return std::format("{{\"format\":\"smbd-rolling-check-v1\",\"passed\":{},\"layers\":[{}],\"render\":{{\"wav\":{},\"seconds\":{:.2f},"
        "\"peak\":{:.4f},\"clipped\":{},\"silent_before_roll\":{},\"silent_after_release\":{},\"slow_rms_db\":{:.1f},\"fast_rms_db\":{:.1f},"
        "\"doppler\":[{:.0f},{:.0f}],\"rate_hz\":[{},{}]}}}}",
        passed?"true":"false",layers,json(path_text(wav)),mixed.size()/2.0/output_rate,peak,clipped,silent_start,silent_end,slow,fast,
        low_doppler,high_doppler,frequency(low_doppler),frequency(high_doppler));
}
}
