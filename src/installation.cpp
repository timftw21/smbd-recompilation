#include "installation.h"
#include "disc.h"
#include "target.h"
#ifdef _WIN32
#include <Windows.h>
#endif
#include <algorithm>
#include <array>
#include <charconv>
#include <fstream>
#include <format>
#include <sstream>

namespace smbd {
namespace {
DiscEntry verified(Disc& disc) {
    const auto entry=disc.find("default.xbe");
    if(entry.size!=target::file_size || disc.sha256(entry)!=target::sha256)
        throw std::runtime_error("Select the supported US Xbox release of Super Monkey Ball Deluxe.");
    return entry;
}
void replace_file(const std::filesystem::path& temporary,const std::filesystem::path& output) {
#ifdef _WIN32
    if(!MoveFileExW(temporary.c_str(),output.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("Cannot save "+path_text(output)+std::format(" (Windows error {}).",GetLastError()));
#else
    std::filesystem::rename(temporary,output);
#endif
}
struct Temporary {
    std::filesystem::path path;
    explicit Temporary(std::filesystem::path value):path(std::move(value)) {
        if(std::filesystem::exists(path)) throw std::runtime_error("An unfinished installation file already exists: "+path_text(path));
    }
    ~Temporary() {std::error_code error;std::filesystem::remove(path,error);}
};
}
std::filesystem::path install_relative_path(std::string_view value,bool data) {
    const auto path=utf8_path(value);
    if(path.empty() || path.is_absolute() || path.has_root_name() || path.has_root_directory() || value.find_first_of("\r\n\t:")!=std::string_view::npos)
        throw std::runtime_error("Invalid path in installation manifest.");
    for(const auto& part:path) if(part==".." || part==".") throw std::runtime_error("Invalid installation path.");
    const auto first=*path.begin();
    if(first=="logs" || first=="captures" || (first=="data" && !(data && (path=="data/local/default.xbe" || path=="data/local/game.iso"))))
        throw std::runtime_error("The installation manifest includes private user data.");
    return path;
}
std::vector<PayloadEntry> payload_manifest(std::string_view text) {
    if(text.size()>256*1024) throw std::runtime_error("Installation manifest exceeds its size limit.");
    std::istringstream input{std::string(text)};
    std::string line;std::getline(input,line);
    if(line!="SMBD-PAYLOAD-V1") throw std::runtime_error("Invalid installation manifest.");
    std::vector<PayloadEntry> entries;
    while(std::getline(input,line)) {
        const auto a=line.find('\t'),b=line.find('\t',a==std::string::npos?0:a+1);
        if(a==std::string::npos || b==std::string::npos || entries.size()==128) throw std::runtime_error("Invalid payload entry.");
        std::uint64_t size=0;const auto parsed=std::from_chars(line.data()+a+1,line.data()+b,size);
        if(parsed.ec!=std::errc{} || parsed.ptr!=line.data()+b || line.size()-b-1!=64) throw std::runtime_error("Invalid payload size or checksum.");
        entries.push_back({install_relative_path(std::string_view(line).substr(0,a)),size,line.substr(b+1)});
    }
    if(entries.empty()) throw std::runtime_error("Setup does not contain any game files.");
    return entries;
}
std::string verify_install_disc(const std::filesystem::path& path,const InstallProgress& progress) {
    if(progress) progress(0,"Checking your game image");
    Disc disc(path);verified(disc);
    if(progress) progress(100,"Game image verified");
    return std::format("{{\"format\":\"smbd-install-disc-v1\",\"passed\":true,\"source_sha256\":{},\"disc_bytes\":{}}}",json(target::sha256),disc.size());
}
std::string install_game_files(const std::filesystem::path& iso,const std::filesystem::path& directory,bool copy_disc,
                              const InstallProgress& progress,const std::atomic_bool* cancel) {
    const auto cancelled=[&] {if(cancel && cancel->load()) throw std::runtime_error("Installation cancelled. Your original game image and saves were kept.");};
    cancelled();
    Disc disc(iso);const auto entry=verified(disc);
    const auto root=std::filesystem::absolute(directory).lexically_normal();
    if(root==root.root_path() || std::filesystem::weakly_canonical(root)!=root)
        throw std::runtime_error("Choose an installation folder without links or junctions.");
    const auto local=root/"data/local";
    if(std::filesystem::weakly_canonical(local)!=local) throw std::runtime_error("The game-data folder crosses a link or junction.");
    std::filesystem::create_directories(local);
    const auto executable=local/"default.xbe",settings=local/"settings.cfg",image=local/"game.iso";
    auto disc_path=std::filesystem::absolute(iso).lexically_normal();
    if(progress) progress(0,copy_disc?"Copying your game image":"Preparing game files");
    const bool same_image=std::filesystem::exists(image) && std::filesystem::equivalent(iso,image);
    if(copy_disc && !same_image) {
        if(std::filesystem::space(local).available<disc.size()+entry.size+16*1024*1024)
            throw std::runtime_error("There is not enough free space to copy the game image. Choose another folder or keep the image in its current location.");
        Temporary temporary(local/"game.iso.installing");
        File source(iso);std::ofstream output(temporary.path,std::ios::binary|std::ios::trunc);
        output.exceptions(std::ios::failbit|std::ios::badbit);
        std::vector<std::byte> buffer(1024*1024);unsigned previous=101;
        for(std::uint64_t offset=0;offset<source.size();) {
            cancelled();const auto count=std::min<std::uint64_t>(buffer.size(),source.size()-offset);
            source.read(offset,std::span(buffer).first(static_cast<std::size_t>(count)));
            output.write(reinterpret_cast<const char*>(buffer.data()),static_cast<std::streamsize>(count));offset+=count;
            const auto percent=static_cast<unsigned>(offset*100/source.size());
            if(progress && percent!=previous) {progress(percent,"Copying your game image");previous=percent;}
        }
        output.close();cancelled();
        {Disc copied(temporary.path);verified(copied);}
        replace_file(temporary.path,image);
    }
    if(copy_disc) disc_path=image;
    cancelled();
    Temporary extracted(local/"default.xbe.installing");disc.extract(entry,extracted.path);
    {File check(extracted.path);if(hash(check,0,check.size())!=target::sha256) throw std::runtime_error("Extracted game executable failed verification.");}
    std::string preferences;
    if(std::filesystem::exists(settings)) {
        if(std::filesystem::file_size(settings)>16384) throw std::runtime_error("Existing settings file exceeds 16 KiB.");
        std::ifstream input(settings);std::string line;
        while(std::getline(input,line)) if(!line.starts_with("executable=") && !line.starts_with("disc=")) preferences+=line+'\n';
    }
    const auto path=path_text(copy_disc?std::filesystem::path("data/local/game.iso"):disc_path);
    if(path.find_first_of("\r\n")!=std::string::npos) throw std::runtime_error("The game-image path contains a line break.");
    Temporary configured(local/"settings.cfg.installing");
    write_text(configured.path,"executable=data/local/default.xbe\ndisc="+path+'\n'+preferences,false);
    cancelled();replace_file(extracted.path,executable);replace_file(configured.path,settings);
    if(progress) progress(100,"Game files ready");
    return std::format("{{\"format\":\"smbd-install-files-v1\",\"passed\":true,\"source_sha256\":{},\"copied_disc\":{},\"directory\":{}}}",json(target::sha256),copy_disc,json(path_text(root)));
}
}
