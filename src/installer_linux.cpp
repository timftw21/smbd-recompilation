// Linux release installer: one self-contained executable with the verified
// application payload, matching the Windows setup's install/uninstall rules.
#include "installation.h"
#include "file.h"
#include "diagnostics.h"
#include "ui_theme.h"
#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>
#include <lzma.h>
#include <fcntl.h>
#include <spawn.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cfloat>
#include <charconv>
#include <cmath>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <format>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <thread>
#include <utility>

extern char** environ;
extern "C" {
extern const unsigned char smbd_installer_payload[],smbd_installer_manifest[],smbd_installer_icon[];
extern const std::uint64_t smbd_installer_payload_size,smbd_installer_manifest_size,smbd_installer_icon_size;
}

namespace fs=std::filesystem;
namespace {
constexpr char title[]="Super Monkey Ball Deluxe";
constexpr char uninstaller[]="smbd-uninstall";
constexpr char entry_name[]="smbd-recompilation.desktop";
constexpr std::string_view marker=smbd::install_marker;
constexpr std::array<std::string_view,3> executables{"smbd","smbd-tool",uninstaller};
constexpr ImU32 cream=IM_COL32(255,242,201,255),gold=IM_COL32(255,212,82,255),brown=IM_COL32(48,27,10,255),
    yellow=IM_COL32(255,204,31,255),rim=IM_COL32(183,129,51,255),warning=IM_COL32(255,200,134,255);
struct Box {float left,top,right,bottom;};
// Layout in points, shared with the Windows setup window.
constexpr Box form_panel{24,116,856,432};
constexpr float status_top=form_panel.bottom+8;
constexpr Box status_panel{24,status_top,856,status_top+188};
constexpr float buttons_top=status_panel.bottom+10,client_width=880,client_height=buttons_top+54;
constexpr float form_left=form_panel.left+24,form_right=form_panel.right-24;
constexpr float status_left=status_panel.left+16,status_right=status_panel.right-16;
constexpr std::array checkbox_rows{Box{form_left,330,form_right,364},Box{form_left,374,form_right,408}};
using Entry=smbd::PayloadEntry;
struct State {
    std::mutex mutex;std::string phase="Choose your game image and installation folder.",error;
    unsigned percent=0,stage=0;bool running=false,complete=false;
    fs::path installed;
    std::atomic_bool cancel=false;
    std::jthread worker;
} state;
bool uninstalling=false;
unsigned cancellation_percent=101;
fs::path uninstall_root;
std::chrono::steady_clock::time_point started;

[[noreturn]] void system_error(std::string_view operation) {throw std::runtime_error(std::format("{} ({}).",operation,std::strerror(errno)));}
std::string environment(const char* name) {const auto value=std::getenv(name);return value?value:"";}
fs::path home() {
    const auto value=environment("HOME");
    if(value.empty() || value[0]!='/') throw std::runtime_error("Cannot locate your home folder.");
    return value;
}
fs::path xdg(const char* name,std::string_view fallback) {
    const auto value=environment(name);
    return !value.empty() && value[0]=='/'?fs::path(value):home()/fallback;
}
fs::path applications() {return xdg("XDG_DATA_HOME",".local/share")/"applications";}
fs::path desktop_folder() {
    // xdg-user-dirs records the localized desktop as XDG_DESKTOP_DIR="$HOME/...".
    std::ifstream input(xdg("XDG_CONFIG_HOME",".config")/"user-dirs.dirs");std::string line;
    while(std::getline(input,line)) {
        if(!line.starts_with("XDG_DESKTOP_DIR=\"") || line.size()<18 || line.back()!='"') continue;
        const auto value=line.substr(17,line.size()-18);
        if(value=="$HOME") return home();
        if(value.starts_with("$HOME/")) return home()/value.substr(6);
        if(value.starts_with('/')) return value;
    }
    return home()/"Desktop";
}
fs::path expand(std::string text) {
    while(text.size()>1 && text.back()=='/') text.pop_back();
    if(text=="~" || text.starts_with("~/")) text=smbd::path_text(home())+text.substr(1);
    return smbd::utf8_path(text);
}
bool steam_deck() {
    std::ifstream input("/etc/os-release");std::string line;
    while(std::getline(input,line)) if(line=="ID=steamos" || line=="ID=\"steamos\"") return true;
    return false;
}
std::vector<Entry> payload() {
    return smbd::payload_manifest({reinterpret_cast<const char*>(smbd_installer_manifest),std::size_t(smbd_installer_manifest_size)});
}
void report(unsigned stage,unsigned percent,std::string_view phase) {
    {std::lock_guard lock(state.mutex);state.stage=stage;state.percent=percent;state.phase=phase;}
    smbd::diagnostic_record(std::format("{{\"type\":\"install_progress\",\"stage\":{},\"percent\":{},\"phase\":{}}}",stage,percent,smbd::json(phase)));
    if(stage==3 && percent>=cancellation_percent) state.cancel=true;
}
void cancellation() {if(state.cancel.load()) throw std::runtime_error("Installation cancelled. Your original game image and saved games were kept.");}
fs::path installation_root(const fs::path& chosen) {
    const auto text=smbd::path_text(chosen);
    if(text.find_first_not_of(" \t")==std::string::npos) throw std::runtime_error("Choose an installation folder.");
    if(text.size()>1024 || std::ranges::any_of(text,[](unsigned char c) {return c<0x20 || c==0x7f;}))
        throw std::runtime_error("Choose a shorter installation folder without control characters.");
    // Links above the selected folder are resolved once, such as /home on
    // Fedora Atomic; none may appear inside it.
    const auto root=fs::weakly_canonical(fs::absolute(expand(text)));
    if(root==root.root_path() || root==home()) throw std::runtime_error("Choose a separate folder for the game.");
    return root;
}
bool empty_installation_folder(const fs::path& root) {
    for(const auto& item:fs::directory_iterator(root)) {
        const auto name=item.path().filename().native();
        if(name!=".setup-lock" && !name.starts_with(".installing-")) return false;
    }
    return true;
}
struct SetupLock {
    fs::path path;int handle=-1;
    explicit SetupLock(const fs::path& root):path(root/".setup-lock") {
        // A finishing setup unlinks its lock file, so a lock taken on an
        // unlinked file proves nothing: retry until it is the current file.
        for(unsigned attempt=0;attempt<8;++attempt) {
            handle=open(path.c_str(),O_RDWR|O_CREAT|O_CLOEXEC|O_NOFOLLOW,0600);
            if(handle<0 || flock(handle,LOCK_EX|LOCK_NB)!=0) break;
            struct stat locked{},current{};
            if(fstat(handle,&locked)==0 && lstat(path.c_str(),&current)==0 && locked.st_dev==current.st_dev && locked.st_ino==current.st_ino) return;
            close(std::exchange(handle,-1));
        }
        if(handle>=0) close(handle);
        throw std::runtime_error("Another setup may be using this folder. Close it and try again.");
    }
    ~SetupLock() {unlink(path.c_str());close(handle);}
    SetupLock(const SetupLock&)=delete;SetupLock& operator=(const SetupLock&)=delete;
};
void remove_stale_staging(const fs::path& root) {
    // Holding the folder lock proves no other setup owns these directories.
    std::vector<fs::path> stale;
    for(const auto& item:fs::directory_iterator(root))
        if(item.path().filename().native().starts_with(".installing-") && item.is_directory() && !item.is_symlink()) stale.push_back(item.path());
    for(const auto& path:stale) fs::remove_all(path);
}
class Decoder {
public:
    Decoder() {
        if(lzma_stream_decoder(&stream_,256ULL<<20,0)!=LZMA_OK) throw std::runtime_error("Cannot open the installer payload.");
        stream_.next_in=smbd_installer_payload;stream_.avail_in=std::size_t(smbd_installer_payload_size);
    }
    ~Decoder() {lzma_end(&stream_);}
    Decoder(const Decoder&)=delete;Decoder& operator=(const Decoder&)=delete;
    void read(std::span<std::byte> output) {
        stream_.next_out=reinterpret_cast<std::uint8_t*>(output.data());stream_.avail_out=output.size();
        while(stream_.avail_out) {
            if(ended_) throw std::runtime_error("Setup payload is incomplete.");
            const auto result=lzma_code(&stream_,LZMA_FINISH);
            if(result==LZMA_STREAM_END) ended_=true;
            else if(result!=LZMA_OK) throw std::runtime_error(std::format("Cannot unpack the application (xz error {}).",int(result)));
        }
    }
    void finish() {
        std::uint8_t extra=0;stream_.next_out=&extra;stream_.avail_out=1;
        if(!ended_ && lzma_code(&stream_,LZMA_FINISH)!=LZMA_STREAM_END) throw std::runtime_error("Setup payload contains unexpected data.");
        if(stream_.avail_out!=1 || stream_.avail_in) throw std::runtime_error("Setup payload contains unexpected data.");
    }
private:
    lzma_stream stream_=LZMA_STREAM_INIT;bool ended_=false;
};
bool executable(const fs::path& path) {return std::ranges::find(executables,smbd::path_text(path))!=executables.end();}
void write_all(int handle,std::span<const std::byte> data) {
    while(!data.empty()) {
        const auto written=write(handle,data.data(),data.size());
        if(written<0 && errno==EINTR) continue;
        if(written<=0) system_error("Cannot write an application file");
        data=data.subspan(std::size_t(written));
    }
}
void extract_payload(const fs::path& directory,const std::vector<Entry>& entries) {
    std::uint64_t total=0,completed=0;for(const auto& entry:entries) total+=entry.size;
    total=std::max<std::uint64_t>(total,1);
    Decoder decoder;std::vector<std::byte> buffer(1<<20);unsigned previous=101;
    for(const auto& entry:entries) {
        cancellation();
        const auto target=directory/entry.path;fs::create_directories(target.parent_path());
        int handle=open(target.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,executable(entry.path)?0755:0644);
        if(handle<0) system_error("Cannot create an application file");
        struct Closer {int& handle;~Closer() {if(handle>=0) close(handle);}} closer{handle};
        const auto phase="Installing "+smbd::path_text(entry.path);
        for(std::uint64_t remaining=entry.size;remaining;) {
            cancellation();
            const auto chunk=std::span(buffer).first(std::size_t(std::min<std::uint64_t>(remaining,buffer.size())));
            decoder.read(chunk);write_all(handle,chunk);remaining-=chunk.size();completed+=chunk.size();
            const auto percent=unsigned(completed*100/total);
            if(percent!=previous) {report(2,percent,phase);previous=percent;}
        }
        const auto status=close(std::exchange(handle,-1));
        if(status!=0) system_error("Cannot finish writing an application file");
    }
    decoder.finish();
    for(const auto& entry:entries) {
        cancellation();smbd::File file(directory/entry.path);
        if(file.size()!=entry.size || smbd::hash(file,0,file.size())!=entry.digest) throw std::runtime_error("Installed file failed verification: "+smbd::path_text(entry.path));
    }
}
struct Pipe {
    int handles[2]{-1,-1};
    Pipe() {if(pipe2(handles,O_CLOEXEC)!=0) system_error("Cannot check system libraries");}
    ~Pipe() {for(const auto handle:handles) if(handle>=0) close(handle);}
};
void check_runtime(const fs::path& directory) {
    // Load the diagnostics tool with every symbol bound immediately. A missing
    // library or symbol version fails here, before any installed file changes.
    Pipe output;posix_spawn_file_actions_t actions;posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions,0,"/dev/null",O_RDONLY,0);
    posix_spawn_file_actions_addopen(&actions,1,"/dev/null",O_WRONLY,0);
    posix_spawn_file_actions_adddup2(&actions,output.handles[1],2);
    std::vector<std::string> variables{"LD_BIND_NOW=1"};
    for(auto** item=environ;*item;++item) if(!std::string_view(*item).starts_with("LD_")) variables.emplace_back(*item);
    std::vector<char*> variable_pointers;for(auto& variable:variables) variable_pointers.push_back(variable.data());variable_pointers.push_back(nullptr);
    auto program=smbd::path_text(directory/"smbd-tool");std::string option="--help";
    char* arguments[]={program.data(),option.data(),nullptr};
    pid_t child=0;const auto spawned=posix_spawn(&child,program.c_str(),&actions,nullptr,arguments,variable_pointers.data());
    posix_spawn_file_actions_destroy(&actions);close(std::exchange(output.handles[1],-1));
    if(spawned==EACCES) throw std::runtime_error("This drive does not allow running programs. Choose an installation folder on another drive.");
    if(spawned!=0) {errno=spawned;system_error("Cannot check system libraries");}
    std::string text;std::array<char,1024> chunk;
    for(;;) {
        const auto count=read(output.handles[0],chunk.data(),chunk.size());
        if(count<0 && errno==EINTR) continue;
        if(count<=0) break;
        if(text.size()<4096) text.append(chunk.data(),std::size_t(count));
    }
    int status=0;while(waitpid(child,&status,0)<0) if(errno!=EINTR) system_error("Cannot check system libraries");
    if(WIFEXITED(status) && WEXITSTATUS(status)==0) return;
    auto line=text.substr(0,text.find('\n'));
    if(line.empty()) line=WIFSIGNALED(status)?std::format("signal {}",WTERMSIG(status)):std::format("exit code {}",WEXITSTATUS(status));
    throw std::runtime_error("This Linux system cannot run the game: "+line+". See the README for supported distributions.");
}
bool game_running(const fs::path& root) {
    std::error_code error;fs::directory_iterator process("/proc",error),end;
    for(;!error && process!=end;process.increment(error)) {
        const auto name=process->path().filename().native();
        if(name.empty() || !std::ranges::all_of(name,[](char c) {return c>='0' && c<='9';})) continue;
        std::array<char,4096> target{};const auto size=readlink((process->path()/"exe").c_str(),target.data(),target.size()-1);
        if(size<=0) continue;
        std::string_view value(target.data(),std::size_t(size));
        if(value.ends_with(" (deleted)")) value.remove_suffix(10);
        const fs::path program(value);
        if(program.parent_path()==root && (program.filename()=="smbd" || program.filename()=="smbd-tool")) return true;
    }
    return false;
}
// Desktop entry strings unescape backslashes before the Exec field is parsed.
std::string desktop_string(std::string_view value) {
    std::string result;for(const char c:value) {if(c=='\\') result+='\\';result+=c;}
    return result;
}
std::string exec_argument(const fs::path& path) {
    std::string result="\"";
    for(const char c:smbd::path_text(path)) {if(c=='"' || c=='`' || c=='$' || c=='\\') result+='\\';if(c=='%') result+='%';result+=c;}
    return desktop_string(result+'"');
}
std::string desktop_entry(const fs::path& root) {
    const auto location=desktop_string(smbd::path_text(root));
    return std::format("[Desktop Entry]\nType=Application\nName={0}\nComment=Play {0}\nExec={1}\nPath={2}\nIcon={3}\nTerminal=false\n"
        "Categories=Game;\nStartupNotify=true\nActions=Uninstall;\nX-SMBD-InstallLocation={2}\n\n"
        "[Desktop Action Uninstall]\nName=Uninstall\nExec={4} --uninstall {5}\n",
        title,exec_argument(root/"smbd"),location,desktop_string(smbd::path_text(root/"resources/smbd.png")),
        exec_argument(root/uninstaller),exec_argument(root));
}
std::optional<fs::path> entry_location(const fs::path& path) {
    std::error_code error;
    if(!fs::is_regular_file(path,error) || fs::file_size(path,error)>16384) return {};
    std::ifstream input(path);std::string line;
    while(std::getline(input,line)) if(line.starts_with("X-SMBD-InstallLocation=")) {
        std::string value;
        for(std::size_t i=23;i<line.size();++i) {
            if(line[i]=='\\' && i+1<line.size()) {++i;value+=line[i]=='s'?' ':line[i];}
            else value+=line[i];
        }
        return smbd::utf8_path(value);
    }
    return {};
}
void write_entry(const fs::path& path,std::string_view text,mode_t mode) {
    fs::create_directories(path.parent_path());
    const auto temporary=path.parent_path()/std::format(".{}.{}",path.filename().native(),getpid());
    const int handle=open(temporary.c_str(),O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC|O_NOFOLLOW,mode);
    if(handle<0) system_error("Cannot create the game's menu entry");
    try {write_all(handle,std::as_bytes(std::span(text)));} catch(...) {close(handle);unlink(temporary.c_str());throw;}
    int error=fchmod(handle,mode)==0?0:errno;
    if(close(handle)!=0 && !error) error=errno;
    if(!error && rename(temporary.c_str(),path.c_str())!=0) error=errno;
    if(error) {unlink(temporary.c_str());errno=error;system_error("Cannot save the game's menu entry");}
}
void remove_entry(const fs::path& path,const fs::path& root) {
    if(entry_location(path)==root) {std::error_code error;fs::remove(path,error);}
}
void trust_shortcut(const fs::path& path) {
    // GNOME launches desktop-folder entries only once they are marked trusted.
    std::string program="gio",command="set",file=smbd::path_text(path),key="metadata::trusted",value="true";
    char* arguments[]={program.data(),command.data(),file.data(),key.data(),value.data(),nullptr};
    posix_spawn_file_actions_t actions;posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions,1,"/dev/null",O_WRONLY,0);posix_spawn_file_actions_addopen(&actions,2,"/dev/null",O_WRONLY,0);
    pid_t child=0;
    if(posix_spawnp(&child,"gio",&actions,nullptr,arguments,environ)==0) {int status=0;while(waitpid(child,&status,0)<0 && errno==EINTR) {}}
    posix_spawn_file_actions_destroy(&actions);
}
// desktop: create or remove the desktop shortcut; empty keeps the current choice.
void register_install(const fs::path& root,std::optional<bool> desktop) {
    const auto entry=desktop_entry(root);
    write_entry(applications()/entry_name,entry,0644);
    const auto shortcut=desktop_folder()/entry_name;
    if(!desktop) desktop=entry_location(shortcut)==root;
    if(*desktop) {write_entry(shortcut,entry,0755);trust_shortcut(shortcut);}
    else remove_entry(shortcut,root);
}
std::set<fs::path> installed_files(const fs::path& root,bool removing=false) {
    const auto path=root/"install-manifest.txt";
    if(fs::file_size(path)>256*1024) throw std::runtime_error("Invalid installation manifest.");
    std::ifstream input(path);std::string line;std::getline(input,line);
    if(line!=marker) throw std::runtime_error(removing?"This folder is not a Super Monkey Ball Deluxe installation.":
        "This folder belongs to another program. Choose an empty installation folder.");
    std::set<fs::path> files;
    while(std::getline(input,line)) files.insert(smbd::install_relative_path(line,true));
    return files;
}
bool linked(const fs::path& path) {return fs::weakly_canonical(path)!=path;}
void write_manifest(const fs::path& path,const std::set<fs::path>& files) {
    std::string text(marker);text+='\n';for(const auto& file:files) text+=smbd::path_text(file)+'\n';
    smbd::write_text(path,text,false);
}
void install(const fs::path& source,const fs::path& chosen,bool copy_disc,std::optional<bool> desktop,bool registration) {
    const auto iso=fs::absolute(expand(smbd::path_text(source)));
    const auto root=installation_root(chosen);
    if(fs::exists(root) && !fs::is_directory(root)) throw std::runtime_error("Choose a folder for the game, not a file.");
    if(fs::exists(root) && !empty_installation_folder(root) && !fs::exists(root/"install-manifest.txt"))
        throw std::runtime_error("Choose an empty folder, or the folder containing an existing installation.");
    const auto previous_files=fs::exists(root/"install-manifest.txt")?installed_files(root):std::set<fs::path>{};
    const auto entries=payload();
    for(const auto& entry:entries) {
        if(linked((root/entry.path).parent_path())) throw std::runtime_error("An application folder crosses a symbolic link.");
        if(fs::exists(root/entry.path) && !previous_files.contains(entry.path))
            throw std::runtime_error("The destination contains an unrelated application file. Choose an empty folder.");
    }
    for(const auto& file:previous_files) if(linked((root/file).parent_path())) throw std::runtime_error("An application folder crosses a symbolic link.");
    if(linked(root/"data/local")) throw std::runtime_error("The game-data folder crosses a symbolic link.");
    report(1,0,"Checking your game image");smbd::verify_install_disc(iso);cancellation();report(1,100,"Game image verified");
    fs::create_directories(root);
    SetupLock lock(root);
    remove_stale_staging(root);
    const auto staging=root/std::format(".installing-{}",getpid());
    if(!fs::create_directory(staging)) throw std::runtime_error("An installation is already using this folder.");
    struct Staging {
        fs::path path,root;
        ~Staging() {
            // Recursive cleanup is restricted to this newly created child of the installation folder.
            std::error_code error;
            if(path.parent_path()==root && path.filename().native().starts_with(".installing-") && fs::weakly_canonical(path,error)==path)
                fs::remove_all(path,error);
        }
    } cleanup{staging,root};
    report(2,0,"Installing application files");extract_payload(staging,entries);
    report(2,100,"Checking system libraries");check_runtime(staging);cancellation();
    const auto preferences=root/"data/local/settings.cfg";
    fs::create_directories(staging/"data/local");
    if(fs::exists(preferences)) fs::copy_file(preferences,staging/"data/local/settings.cfg");
    else if(steam_deck()) smbd::write_text(staging/"data/local/settings.cfg","fullscreen=1\nrefresh_rate=0\nresolution=1\naspect=1\n",false);
    report(3,0,"Preparing your game files");
    bool reuse_disc=false;
    if(copy_disc && fs::exists(root/"data/local/game.iso") && fs::equivalent(iso,root/"data/local/game.iso"))
        reuse_disc=link((root/"data/local/game.iso").c_str(),(staging/"data/local/game.iso").c_str())==0;
    smbd::install_game_files(iso,staging,copy_disc,[](unsigned percent,std::string_view phase) {report(3,percent,phase);},&state.cancel);
    // /proc/self/exe still reads this program if its download was replaced meanwhile.
    fs::copy_file("/proc/self/exe",staging/uninstaller);
    fs::permissions(staging/uninstaller,fs::perms::owner_all|fs::perms::group_read|fs::perms::group_exec|fs::perms::others_read|fs::perms::others_exec);
    std::set<fs::path> recorded;for(const auto& entry:entries) recorded.insert(entry.path);
    recorded.insert(uninstaller);recorded.insert("data/local/default.xbe");
    if(copy_disc || previous_files.contains("data/local/game.iso")) recorded.insert("data/local/game.iso");
    write_manifest(staging/"install-manifest.txt",recorded);
    // Until every file is in place the folder's manifest also lists the previous
    // files, so an interrupted update is repaired by running Setup again.
    auto pending=recorded;pending.insert(previous_files.begin(),previous_files.end());
    write_manifest(staging/"install-manifest.pending",pending);
    std::vector<fs::path> files(recorded.begin(),recorded.end());
    files.push_back("data/local/settings.cfg");cancellation();
    if(game_running(root)) throw std::runtime_error("Close the game and its diagnostics, then try installing again.");
    report(4,0,"Finishing installation");
    const auto move=[](const fs::path& from,const fs::path& to) {
        if(rename(from.c_str(),to.c_str())!=0) system_error("Cannot finish installing application files");
    };
    move(staging/"install-manifest.pending",root/"install-manifest.txt");
    unsigned committed=0;
    for(const auto& file:files) {
        if(reuse_disc && file=="data/local/game.iso") continue;
        if(!fs::exists(staging/file)) continue;
        fs::create_directories((root/file).parent_path());
        move(staging/file,root/file);
        report(4,unsigned(++committed*100/files.size()),"Finishing installation");
    }
    smbd::finish_install_manifest(root,staging,previous_files,recorded);
    if(registration) register_install(root,desktop);
    {std::lock_guard guard(state.mutex);state.installed=root;}
    report(4,100,"Ready to play. Your installation is complete.");
}
void uninstall(const fs::path& chosen) {
    const auto root=installation_root(chosen);
    if(!fs::exists(root/"install-manifest.txt")) throw std::runtime_error("This folder is not an installed copy of the game.");
    std::vector<fs::path> files;
    {
        SetupLock lock(root);
        const auto entries=payload();
        for(const auto& file:installed_files(root,true)) {
            if(std::ranges::find(entries,file,&Entry::path)==entries.end() && file!=uninstaller && file!="data/local/default.xbe" && file!="data/local/game.iso")
                throw std::runtime_error("Uninstall manifest contains an unknown file. No files were removed.");
            if(linked((root/file).parent_path())) throw std::runtime_error("An uninstall path crosses a symbolic link.");
            files.push_back(file);
        }
        if(game_running(root)) throw std::runtime_error("Close the game and its diagnostics before uninstalling. No files were removed.");
        unsigned count=0;
        for(const auto& file:files) {
            report(1,unsigned(count++*100/files.size()),"Removing "+smbd::path_text(file));
            std::error_code error;fs::remove(root/file,error);
            if(error) throw std::runtime_error("Could not remove "+smbd::path_text(file)+": "+error.message());
        }
        fs::remove(root/"install-manifest.txt");
        remove_entry(applications()/entry_name,root);remove_entry(desktop_folder()/entry_name,root);
    }
    // Remove now-empty application folders, deepest first; saves keep data/ in place.
    std::set<fs::path,std::greater<>> folders;
    for(const auto& file:files) for(auto parent=file.parent_path();!parent.empty();parent=parent.parent_path()) folders.insert(parent);
    for(const auto& folder:folders) {std::error_code error;fs::remove(root/folder,error);}
    std::error_code error;fs::remove(root,error);
    report(1,100,"Uninstalled. Your saved games and preferences were kept.");
}
struct Defaults {fs::path folder,iso;bool copy=true,desktop=false;};
Defaults defaults() {
    Defaults result{xdg("XDG_DATA_HOME",".local/share")/title,{},true,false};
    if(const auto previous=entry_location(applications()/entry_name);previous && previous->is_absolute() && fs::exists(*previous/"install-manifest.txt"))
        result.folder=*previous;
    const auto manifest=result.folder/"install-manifest.txt";
    std::error_code error;
    if(fs::exists(manifest) && fs::file_size(manifest,error)<256*1024) {
        std::ifstream input(manifest);std::string line;std::getline(input,line);
        if(line==marker) {
            result.copy=false;
            while(std::getline(input,line)) if(line=="data/local/game.iso") result.copy=true;
            result.desktop=entry_location(desktop_folder()/entry_name)==result.folder;
        }
    }
    if(fs::exists(result.folder/"data/local/game.iso") && result.copy) result.iso=result.folder/"data/local/game.iso";
    else {
        const auto settings=result.folder/"data/local/settings.cfg";
        if(fs::exists(settings) && fs::file_size(settings,error)<16384) {
            std::ifstream input(settings);std::string line;
            while(std::getline(input,line)) if(line.starts_with("disc=")) result.iso=result.folder/smbd::utf8_path(line.substr(5));
        }
    }
    if(!result.iso.empty() && !fs::exists(result.iso)) result.iso.clear();
    return result;
}

// Window: SDL3 with its software renderer and Dear ImGui, so setup needs no GPU driver.
struct Dialog {std::mutex mutex;bool done=false,folder=false;std::string path,error;};
struct Window {
    SDL_Window* window=nullptr;SDL_Renderer* renderer=nullptr;SDL_Texture* wood=nullptr;SDL_Texture* logo=nullptr;
    std::array<char,4096> iso{},folder{};bool copy=true,desktop=false,closing=false,focus=true,install_enabled=false;
    float scale=1;std::string font="default",browse="Browse...",dot="-";
    ImGuiStyle base;std::shared_ptr<Dialog> dialog;std::vector<pid_t> games;
} ui;
ImVec2 at(float x,float y) {return {x*ui.scale,y*ui.scale};}
ImTextureID texture(SDL_Texture* value) {return static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(value));}
void text(std::string_view value,Box box,ImU32 color,float size,bool right=false) {
    auto* font=ImGui::GetFont();const auto pixels=size*ui.scale;
    auto position=at(box.left,box.top);
    if(right) position.x=box.right*ui.scale-font->CalcTextSizeA(pixels,FLT_MAX,0,value.data(),value.data()+value.size()).x;
    ImGui::GetWindowDrawList()->AddText(font,pixels,position,color,value.data(),value.data()+value.size(),right?0:(box.right-box.left)*ui.scale);
}
void apply_scale(float scale) {
    ui.scale=std::clamp(scale,1.0f,4.0f);
    auto& style=ImGui::GetStyle();style=ui.base;style.ScaleAllSizes(ui.scale);style.FontScaleDpi=ui.scale;
}
void load_font() {
    auto& io=ImGui::GetIO();
    constexpr std::array candidates{
        "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf","/usr/share/fonts/noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/google-noto/NotoSans-Regular.ttf","/usr/share/fonts/google-noto-vf/NotoSans[wght].ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf","/usr/share/fonts/TTF/DejaVuSans.ttf",
        "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf","/usr/share/fonts/dejavu/DejaVuSans.ttf"};
    for(const auto* candidate:candidates) {
        std::error_code error;
        if(fs::is_regular_file(candidate,error) && io.Fonts->AddFontFromFileTTF(candidate,18)) {
            ui.font=candidate;ui.browse="Browse…";ui.dot="·";return;
        }
    }
    io.Fonts->AddFontDefaultVector();
}
std::string current(const std::array<char,4096>& value) {return {value.data(),strnlen(value.data(),value.size())};}
void assign(std::array<char,4096>& output,std::string_view value) {
    const auto count=std::min(value.size(),output.size()-1);std::memcpy(output.data(),value.data(),count);output[count]=0;
}
bool paths_ready() {
    const auto blank=[](const std::string& value) {return value.find_first_not_of(" \t\r\n")==std::string::npos;};
    return uninstalling || (!blank(current(ui.iso)) && !blank(current(ui.folder)));
}
void choose(bool folder) {
    if(ui.dialog) return;
    // Start in the nearest existing folder above the current choice.
    std::error_code error;
    auto location=expand(current(folder?ui.folder:ui.iso));
    if(!folder && fs::is_regular_file(location,error)) location=location.parent_path();
    while(!location.empty() && location!=location.root_path() && !fs::is_directory(location,error)) location=location.parent_path();
    if(location.empty() || !location.is_absolute()) location=home();
    const auto start=smbd::path_text(location);
    auto pending=std::make_unique<std::shared_ptr<Dialog>>(std::make_shared<Dialog>());
    (*pending)->folder=folder;ui.dialog=*pending;
    const auto callback=[](void* data,const char* const* files,int) {
        const std::unique_ptr<std::shared_ptr<Dialog>> owner(static_cast<std::shared_ptr<Dialog>*>(data));
        auto& result=**owner;std::lock_guard lock(result.mutex);
        try {if(!files) result.error=SDL_GetError();else if(*files) result.path=*files;} catch(...) {}
        result.done=true;
    };
    static constexpr SDL_DialogFileFilter filters[]={{"Xbox disc images (*.iso)","iso"},{"All files","*"}};
    if(folder) SDL_ShowOpenFolderDialog(callback,pending.release(),ui.window,start.c_str(),false);
    else SDL_ShowOpenFileDialog(callback,pending.release(),ui.window,filters,2,start.c_str(),false);
}
void poll_dialog() {
    if(!ui.dialog) return;
    std::lock_guard lock(ui.dialog->mutex);
    if(!ui.dialog->done) return;
    if(!ui.dialog->path.empty()) assign(ui.dialog->folder?ui.folder:ui.iso,ui.dialog->path);
    else if(!ui.dialog->error.empty()) {
        std::lock_guard guard(state.mutex);
        state.error="The file picker is unavailable ("+ui.dialog->error+"). Type the path instead.";
    }
    ui.dialog.reset();
}
void start() {
    if(state.worker.joinable()) state.worker.join();
    const auto iso=smbd::utf8_path(current(ui.iso));
    const auto destination=uninstalling?uninstall_root:smbd::utf8_path(current(ui.folder));
    const bool copy=ui.copy,desktop=ui.desktop;
    if(!paths_ready()) throw std::runtime_error("Choose your game image and installation folder first.");
    {std::lock_guard lock(state.mutex);state.running=true;state.complete=false;state.error.clear();state.stage=0;state.percent=0;}
    state.cancel=false;started=std::chrono::steady_clock::now();
    state.worker=std::jthread([iso,destination,copy,desktop] {
        try {
            if(uninstalling) uninstall(destination);else install(iso,destination,copy,desktop,true);
            std::lock_guard lock(state.mutex);state.complete=true;
        } catch(const std::exception& error) {
            smbd::diagnostic_record("{\"type\":\"install_error\",\"message\":"+smbd::json(error.what())+'}');
            std::lock_guard lock(state.mutex);state.error=error.what();state.phase="Setup did not finish. You can correct the problem and try again.";
        }
        std::lock_guard lock(state.mutex);state.running=false;
    });
}
void launch_game() {
    fs::path root;{std::lock_guard lock(state.mutex);root=state.installed;}
    auto program=smbd::path_text(root/"smbd");char* arguments[]={program.data(),nullptr};
    posix_spawnattr_t attributes;posix_spawnattr_init(&attributes);posix_spawnattr_setflags(&attributes,POSIX_SPAWN_SETSID);
    pid_t child=0;const auto result=posix_spawn(&child,program.c_str(),nullptr,&attributes,arguments,environ);
    posix_spawnattr_destroy(&attributes);
    if(result!=0) {errno=result;system_error("Cannot launch the game");}
    ui.games.push_back(child);
}
void request_close(bool& quit) {
    bool running;{std::lock_guard lock(state.mutex);running=state.running;}
    if(running && uninstalling) return;  // Uninstall cannot be cancelled.
    if(running) {state.cancel=true;ui.closing=true;return;}
    quit=true;
}
bool button(const char* label,float x,float y,float width,float height,bool primary,bool enabled) {
    ImGui::SetCursorScreenPos(at(x,y));
    if(primary) {
        ImGui::PushStyleVar(ImGuiStyleVar_DisabledAlpha,1.0f);
        ImGui::PushStyleColor(ImGuiCol_Button,enabled?yellow:IM_COL32(147,115,37,255));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,IM_COL32(255,216,77,255));ImGui::PushStyleColor(ImGuiCol_ButtonActive,IM_COL32(242,178,10,255));
        ImGui::PushStyleColor(ImGuiCol_Text,IM_COL32(30,30,9,255));
    }
    ImGui::BeginDisabled(!enabled);
    const bool pressed=ImGui::Button(label,at(width,height));
    ImGui::EndDisabled();
    if(primary) {ImGui::PopStyleColor(4);ImGui::PopStyleVar();}
    return pressed;
}
void wood(ImVec2 min,ImVec2 max,ImU32 tint,float radius) {
    // SDL's software rasterizer rejects textured triangles above about a
    // megapixel, so stretch the grain across tiles; only corner tiles round.
    auto* draw=ImGui::GetWindowDrawList();
    const int columns=std::max(1,int(std::ceil((max.x-min.x)/(128*ui.scale)))),rows=std::max(1,int(std::ceil((max.y-min.y)/(128*ui.scale))));
    for(int row=0;row<rows;++row) for(int column=0;column<columns;++column) {
        const float u0=float(column)/columns,u1=float(column+1)/columns,v0=float(row)/rows,v1=float(row+1)/rows;
        const ImVec2 a{min.x+(max.x-min.x)*u0,min.y+(max.y-min.y)*v0},b{min.x+(max.x-min.x)*u1,min.y+(max.y-min.y)*v1};
        ImDrawFlags corners=0;
        if(row==0 && column==0) corners|=ImDrawFlags_RoundCornersTopLeft;
        if(row==0 && column==columns-1) corners|=ImDrawFlags_RoundCornersTopRight;
        if(row==rows-1 && column==0) corners|=ImDrawFlags_RoundCornersBottomLeft;
        if(row==rows-1 && column==columns-1) corners|=ImDrawFlags_RoundCornersBottomRight;
        if(radius>0 && corners) draw->AddImageRounded(texture(ui.wood),a,b,{u0,v0},{u1,v1},tint,radius,corners);
        else draw->AddImage(texture(ui.wood),a,b,{u0,v0},{u1,v1},tint);
    }
}
void panel(Box box) {
    const float radius=7*ui.scale;
    wood(at(box.left,box.top),at(box.right,box.bottom),IM_COL32(153,139,118,255),radius);
    ImGui::GetWindowDrawList()->AddRect(at(box.left,box.top),at(box.right,box.bottom),rim,radius,0,2*ui.scale);
}
void frame(bool& quit) {
    ImGui_ImplSDLRenderer3_NewFrame();ImGui_ImplSDL3_NewFrame();ImGui::NewFrame();
    std::string phase,error;unsigned percent,stage;bool running,complete;
    {std::lock_guard lock(state.mutex);phase=state.phase;error=state.error;percent=state.percent;stage=state.stage;running=state.running;complete=state.complete;}
    if(!running) ui.closing=false;
    const auto& io=ImGui::GetIO();
    ImGui::SetNextWindowPos({0,0});ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("Setup",nullptr,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoBackground|
        ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoBringToFrontOnFocus|ImGuiWindowFlags_NoScrollWithMouse);
    auto* draw=ImGui::GetWindowDrawList();
    wood({0,0},io.DisplaySize,IM_COL32_WHITE,0);
    panel(form_panel);panel(status_panel);
    if(!uninstalling) for(const auto row:checkbox_rows) draw->AddRectFilled(at(row.left,row.top),at(row.right,row.bottom),brown);
    draw->AddImage(texture(ui.logo),at(28,28),at(84,84));
    text(uninstalling?"Uninstallation":"Installation",{100,28,850,65},gold,26);
    text(std::format("v{} {} Linux x86-64",SMBD_VERSION,ui.dot),{640,34,840,60},gold,15,true);
    text(uninstalling?"Uninstall - your saved games and preferences will be kept.":"Select your copy of Super Monkey Ball Deluxe and get ready to play!",{100,70,850,106},cream,15);
    const bool editable=!running && !complete;
    if(!uninstalling) {
        text("YOUR GAME IMAGE",{form_left,140,form_right,162},gold,15);
        text("Use a US Xbox XISO image. Game files are verified before installation.",{form_left,218,form_right,242},cream,15);
        text("INSTALLATION FOLDER",{form_left,254,form_right,276},gold,15);
        ImGui::BeginDisabled(!editable);
        ImGui::SetCursorScreenPos(at(form_left,172));ImGui::SetNextItemWidth((form_right-116-form_left)*ui.scale);
        if(std::exchange(ui.focus,false)) ImGui::SetKeyboardFocusHere();
        ImGui::InputText("##iso",ui.iso.data(),ui.iso.size());
        if(button((ui.browse+"##iso").c_str(),form_right-104,172,104,34,false,editable && !ui.dialog)) choose(false);
        ImGui::SetCursorScreenPos(at(form_left,286));ImGui::SetNextItemWidth((form_right-116-form_left)*ui.scale);
        ImGui::InputText("##folder",ui.folder.data(),ui.folder.size());
        if(button((ui.browse+"##folder").c_str(),form_right-104,286,104,34,false,editable && !ui.dialog)) choose(true);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,{ImGui::GetStyle().FramePadding.x,4*ui.scale});
        ImGui::SetCursorScreenPos(at(checkbox_rows[0].left+12,checkbox_rows[0].top+4));
        ImGui::Checkbox("Keep a copy of my ISO with the game (uses additional disk space)",&ui.copy);
        ImGui::SetCursorScreenPos(at(checkbox_rows[1].left+12,checkbox_rows[1].top+4));
        ImGui::Checkbox("Create a desktop shortcut",&ui.desktop);
        ImGui::PopStyleVar();
        ImGui::EndDisabled();
    } else text("Remove the installed program and its copied game files. Your original ISO, saved games, and preferences are kept.",{40,140,824,215},cream,18);
    const auto caption=running?std::format("STEP {} OF {}  {}  {}%",stage,uninstalling?1:4,ui.dot,percent):complete?std::string("COMPLETE"):std::string("INSTALLATION PROGRESS");
    text(caption,{status_left,status_top+12,status_right,status_top+36},gold,15);
    text(phase,{status_left,status_top+43,status_right,status_top+77},cream,18);
    draw->AddRectFilled(at(status_left,status_top+84),at(status_right,status_top+104),brown);
    if(percent) draw->AddRectFilled(at(status_left,status_top+84),at(status_left+(status_right-status_left)*std::min(percent,100U)/100,status_top+104),yellow);
    draw->AddRect(at(status_left,status_top+84),at(status_right,status_top+104),rim,0,0,ui.scale);
    if(!error.empty()) text(error,{status_left,status_top+112,status_right,status_top+178},warning,15);
    else if(running) {
        const auto seconds=std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now()-started).count();
        text(std::format("Elapsed: {}:{:02}  {}  You can leave this window open while setup works.",seconds/60,seconds%60,ui.dot),
            {status_left,status_top+112,status_right,status_top+152},cream,15);
    } else text(uninstalling?"Only files recorded by this installer are removed.":"Your original game image and saved games are kept.",
        {status_left,status_top+112,status_right,status_top+152},cream,15);
    ui.install_enabled=!running && (complete?!uninstalling:paths_ready());
    const auto primary=complete && !uninstalling?"Play game###primary":uninstalling?"Uninstall###primary":"Install###primary";
    if(uninstalling && std::exchange(ui.focus,false)) ImGui::SetKeyboardFocusHere();
    if(button(primary,620,buttons_top,128,38,true,ui.install_enabled)) {
        try {if(complete && !uninstalling) launch_game();else start();}
        catch(const std::exception& failure) {std::lock_guard lock(state.mutex);state.error=failure.what();}
    }
    const auto close=ui.closing?"Cancelling...###close":running?"Cancel###close":"Close###close";
    if(button(close,760,buttons_top,88,38,false,!running || (!uninstalling && stage<4 && !ui.closing))) request_close(quit);
    ImGui::End();ImGui::Render();
    SDL_SetRenderScale(ui.renderer,io.DisplayFramebufferScale.x,io.DisplayFramebufferScale.y);
    SDL_SetRenderDrawColor(ui.renderer,48,27,10,255);SDL_RenderClear(ui.renderer);
    ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(),ui.renderer);
}
void save_preview(const fs::path& path) {
    SDL_Surface* surface=SDL_RenderReadPixels(ui.renderer,nullptr);
    if(!surface || !SDL_SavePNG(surface,smbd::path_text(path).c_str())) {
        if(surface) SDL_DestroySurface(surface);
        throw std::runtime_error(std::string("Cannot capture the installer preview: ")+SDL_GetError());
    }
    const auto width=surface->w,height=surface->h;SDL_DestroySurface(surface);
    float logo_width=0,logo_height=0;SDL_GetTextureSize(ui.logo,&logo_width,&logo_height);
    smbd::write_text(smbd::path_text(path)+".json",std::format("{{\"format\":\"smbd-installer-preview-v1\",\"platform\":\"linux-x86_64\",\"render_scale\":{},\"size\":[{},{}],"
        "\"icon_size\":[{},{}],\"renderer\":{},\"font\":{},\"install_enabled\":{}}}\n",ui.scale,width,height,logo_width,logo_height,
        smbd::json(SDL_GetRendererName(ui.renderer)),smbd::json(ui.font),ui.install_enabled),true);
}
int wizard(const fs::path& preview={},unsigned preview_dpi=0) {
    if(!preview.empty()) SDL_SetHint(SDL_HINT_VIDEO_DRIVER,"offscreen");
    SDL_SetAppMetadata("Super Monkey Ball Deluxe Setup",SMBD_VERSION,"io.github.timftw21.smbd_recompilation.setup");
    if(!SDL_Init(SDL_INIT_VIDEO|SDL_INIT_GAMEPAD)) throw std::runtime_error(std::string("Cannot open the installer window: ")+SDL_GetError());
    struct Sdl {~Sdl() {SDL_Quit();}} sdl;
    ui.window=SDL_CreateWindow(uninstalling?"Super Monkey Ball Deluxe — Uninstall":"Super Monkey Ball Deluxe — Setup",
        int(client_width),int(client_height),SDL_WINDOW_HIDDEN|SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if(!ui.window) throw std::runtime_error(std::string("Cannot open the installer window: ")+SDL_GetError());
    const auto density=SDL_GetWindowPixelDensity(ui.window);
    const float scale=preview_dpi?preview_dpi/96.0f:density>0?SDL_GetWindowDisplayScale(ui.window)/density:1.0f;
    ui.renderer=SDL_CreateRenderer(ui.window,"software");
    if(!ui.renderer) throw std::runtime_error(std::string("Cannot draw the installer window: ")+SDL_GetError());
    SDL_Surface* icon=SDL_LoadPNG_IO(SDL_IOFromConstMem(smbd_installer_icon,std::size_t(smbd_installer_icon_size)),true);
    if(!icon) throw std::runtime_error(std::string("Cannot load the installer icon: ")+SDL_GetError());
    SDL_SetWindowIcon(ui.window,icon);ui.logo=SDL_CreateTextureFromSurface(ui.renderer,icon);SDL_DestroySurface(icon);
    std::vector<std::uint8_t> grain(512*256*4);
    for(unsigned y=0;y<256;++y) for(unsigned x=0;x<512;++x) std::ranges::copy(smbd::theme::wood_pixel(x,y),grain.begin()+(y*512+x)*4);
    ui.wood=SDL_CreateTexture(ui.renderer,SDL_PIXELFORMAT_RGBA32,SDL_TEXTUREACCESS_STATIC,512,256);
    if(!ui.logo || !ui.wood || !SDL_UpdateTexture(ui.wood,nullptr,grain.data(),512*4)) throw std::runtime_error(std::string("Cannot prepare the installer artwork: ")+SDL_GetError());
    SDL_SetTextureScaleMode(ui.logo,SDL_SCALEMODE_LINEAR);SDL_SetTextureScaleMode(ui.wood,SDL_SCALEMODE_LINEAR);
    IMGUI_CHECKVERSION();ImGui::CreateContext();
    struct Context {~Context() {ImGui::DestroyContext();}} context;
    auto& io=ImGui::GetIO();io.ConfigFlags|=ImGuiConfigFlags_NavEnableKeyboard|ImGuiConfigFlags_NavEnableGamepad;io.IniFilename=nullptr;io.LogFilename=nullptr;
    load_font();
    auto& style=ImGui::GetStyle();
    style.FontSizeBase=18;style.WindowPadding={0,0};style.WindowBorderSize=0;style.FramePadding={10,8};style.FrameRounding=4;style.FrameBorderSize=1;
    style.ItemInnerSpacing={10,6};
    auto* colors=style.Colors;
    colors[ImGuiCol_Text]=ImGui::ColorConvertU32ToFloat4(cream);colors[ImGuiCol_TextDisabled]={.76f,.65f,.46f,1};
    colors[ImGuiCol_FrameBg]=ImGui::ColorConvertU32ToFloat4(brown);colors[ImGuiCol_FrameBgHovered]={.25f,.15f,.06f,1};colors[ImGuiCol_FrameBgActive]={.30f,.18f,.07f,1};
    colors[ImGuiCol_Border]=ImGui::ColorConvertU32ToFloat4(rim);colors[ImGuiCol_CheckMark]=ImGui::ColorConvertU32ToFloat4(yellow);
    colors[ImGuiCol_Button]={.40f,.24f,.08f,1};colors[ImGuiCol_ButtonHovered]={.50f,.31f,.11f,1};colors[ImGuiCol_ButtonActive]={.33f,.20f,.06f,1};
    colors[ImGuiCol_NavCursor]=ImGui::ColorConvertU32ToFloat4(yellow);colors[ImGuiCol_TextSelectedBg]={.72f,.51f,.20f,.6f};
    ui.base=style;apply_scale(scale);
    if(ui.scale!=1) SDL_SetWindowSize(ui.window,int(client_width*ui.scale+.5f),int(client_height*ui.scale+.5f));
    if(!ImGui_ImplSDL3_InitForSDLRenderer(ui.window,ui.renderer) || !ImGui_ImplSDLRenderer3_Init(ui.renderer)) throw std::runtime_error("Cannot initialize the installer window.");
    struct Backends {~Backends() {ImGui_ImplSDLRenderer3_Shutdown();ImGui_ImplSDL3_Shutdown();}} backends;
    const auto initial=uninstalling?Defaults{uninstall_root,{},true,false}:defaults();
    assign(ui.iso,smbd::path_text(initial.iso));assign(ui.folder,smbd::path_text(initial.folder));ui.copy=initial.copy;ui.desktop=initial.desktop;
    bool quit=false;
    if(!preview.empty()) {
        ui.focus=false;
        for(unsigned i=0;i<3;++i) frame(quit);
        save_preview(preview);return 0;
    }
    SDL_ShowWindow(ui.window);
    for(unsigned busy=4;!quit;) {
        bool running;{std::lock_guard lock(state.mutex);running=state.running;}
        SDL_Event event;
        // Draw a few frames after input so ImGui settles; otherwise wait for events or progress.
        if(SDL_WaitEventTimeout(&event,busy?0:running?100:250)) {
            do {
                ImGui_ImplSDL3_ProcessEvent(&event);
                if(event.type==SDL_EVENT_QUIT || event.type==SDL_EVENT_WINDOW_CLOSE_REQUESTED) request_close(quit);
                else if(event.type==SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED) {
                    const auto pixels=SDL_GetWindowPixelDensity(ui.window);
                    if(pixels>0) apply_scale(SDL_GetWindowDisplayScale(ui.window)/pixels);
                    SDL_SetWindowSize(ui.window,int(client_width*ui.scale+.5f),int(client_height*ui.scale+.5f));
                } else if(event.type==SDL_EVENT_DROP_FILE && event.drop.data && !running) {
                    std::error_code error;const bool directory=fs::is_directory(event.drop.data,error);
                    if(!uninstalling) assign(directory?ui.folder:ui.iso,event.drop.data);
                }
            } while(SDL_PollEvent(&event));
            busy=4;
        } else if(busy) --busy;
        poll_dialog();
        std::erase_if(ui.games,[](pid_t child) {int status=0;return waitpid(child,&status,WNOHANG)!=0;});
        frame(quit);SDL_RenderPresent(ui.renderer);
    }
    state.cancel=true;if(state.worker.joinable()) state.worker.join();
    return 0;
}
unsigned number(std::string_view value,unsigned low,unsigned high,std::string_view name) {
    unsigned result=0;const auto parsed=std::from_chars(value.data(),value.data()+value.size(),result);
    if(parsed.ec!=std::errc{} || parsed.ptr!=value.data()+value.size() || result<low || result>high)
        throw std::runtime_error(std::format("{} must be {}..{}",name,low,high));
    return result;
}
}
int main(int argc,char** argv) {
    // Setup logs go to the user's state folder, not beside the downloaded program.
    try {smbd::initialize_diagnostics(xdg("XDG_STATE_HOME",".local/state")/"smbd-recompilation/setup-logs");} catch(...) {}
    const auto option=[&](int index) {return index<argc?std::string_view(argv[index]):std::string_view();};
    try {
        if((argc==3 || (argc==5 && option(3)=="--dpi")) && (option(1)=="--preview" || option(1)=="--preview-progress")) {
            const unsigned dpi=argc==5?number(option(4),96,384,"Preview DPI"):96;
            if(option(1)=="--preview-progress") {state.running=true;state.stage=3;state.percent=47;state.phase="Copying your game image";started=std::chrono::steady_clock::now();}
            return wizard(smbd::utf8_path(option(2)),dpi);
        }
        if(argc>=4 && option(1)=="--silent-install") {
            bool copy=true,registration=true;
            for(int i=4;i<argc;++i) {
                if(option(i)=="--reference-disc") copy=false;
                else if(option(i)=="--no-register") registration=false;
                else if(option(i)=="--cancel-after-percent" && i+1<argc) cancellation_percent=number(option(++i),0,100,"Cancellation percentage");
                else throw std::runtime_error("Unknown installer option");
            }
            install(smbd::utf8_path(option(2)),smbd::utf8_path(option(3)),copy,std::nullopt,registration);return 0;
        }
        if(argc>=3 && option(1)=="--uninstall") {
            uninstall_root=installation_root(smbd::utf8_path(option(2)));
            if(argc==4 && option(3)=="--silent") {uninstall(uninstall_root);return 0;}
            if(argc!=3) throw std::runtime_error("Unknown installer arguments");
            uninstalling=true;
        } else if(argc==1) {
            // Opening the installed smbd-uninstall directly starts uninstall, not a second installation.
            std::error_code error;const auto self=fs::read_symlink("/proc/self/exe",error);
            if(!error && self.filename()==uninstaller && fs::exists(self.parent_path()/"install-manifest.txt")) {
                uninstall_root=installation_root(self.parent_path());uninstalling=true;
            }
        } else throw std::runtime_error("Unknown installer arguments. Use --silent-install ISO FOLDER, --uninstall FOLDER [--silent] or --preview IMAGE.png [--dpi N].");
        return wizard();
    } catch(const std::exception& error) {
        smbd::diagnostic_record("{\"type\":\"install_error\",\"message\":"+smbd::json(error.what())+'}');
        std::cerr<<error.what()<<'\n';
        if(argc==1 || (argc==3 && option(1)=="--uninstall")) SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR,title,error.what(),nullptr);
        return 1;
    }
}
