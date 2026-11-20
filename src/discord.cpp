#include "discord.h"
#include "disc.h"
#include "file.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <ctime>
#include <format>
#include <mutex>
#include <span>
#include <stdexcept>
#include <thread>
#ifdef _WIN32
#include <Windows.h>
#else
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace smbd {
namespace {
using Clock=std::chrono::steady_clock;
constexpr auto retry_delay=std::chrono::seconds(5),update_delay=std::chrono::seconds(15);
constexpr unsigned max_payload=64*1024;
constexpr std::string_view presence_icon="https://raw.githubusercontent.com/timftw21/smbd-recompilation/60c031012e703ae6b900b3744f50ed0b3cd2cbfa/resources/smbd.png";
enum class Status {disabled,unconfigured,waiting,connecting,connected,rejected,failed};

// Only top-level RPC string fields are needed; nested/user data is ignored.
std::string_view field(std::string_view text,std::string_view name) {
    unsigned depth=0;
    for(std::size_t i=0;i<text.size();) {
        const auto c=text[i++];
        if(c=='{' || c=='[') ++depth;
        else if(c=='}' || c==']') {if(!depth) return {};--depth;}
        else if(c=='"') {
            const auto first=i;bool escaped=false;
            while(i<text.size() && text[i]!='"') {
                if(text[i++]=='\\') {escaped=true;if(i==text.size()) return {};++i;}
            }
            if(i==text.size()) return {};
            const auto key=text.substr(first,i++-first);
            if(depth!=1 || escaped || key!=name) continue;
            const auto space=[&]{while(i<text.size() && (text[i]==' ' || text[i]=='\r' || text[i]=='\n' || text[i]=='\t')) ++i;};
            space();if(i==text.size() || text[i++]!=':') continue;
            space();if(i==text.size() || text[i++]!='"') return {};
            const auto value=i;
            while(i<text.size() && text[i]!='"') {if(text[i++]=='\\') return {};}
            return i<text.size()?text.substr(value,i-value):std::string_view{};
        }
    }
    return {};
}
struct Connection {
#ifdef _WIN32
    HANDLE pipe=INVALID_HANDLE_VALUE,event=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    bool open() {
        if(!event) return false;
        for(unsigned i=0;i<10;++i) {
            const auto path=std::format(L"\\\\?\\pipe\\discord-ipc-{}",i);
            pipe=CreateFileW(path.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,
                FILE_FLAG_OVERLAPPED|SECURITY_SQOS_PRESENT|SECURITY_IDENTIFICATION,nullptr);
            if(pipe!=INVALID_HANDLE_VALUE) return true;
        }
        return false;
    }
    bool valid() const {return pipe!=INVALID_HANDLE_VALUE;}
    void close() {if(valid()) CloseHandle(pipe);pipe=INVALID_HANDLE_VALUE;}
    bool transfer(void* data,unsigned size,unsigned& done,bool writing) {
        OVERLAPPED operation{};operation.hEvent=event;ResetEvent(event);
        const auto complete=writing?WriteFile(pipe,data,size,nullptr,&operation):ReadFile(pipe,data,size,nullptr,&operation);
        if(!complete && GetLastError()!=ERROR_IO_PENDING) return false;
        if(!complete && WaitForSingleObject(event,100)!=WAIT_OBJECT_0) {
            CancelIoEx(pipe,&operation);DWORD cancelled=0;GetOverlappedResult(pipe,&operation,&cancelled,TRUE);return false;
        }
        DWORD count=0;if(!GetOverlappedResult(pipe,&operation,&count,FALSE)) return false;
        done=count;return true;
    }
    bool receive(void* data,unsigned size,unsigned& done) {
        DWORD available=0;done=0;
        if(!PeekNamedPipe(pipe,nullptr,0,nullptr,&available,nullptr)) return false;
        return !available || transfer(data,std::min(size,unsigned(available)),done,false);
    }
    bool send(const void* data,unsigned size) {
        auto* bytes=static_cast<const char*>(data);
        while(size) {unsigned done=0;if(!transfer(const_cast<char*>(bytes),size,done,true) || !done) return false;bytes+=done;size-=done;}
        return true;
    }
    unsigned pid() const {return GetCurrentProcessId();}
    ~Connection() {close();if(event) CloseHandle(event);}
#else
    int socket=-1;
    bool open() {
        std::array<std::string_view,6> prefixes{};unsigned count=0;
        for(const auto* variable:{"XDG_RUNTIME_DIR","TMPDIR","TMP","TEMP"}) if(const auto* value=std::getenv(variable);value && *value)
            prefixes[count++]=value;
        std::string flatpak;
        if(const auto* runtime=std::getenv("XDG_RUNTIME_DIR");runtime && *runtime) {
            flatpak=std::string(runtime)+"/app/com.discordapp.Discord";
            prefixes[count++]=flatpak;
        }
        prefixes[count++]="/tmp";
        for(const auto prefix:std::span(prefixes).first(count)) for(unsigned i=0;i<10;++i) {
            sockaddr_un address{};address.sun_family=AF_UNIX;
            const auto path=std::format("{}/discord-ipc-{}",prefix,i);
            if(path.size()>=sizeof(address.sun_path)) continue;
            std::memcpy(address.sun_path,path.c_str(),path.size()+1);
            socket=::socket(AF_UNIX,SOCK_STREAM,0);if(socket<0) continue;
            if(fcntl(socket,F_SETFL,O_NONBLOCK)<0 || fcntl(socket,F_SETFD,FD_CLOEXEC)<0) {close();continue;}
#ifdef __APPLE__
            int no_signal=1;
            if(setsockopt(socket,SOL_SOCKET,SO_NOSIGPIPE,&no_signal,sizeof(no_signal))) {close();continue;}
#endif
            if(!::connect(socket,reinterpret_cast<sockaddr*>(&address),sizeof(address))) return true;
            if(errno==EINPROGRESS) {
                pollfd pending{socket,POLLOUT,0};int error=0;socklen_t length=sizeof(error);
                if(::poll(&pending,1,100)>0 && !getsockopt(socket,SOL_SOCKET,SO_ERROR,&error,&length) && !error) return true;
            }
            close();
        }
        return false;
    }
    bool valid() const {return socket>=0;}
    void close() {if(valid()) ::close(socket);socket=-1;}
    bool receive(void* data,unsigned size,unsigned& done) {
        const auto count=::recv(socket,data,size,0);done=0;
        if(count<0) return errno==EAGAIN || errno==EWOULDBLOCK || errno==EINTR;
        if(!count) return false;
        done=static_cast<unsigned>(count);return true;
    }
    bool send(const void* data,unsigned size) {
        auto* bytes=static_cast<const char*>(data);const auto deadline=Clock::now()+std::chrono::milliseconds(100);
        while(size) {
#ifdef __APPLE__
            const auto done=::send(socket,bytes,size,0);
#else
            const auto done=::send(socket,bytes,size,MSG_NOSIGNAL);
#endif
            if(done>0) {bytes+=done;size-=static_cast<unsigned>(done);continue;}
            if(done==0 || (errno!=EINTR && errno!=EAGAIN && errno!=EWOULDBLOCK) || Clock::now()>=deadline) return false;
            pollfd pending{socket,POLLOUT,0};if(::poll(&pending,1,10)<0 && errno!=EINTR) return false;
        }
        return true;
    }
    unsigned pid() const {return static_cast<unsigned>(getpid());}
    ~Connection() {close();}
#endif
    bool frame(unsigned opcode,std::string_view payload) {
        if(payload.size()>max_payload) return false;
        const std::array<std::uint32_t,2> header{opcode,static_cast<std::uint32_t>(payload.size())};
        return send(header.data(),sizeof(header)) && send(payload.data(),static_cast<unsigned>(payload.size()));
    }
};
}
namespace {
template<class T> T guest(std::span<const std::byte> ram,std::uint32_t address) {
    if(address>ram.size() || sizeof(T)>ram.size()-address) throw std::runtime_error("Guest activity field lies outside RAM");
    T value;std::memcpy(&value,ram.data()+address,sizeof(T));return value;
}
// Main modes, named MD_* by the table at 4773CC, and the game types that the
// Main Game and party menus write through their tables at 435348/43534C.
enum MainMode:std::int32_t {mode_title=0,mode_menus=1,mode_game=2,mode_test=3,mode_party=4,mode_options=5,mode_cutscenes=6,mode_replays=7};
enum GameType:std::int32_t {type_challenge=0,type_competition=1,type_practice=2,type_story=14};
// Course flags restored together with the difficulty and floor at 2490C9.
constexpr std::uint32_t course_extra=0x8,course_master=0x10,course_master_extra=0x800000;
// Sub-mode ranges, named SMD_* by the table at 4773F8.
struct SubModes {std::int32_t first,last;std::string_view text;};
constexpr std::array<SubModes,12> party_games{{
    {171,172,"Monkey Bowling DX"},{173,174,"Monkey Billiards DX"},{175,176,"Monkey Shot"},
    {185,186,"Monkey Race DX"},{187,188,"Monkey Golf DX"},{189,190,"Monkey Fight DX"},
    {191,192,"Monkey Target DX"},{193,194,"Monkey Boat"},{197,198,"Monkey Soccer"},
    {199,200,"Monkey Dogfight"},{201,202,"Monkey Baseball"},{203,204,"Monkey Tennis"}}};
constexpr std::size_t stage_name_lines=1001,max_stage_name_bytes=64*1024;
bool within(std::int32_t value,std::int32_t first,std::int32_t last) {return value>=first && value<=last;}
std::string_view stage_name(const GameActivity& activity,const StageNames& names) {
    if(activity.stage_id<=0 || std::size_t(activity.stage_id)>=names.size()) return {};
    const std::string_view name=names[activity.stage_id];
    return name=="-"?std::string_view{}:name;
}
// Discord accepts at most 128 bytes per activity line.
std::string_view activity_line(std::string_view text) {
    if(text.size()<=128) return text;
    auto size=std::size_t(128);
    while(size && (static_cast<unsigned char>(text[size])&0xC0)==0x80) --size;
    return text.substr(0,size);
}
}
GameActivity read_game_activity(std::span<const std::byte> ram) {
    GameActivity activity;
    activity.main_mode=guest<std::int32_t>(ram,0x6C9364);
    activity.sub_mode=guest<std::int32_t>(ram,0x6C9354);
    activity.game_type=guest<std::int32_t>(ram,0x6C9328);
    activity.difficulty=guest<std::int32_t>(ram,0x6C92F4);
    activity.course_flags=guest<std::uint32_t>(ram,0x6C92F8);
    activity.stage_id=guest<std::int32_t>(ram,0x5550A8);
    activity.floor=guest<std::int16_t>(ram,0x6C7D20);
    activity.world=guest<std::int16_t>(ram,0x6C7D36);
    activity.stage=guest<std::int16_t>(ram,0x6C7D38);
    activity.scene=guest<std::int16_t>(ram,0x6C9288);
    return activity;
}
StageNames load_stage_names(const std::filesystem::path& path) {
    // Every language file holds the same names; 83F50 indexes lines by stage id.
    Disc disc(path);
    const auto entry=disc.find("media/stgname/usa.str");
    if(entry.directory() || entry.size>max_stage_name_bytes) throw std::runtime_error("Invalid stage-name file on the game disc");
    std::string text(entry.size,'\0');
    disc.read(entry,0,std::as_writable_bytes(std::span(text)));
    StageNames names;names.reserve(stage_name_lines);
    for(std::size_t first=0;first<=text.size() && names.size()<stage_name_lines;) {
        auto end=text.find('\n',first);if(end==std::string::npos) end=text.size();
        auto line=std::string_view(text).substr(first,end-first);
        if(!line.empty() && line.back()=='\r') line.remove_suffix(1);
        names.emplace_back(line);first=end+1;
    }
    return names;
}
PresenceText describe_activity(const GameActivity& activity,const StageNames& names) {
    PresenceText text;
    switch(activity.main_mode) {
    case mode_title:text.details="At the title screen";break;
    case mode_menus:case mode_test:text.details="In the menus";break;
    case mode_options:text.details=within(activity.sub_mode,229,230)?"Watching a replay":"Changing options";break;
    case mode_cutscenes:
        // Story Mode plays its scenes through the same player as the menus.
        if(activity.game_type==type_story) {text.details="Story Mode";text.state="Watching a cutscene";}
        else text.details="Watching story cutscenes";
        break;
    case mode_replays:text.details="Watching a replay";break;
    case mode_party:
        text.details="Party Games";
        for(const auto& game:party_games) if(within(activity.sub_mode,game.first,game.last)) text.details=game.text;
        if(within(activity.sub_mode,169,170)) text.state="Choosing a game";
        break;
    case mode_game: {
        const auto name=stage_name(activity,names);
        if(activity.game_type==type_story) {
            text.details="Story Mode";
            if(within(activity.sub_mode,87,89)) {
                // SMD_GAME_SCENARIO_* runs the story menus; its DMD_SCEN_* scene
                // keeps the previous stage loaded behind stage select.
                if(within(activity.scene,6,7) || within(activity.scene,22,23)) text.state="Watching a cutscene";
                else if(within(activity.scene,8,11))
                    text.state=within(activity.world,0,9)?std::format("Choosing a stage in World {}",activity.world+1):"Choosing a stage";
                else if(within(activity.scene,20,21)) text.state="Entering a name";
            } else if(activity.sub_mode==94) text.state="Watching a cutscene";
            else if(!name.empty()) text.state=within(activity.world,0,9) && within(activity.stage,0,9)?
                std::format("Stage {}-{}: {}",activity.world+1,activity.stage+1,name):std::string(name);
        } else if(activity.game_type==type_challenge) {
            constexpr std::array<std::string_view,3> difficulties{"Beginner","Advanced","Expert"};
            std::string course;
            if(activity.course_flags&course_master_extra) course="Master Extra";
            else if(activity.course_flags&course_master) course="Master";
            else if(within(activity.difficulty,0,2))
                course=std::format("{}{}",difficulties[activity.difficulty],(activity.course_flags&course_extra)?" Extra":"");
            text.details=course.empty()?"Challenge Mode":"Challenge Mode: "+course;
            if(!name.empty()) text.state=activity.floor>0?std::format("Floor {}: {}",activity.floor,name):std::string(name);
        } else {
            text.details=activity.game_type==type_practice?"Practice Mode":activity.game_type==type_competition?"Competition Mode":"Main Game";
            text.state=name;
        }
        // The ending keeps the previous stage loaded.
        if(within(activity.sub_mode,72,73)) text.state="Watching the ending";
        break;
    }
    default:text.details="In game";
    }
    return text;
}
struct DiscordPresence::Impl {
    struct Desired {
        bool enabled=false,playing=false;
        PresenceText activity;
    };
    std::string application;
    bool configured=false;
    std::atomic<bool> stopping{false};
    std::atomic<Status> status{Status::disabled};
    std::mutex mutex;std::condition_variable changed;
    Desired desired;std::uint64_t revision=0; // Guarded by mutex.
    std::thread worker;
    explicit Impl(std::string_view id):application(id) {
        if(id.empty()) return;
        std::uint64_t number=0;const auto parsed=std::from_chars(id.data(),id.data()+id.size(),number);
        configured=number && parsed.ec==std::errc{} && parsed.ptr==id.data()+id.size();
    }
    ~Impl() {
        if(worker.joinable()) {
            {std::lock_guard lock(mutex);stopping=true;}
            changed.notify_one();worker.join();
        }
    }
    void run() {
        Connection connection;
        std::array<char,max_payload+8> input{};unsigned received=0;
        std::uint64_t sequence=0,sent=0;std::string pending;
        auto next_connect=Clock::time_point{},next_update=Clock::time_point{},handshake_deadline=Clock::time_point{};
        bool ready=false;
        auto started=std::time(nullptr);
        Desired current,previous;std::uint64_t current_revision=0;
        const auto activity=[&](const Desired* value) {
            std::string body="null";
            if(value) {
                // Before the game starts, the launcher shares only the title and elapsed time.
                std::string_view details,state;
                if(value->playing) {
                    details=value->activity.details.empty()?"In game":std::string_view(value->activity.details);
                    state=value->activity.state;
                }
                const auto line=[](std::string_view name,std::string_view text) {
                    return text.empty()?std::string{}:std::format("\"{}\":{},",name,json(activity_line(text)));
                };
                body=std::format("{{\"type\":0,{}{}\"assets\":{{\"large_image\":{},\"large_text\":\"Super Monkey Ball Deluxe\"}},\"timestamps\":{{\"start\":{}}}}}",
                    line("details",details),line("state",state),json(presence_icon),started);
            }
            pending=std::to_string(++sequence);
            return connection.frame(1,std::format("{{\"cmd\":\"SET_ACTIVITY\",\"args\":{{\"pid\":{},\"activity\":{}}},\"nonce\":{}}}",connection.pid(),body,json(pending)));
        };
        const auto disconnect=[&] {connection.close();received=0;sent=0;ready=false;pending.clear();next_connect=Clock::now()+retry_delay;};
        while(!stopping.load(std::memory_order_relaxed)) {
            {std::lock_guard lock(mutex);current=desired;current_revision=revision;}
            const auto now=Clock::now();
            if(current.enabled && (!previous.enabled || (current.playing && !previous.playing))) started=std::time(nullptr);
            if(!current.enabled) {
                if(ready) activity(nullptr);
                if(connection.valid()) disconnect();
                status=Status::disabled;next_connect={};
            } else if(!connection.valid() && now>=next_connect) {
                if(connection.open() && connection.frame(0,std::format("{{\"v\":1,\"client_id\":{}}}",json(application)))) {
                    status=Status::connecting;handshake_deadline=now+retry_delay;
                } else {disconnect();status=Status::waiting;}
            }
            if(current.enabled && connection.valid()) {
                unsigned count=0;
                if(!connection.receive(input.data()+received,static_cast<unsigned>(input.size())-received,count)) {disconnect();status=Status::waiting;}
                else {
                    received+=count;unsigned consumed=0;
                    while(received-consumed>=8) {
                        std::array<std::uint32_t,2> header{};std::memcpy(header.data(),input.data()+consumed,8);
                        if(header[1]>max_payload) {disconnect();status=Status::rejected;break;}
                        if(received-consumed<8+header[1]) break;
                        const std::string_view payload(input.data()+consumed+8,header[1]);consumed+=8+header[1];
                        if(header[0]==3) {if(!connection.frame(4,payload)) {disconnect();status=Status::waiting;break;}}
                        else if(header[0]==2 || (header[0]==1 && field(payload,"evt")=="ERROR")) {disconnect();status=Status::rejected;break;}
                        else if(header[0]==1) {
                            if(!ready && field(payload,"evt")=="READY") {ready=true;next_update={};}
                            else if(ready && field(payload,"cmd")=="SET_ACTIVITY" && !pending.empty() && field(payload,"nonce")==pending) {status=Status::connected;pending.clear();}
                        } else if(header[0]!=4) {disconnect();status=Status::rejected;break;}
                    }
                    if(connection.valid() && consumed) {received-=consumed;std::memmove(input.data(),input.data()+consumed,received);}
                }
                if(connection.valid() && (!ready || !pending.empty()) && now>=handshake_deadline) {disconnect();status=Status::waiting;}
                if(connection.valid() && ready && current_revision!=sent && now>=next_update) {
                    if(activity(&current)) {sent=current_revision;next_update=now+update_delay;handshake_deadline=now+retry_delay;}
                    else {disconnect();status=Status::waiting;}
                }
            }
            previous=current;
            std::unique_lock lock(mutex);
            const auto wake=[&]{return stopping.load(std::memory_order_relaxed) || revision!=current_revision;};
            if(current.enabled) changed.wait_for(lock,std::chrono::milliseconds(100),wake);
            else changed.wait(lock,wake);
        }
        if(ready) activity(nullptr);
    }
};
DiscordPresence::DiscordPresence(std::string_view application):impl_(std::make_unique<Impl>(application)) {}
DiscordPresence::~DiscordPresence()=default;
void DiscordPresence::update(bool enabled,bool playing,const PresenceText& activity) {
    auto& state=*impl_;
    if(enabled && !state.configured) {state.status=Status::unconfigured;return;}
    if(state.status==Status::failed) {
        if(!enabled) {
            if(state.worker.joinable()) state.worker.join();
            {std::lock_guard lock(state.mutex);state.desired={};++state.revision;}
            state.status=Status::disabled;
        }
        return;
    }
    {
        // Called every UI frame: compare in place and copy only real changes.
        std::lock_guard lock(state.mutex);
        const auto& current=state.desired;
        if(current.enabled==enabled && (!enabled || (current.playing==playing && current.activity==activity))) {
            if(!enabled && state.status==Status::unconfigured) state.status=Status::disabled;
            return;
        }
    }
    if(enabled && !state.worker.joinable()) {
        try {
            state.worker=std::thread([&state] {
                try {state.run();} catch(const std::exception&) {state.status=Status::failed;}
            });
        } catch(const std::system_error&) {state.status=Status::failed;return;}
    }
    {
        std::lock_guard lock(state.mutex);
        state.desired=enabled?Impl::Desired{true,playing,activity}:Impl::Desired{};++state.revision;
    }
    state.changed.notify_one();
}
std::string_view DiscordPresence::status() const {
    switch(impl_->status.load(std::memory_order_relaxed)) {
    case Status::disabled:return "Off";
    case Status::unconfigured:return "Discord Rich Presence is unavailable in this build.";
    case Status::waiting:return "Waiting for Discord desktop...";
    case Status::connecting:return "Connecting to Discord...";
    case Status::connected:return "Connected to Discord";
    case Status::rejected:return "Discord could not accept the activity. Retrying...";
    case Status::failed:return "Discord connection stopped. Turn Rich Presence off and back on to retry.";
    }
    return {};
}
}
