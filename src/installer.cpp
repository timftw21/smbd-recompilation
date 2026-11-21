#include "installation.h"
#include "file.h"
#include "diagnostics.h"
#include "ui_theme.h"
#include <Windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <fdi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <uxtheme.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <fstream>
#include <format>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <sstream>
#include <functional>
#include <optional>
#include <set>
#include <thread>

using Microsoft::WRL::ComPtr;
namespace {
constexpr wchar_t title[]=L"Super Monkey Ball Deluxe";
constexpr wchar_t registry[]=L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\SMBDRecomp";
constexpr std::string_view marker=smbd::install_marker;
constexpr COLORREF cream=RGB(255,242,201),gold=RGB(255,212,82),brown=RGB(48,27,10),yellow=RGB(255,204,31);
constexpr RECT form_panel{24,116,856,432};
constexpr int status_top=form_panel.bottom+8;
constexpr RECT status_panel{24,status_top,856,status_top+188};
constexpr int buttons_top=status_panel.bottom+10,client_width=880,client_height=buttons_top+54;
constexpr int form_left=form_panel.left+24,form_right=form_panel.right-24;
constexpr int status_left=status_panel.left+16,status_right=status_panel.right-16;
constexpr int checkbox_inset_x=12,checkbox_inset_y=4;
constexpr std::array checkbox_rows{RECT{form_left,330,form_right,364},RECT{form_left,374,form_right,408}};
using Entry=smbd::PayloadEntry;
struct State {
    std::mutex mutex;std::wstring phase=L"Choose your game image and installation folder.",error;
    unsigned percent=0,stage=0;bool running=false,complete=false;
    std::uint64_t revision=1;
    std::atomic_bool cancel=false;
    std::jthread worker;
} state;
HWND window=nullptr,source_edit=nullptr,destination_edit=nullptr,copy_check=nullptr,desktop_check=nullptr,install_button=nullptr,close_button=nullptr,progress_bar=nullptr;
std::vector<HWND> inputs;
std::vector<std::pair<HWND,RECT>> controls;
HFONT font=nullptr,heading=nullptr,font_small=nullptr;
HICON logo_icon=nullptr;
HBRUSH edit_brush=nullptr;
std::vector<std::uint32_t> wood,wood_inset;
unsigned dpi=96;
unsigned preview_dpi=0;
bool uninstalling=false;
unsigned cancellation_percent=101;
std::filesystem::path uninstall_root;
std::uint64_t started=0;
std::uint64_t status_invalidations=0;

void checked(bool success,std::string_view operation) {
    if(!success) throw std::runtime_error(std::string(operation)+std::format(" (Windows error {}).",GetLastError()));
}
void checked_hr(HRESULT result,std::string_view operation) {if(FAILED(result)) throw std::runtime_error(std::format("{} (0x{:08X}).",operation,unsigned(result)));}
std::filesystem::path module_path() {
    std::array<wchar_t,32768> path{};const auto count=GetModuleFileNameW(nullptr,path.data(),DWORD(path.size()));
    checked(count && count<path.size(),"Cannot locate Setup");return std::wstring_view(path.data(),count);
}
// A typed trailing separator would break folder comparisons and the quoted
// "root\" UninstallString argument, where \" reads as a literal quote.
std::filesystem::path folder_path(const std::filesystem::path& value) {
    auto path=std::filesystem::absolute(value).lexically_normal();
    while(!path.has_filename() && path!=path.root_path()) path=path.parent_path();
    return path;
}
std::filesystem::path known_folder(REFKNOWNFOLDERID id) {
    PWSTR value=nullptr;checked_hr(SHGetKnownFolderPath(id,0,nullptr,&value),"Cannot locate your user folders");
    std::filesystem::path path(value);CoTaskMemFree(value);return path;
}
smbd::Bytes resource(unsigned id) {
    const auto entry=FindResourceW(nullptr,MAKEINTRESOURCEW(id),RT_RCDATA);checked(entry!=nullptr,"Setup data is missing");
    const auto handle=LoadResource(nullptr,entry);checked(handle!=nullptr,"Cannot read Setup data");
    return {static_cast<const std::byte*>(LockResource(handle)),SizeofResource(nullptr,entry)};
}
std::filesystem::path relative_path(std::string_view value,bool data=false) {return smbd::install_relative_path(value,data);}
std::vector<Entry> payload() {
    const auto bytes=resource(3);
    return smbd::payload_manifest({reinterpret_cast<const char*>(bytes.data()),bytes.size()});
}
void report(unsigned stage,unsigned percent,std::string_view phase) {
    {std::lock_guard lock(state.mutex);state.stage=stage;state.percent=percent;state.phase=smbd::wide(phase);++state.revision;}
    smbd::diagnostic_record(std::format("{{\"type\":\"install_progress\",\"stage\":{},\"percent\":{},\"phase\":{}}}",stage,percent,smbd::json(phase)));
    if(stage==3 && percent>=cancellation_percent) state.cancel=true;
}
void cancellation() {if(state.cancel.load()) throw std::runtime_error("Installation cancelled. Your original game image and saved games were kept.");}
struct SetupLock {
    HANDLE handle=INVALID_HANDLE_VALUE;
    explicit SetupLock(const std::filesystem::path& root) {
        handle=CreateFileW((root/".setup-lock").c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,CREATE_NEW,
            FILE_ATTRIBUTE_TEMPORARY|FILE_FLAG_DELETE_ON_CLOSE,nullptr);
        if(handle==INVALID_HANDLE_VALUE) throw std::runtime_error("Another setup may be using this folder. Close it and try again.");
    }
    ~SetupLock() {CloseHandle(handle);}
};
struct Cabinet {
    std::filesystem::path directory;const std::vector<Entry>& entries;
    smbd::Bytes archive;std::map<INT_PTR,std::size_t> cursors;INT_PTR next_archive=-2;
    std::uint64_t completed=0,total=0,current_size=0;std::string error;
    std::set<HANDLE> opened;
    Cabinet(std::filesystem::path path,const std::vector<Entry>& files,smbd::Bytes bytes):directory(std::move(path)),entries(files),archive(bytes) {}
};
thread_local Cabinet* reading_cabinet=nullptr;
void* DIAMONDAPI cabinet_allocate(ULONG bytes) {return std::malloc(bytes);}
void DIAMONDAPI cabinet_free(void* data) {std::free(data);}
INT_PTR DIAMONDAPI cabinet_open(char*,int,int) {
    try {const auto handle=reading_cabinet->next_archive--;reading_cabinet->cursors.emplace(handle,0);return handle;} catch(...) {return -1;}
}
UINT DIAMONDAPI cabinet_read(INT_PTR handle,void* output,UINT bytes) {
    auto& cabinet=*reading_cabinet;
    const auto found=cabinet.cursors.find(handle);if(found==cabinet.cursors.end()) return UINT(-1);
    auto& cursor=found->second;const auto count=std::min<std::size_t>(bytes,cabinet.archive.size()-cursor);
    std::memcpy(output,cabinet.archive.data()+cursor,count);cursor+=count;return UINT(count);
}
UINT DIAMONDAPI cabinet_write(INT_PTR handle,void* data,UINT bytes) {
    DWORD written=0;return WriteFile(reinterpret_cast<HANDLE>(handle),data,bytes,&written,nullptr)?written:UINT(-1);
}
int DIAMONDAPI cabinet_close(INT_PTR handle) {
    if(reading_cabinet->cursors.erase(handle)) return 0;
    const auto file=reinterpret_cast<HANDLE>(handle);
    if(reading_cabinet->opened.erase(file)) return CloseHandle(file)?0:-1;
    return 0;
}
long DIAMONDAPI cabinet_seek(INT_PTR handle,long offset,int origin) {
    auto& cabinet=*reading_cabinet;
    const auto found=cabinet.cursors.find(handle);if(found==cabinet.cursors.end()) return -1;
    const auto base=origin==SEEK_SET?0:origin==SEEK_CUR?std::int64_t(found->second):std::int64_t(cabinet.archive.size());
    const auto position=base+offset;
    if(position<0 || std::uint64_t(position)>cabinet.archive.size()) return -1;
    found->second=std::size_t(position);return long(position);
}
INT_PTR DIAMONDAPI cabinet_callback(FDINOTIFICATIONTYPE notification,PFDINOTIFICATION info) {
    auto& cabinet=*static_cast<Cabinet*>(info->pv);
    try {
        cancellation();
        if(notification==fdintCOPY_FILE) {
            const auto name=relative_path(info->psz1);
            const auto found=std::ranges::find(cabinet.entries,name,&Entry::path);
            if(found==cabinet.entries.end() || std::uint64_t(info->cb)!=found->size) throw std::runtime_error("Unexpected file in Setup payload.");
            const auto target=cabinet.directory/name;std::filesystem::create_directories(target.parent_path());
            const auto file=CreateFileW(target.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
            checked(file!=INVALID_HANDLE_VALUE,"Cannot create an application file");cabinet.opened.insert(file);cabinet.current_size=found->size;
            report(2,unsigned(cabinet.completed*100/cabinet.total),"Installing "+smbd::path_text(name));
            return reinterpret_cast<INT_PTR>(file);
        }
        if(notification==fdintCLOSE_FILE_INFO) {
            checked(cabinet_close(info->hf)==0,"Cannot finish writing an application file");cabinet.completed+=cabinet.current_size;
            report(2,unsigned(cabinet.completed*100/cabinet.total),"Installing application files");
            return TRUE;
        }
        if(notification==fdintNEXT_CABINET || notification==fdintPARTIAL_FILE) throw std::runtime_error("Setup payload is incomplete.");
        return 0;
    } catch(const std::exception& error) {cabinet.error=error.what();return -1;}
}
void extract_payload(const std::filesystem::path& directory,const std::vector<Entry>& entries) {
    Cabinet context{directory,entries,resource(2)};for(const auto& entry:entries) context.total+=entry.size;
    reading_cabinet=&context;ERF error{};
    const auto decoder=FDICreate(cabinet_allocate,cabinet_free,cabinet_open,cabinet_read,cabinet_write,cabinet_close,cabinet_seek,cpuUNKNOWN,&error);
    if(!decoder) throw std::runtime_error("Cannot open the installer payload.");
    const auto result=FDICopy(decoder,const_cast<char*>("payload.cab"),const_cast<char*>(""),0,cabinet_callback,nullptr,&context);
    FDIDestroy(decoder);for(const auto file:context.opened) CloseHandle(file);reading_cabinet=nullptr;
    if(!context.error.empty()) throw std::runtime_error(context.error);
    if(!result) throw std::runtime_error(std::format("Cannot unpack the application (cabinet error {}).",error.erfOper));
    for(const auto& entry:entries) {
        cancellation();smbd::File file(directory/entry.path);
        if(file.size()!=entry.size || smbd::hash(file,0,file.size())!=entry.digest) throw std::runtime_error("Installed file failed verification: "+smbd::path_text(entry.path));
    }
}
void shortcut(const std::filesystem::path& destination,const std::filesystem::path& root) {
    ComPtr<IShellLinkW> link;checked_hr(CoCreateInstance(CLSID_ShellLink,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&link)),"Cannot create the game shortcut");
    checked_hr(link->SetPath((root/"smbd.exe").c_str()),"Cannot set the shortcut target");
    checked_hr(link->SetWorkingDirectory(root.c_str()),"Cannot set the shortcut folder");
    link->SetDescription(L"Play Super Monkey Ball Deluxe");link->SetIconLocation((root/"resources/smbd.ico").c_str(),0);
    ComPtr<IPersistFile> file;checked_hr(link.As(&file),"Cannot save the game shortcut");
    std::filesystem::create_directories(destination.parent_path());checked_hr(file->Save(destination.c_str(),TRUE),"Cannot save the game shortcut");
}
bool shortcut_targets(const std::filesystem::path& path,const std::filesystem::path& root) {
    ComPtr<IShellLinkW> link;
    if(FAILED(CoCreateInstance(CLSID_ShellLink,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&link)))) return false;
    ComPtr<IPersistFile> file;if(FAILED(link.As(&file)) || FAILED(file->Load(path.c_str(),STGM_READ))) return false;
    std::array<wchar_t,32768> target{};
    return SUCCEEDED(link->GetPath(target.data(),int(target.size()),nullptr,SLGP_RAWPATH)) && std::filesystem::path(target.data())==root/"smbd.exe";
}
void remove_registered_shortcut(const std::filesystem::path& path,const std::filesystem::path& root) {
    if(shortcut_targets(path,root)) {std::error_code error;std::filesystem::remove(path,error);}
}
void register_install(const std::filesystem::path& root,std::optional<bool> desktop) {
    checked_hr(CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED),"Cannot initialize shortcuts");
    struct Com {~Com(){CoUninitialize();}} com;
    shortcut(known_folder(FOLDERID_Programs)/L"Super Monkey Ball Deluxe.lnk",root);
    const auto desktop_link=known_folder(FOLDERID_Desktop)/L"Super Monkey Ball Deluxe.lnk";
    if(!desktop) desktop=shortcut_targets(desktop_link,root);
    if(*desktop) shortcut(desktop_link,root);
    else remove_registered_shortcut(desktop_link,root);
    HKEY key=nullptr;const auto status=RegCreateKeyExW(HKEY_CURRENT_USER,registry,0,nullptr,0,KEY_SET_VALUE,nullptr,&key,nullptr);
    if(status!=ERROR_SUCCESS) {SetLastError(status);checked(false,"Cannot register the uninstaller");}
    struct Key {HKEY key;~Key(){RegCloseKey(key);}} guard{key};
    const auto value=[&](const wchar_t* name,std::wstring text) {
        const auto error=RegSetValueExW(key,name,0,REG_SZ,reinterpret_cast<const BYTE*>(text.c_str()),DWORD((text.size()+1)*sizeof(wchar_t)));
        if(error!=ERROR_SUCCESS) {SetLastError(error);checked(false,"Cannot register installation details");}
    };
    value(L"DisplayName",title);value(L"DisplayVersion",smbd::wide(SMBD_VERSION));value(L"InstallLocation",root.native());
    value(L"DisplayIcon",(root/"smbd.exe").native());value(L"UninstallString",L"\""+(root/"Uninstall.exe").native()+L"\" --uninstall \""+root.native()+L"\"");
    const DWORD one=1;RegSetValueExW(key,L"NoModify",0,REG_DWORD,reinterpret_cast<const BYTE*>(&one),sizeof(one));
    RegSetValueExW(key,L"NoRepair",0,REG_DWORD,reinterpret_cast<const BYTE*>(&one),sizeof(one));
    const DWORD desktop_value=*desktop?1:0;
    RegSetValueExW(key,L"DesktopShortcut",0,REG_DWORD,reinterpret_cast<const BYTE*>(&desktop_value),sizeof(desktop_value));
}
void remove_stale_uninstallers() {
    // Earlier temporary uninstallers cannot delete themselves; remove idle ones.
    // A copy that is still running stays, because Windows refuses the deletion.
    std::error_code error;const auto directory=std::filesystem::temp_directory_path(error);if(error) return;
    for(std::filesystem::directory_iterator item(directory,error),end;!error && item!=end;item.increment(error)) {
        const std::wstring_view name=item->path().filename().native();
        if(!name.starts_with(L"smbd-uninstall-") || !name.ends_with(L".exe") || name.size()<=19) continue;
        const auto digits=name.substr(15,name.size()-19);
        if(std::ranges::all_of(digits,[](wchar_t c) {return c>=L'0' && c<=L'9';})) DeleteFileW(item->path().c_str());
    }
}
// Windows paths may differ only in letter case, as typed or as stored on disk.
bool same_path(const std::filesystem::path& a,const std::filesystem::path& b) {
    const auto left=a.lexically_normal().make_preferred(),right=b.lexically_normal().make_preferred();
    return CompareStringOrdinal(left.c_str(),-1,right.c_str(),-1,TRUE)==CSTR_EQUAL;
}
bool linked(const std::filesystem::path& path) {return !same_path(std::filesystem::weakly_canonical(path),path);}
bool empty_installation_folder(const std::filesystem::path& root) {
    for(const auto& item:std::filesystem::directory_iterator(root))
        if(!item.path().filename().native().starts_with(L".installing-")) return false;
    return true;
}
void remove_stale_staging(const std::filesystem::path& root) {
    // The exclusive folder lock proves no other setup owns these directories.
    std::vector<std::filesystem::path> stale;
    for(const auto& item:std::filesystem::directory_iterator(root)) {
        const auto attributes=GetFileAttributesW(item.path().c_str());
        if(item.path().filename().native().starts_with(L".installing-") && attributes!=INVALID_FILE_ATTRIBUTES &&
           (attributes&FILE_ATTRIBUTE_DIRECTORY) && !(attributes&FILE_ATTRIBUTE_REPARSE_POINT)) stale.push_back(item.path());
    }
    for(const auto& path:stale) std::filesystem::remove_all(path);
}
void write_manifest(const std::filesystem::path& path,const std::set<std::filesystem::path>& files) {
    std::string text(marker);text+='\n';for(const auto& file:files) text+=smbd::path_text(file)+'\n';
    smbd::write_text(path,text,false);
}
// desktop: create or remove the desktop shortcut; empty keeps the current choice.
void install(const std::filesystem::path& iso,const std::filesystem::path& chosen,bool copy_disc,std::optional<bool> desktop,bool registration) {
    auto root=folder_path(chosen);
    if(root==root.root_path() || root.native().size()>160 || linked(root))
        throw std::runtime_error("Choose a shorter installation folder without links or junctions.");
    root=std::filesystem::weakly_canonical(root);
    if(same_path(module_path().parent_path(),root)) throw std::runtime_error("Run the downloaded Setup to update this installation.");
    if(std::filesystem::exists(root) && !empty_installation_folder(root) && !std::filesystem::exists(root/"install-manifest.txt"))
        throw std::runtime_error("Choose an empty folder, or the folder containing an existing installation.");
    std::set<std::filesystem::path> previous_files;
    if(std::filesystem::exists(root/"install-manifest.txt")) {
        if(std::filesystem::file_size(root/"install-manifest.txt")>256*1024) throw std::runtime_error("Invalid previous installation manifest.");
        std::ifstream input(root/"install-manifest.txt");std::string line;std::getline(input,line);
        if(line!=marker) throw std::runtime_error("This folder belongs to another program. Choose an empty installation folder.");
        while(std::getline(input,line)) previous_files.insert(relative_path(line,true));
    }
    const auto entries=payload();
    for(const auto& entry:entries) {
        if(linked((root/entry.path).parent_path())) throw std::runtime_error("An application folder crosses a link or junction.");
        if(std::filesystem::exists(root/entry.path) && !previous_files.contains(entry.path))
            throw std::runtime_error("The destination contains an unrelated application file. Choose an empty folder.");
    }
    for(const auto& file:previous_files) if(linked((root/file).parent_path())) throw std::runtime_error("An application folder crosses a link or junction.");
    if(linked(root/"data/local")) throw std::runtime_error("The game-data folder crosses a link or junction.");
    report(1,0,"Checking your game image");smbd::verify_install_disc(iso);cancellation();report(1,100,"Game image verified");
    std::filesystem::create_directories(root);
    SetupLock lock(root);
    remove_stale_staging(root);
    const auto staging=root/std::format(".installing-{}",GetCurrentProcessId());
    if(!std::filesystem::create_directory(staging)) throw std::runtime_error("An installation is already using this folder.");
    struct Staging {
        std::filesystem::path path,root;
        ~Staging() {
            // Recursive cleanup is restricted to this newly-created child of the selected installation folder.
            std::error_code error;
            if(path.parent_path()==root && path.filename().native().starts_with(L".installing-") && same_path(std::filesystem::weakly_canonical(path,error),path))
                std::filesystem::remove_all(path,error);
        }
    } cleanup{staging,root};
    report(2,0,"Installing application files");extract_payload(staging,entries);
    const auto preferences=root/"data/local/settings.cfg";
    if(std::filesystem::exists(preferences)) {
        std::filesystem::create_directories(staging/"data/local");
        std::filesystem::copy_file(preferences,staging/"data/local/settings.cfg");
    }
    report(3,0,"Preparing your game files");
    bool reuse_disc=false;
    if(copy_disc && std::filesystem::exists(root/"data/local/game.iso") && std::filesystem::equivalent(iso,root/"data/local/game.iso")) {
        std::filesystem::create_directories(staging/"data/local");std::error_code error;
        std::filesystem::create_hard_link(root/"data/local/game.iso",staging/"data/local/game.iso",error);reuse_disc=!error;
    }
    smbd::install_game_files(iso,staging,copy_disc,[](unsigned percent,std::string_view phase){report(3,percent,phase);},&state.cancel);
    checked(CopyFileW(module_path().c_str(),(staging/"Uninstall.exe").c_str(),TRUE)!=FALSE,"Cannot prepare the uninstaller");
    std::set<std::filesystem::path> recorded;for(const auto& entry:entries) recorded.insert(entry.path);
    recorded.insert("Uninstall.exe");recorded.insert("data/local/default.xbe");
    if(copy_disc || previous_files.contains("data/local/game.iso")) recorded.insert("data/local/game.iso");
    write_manifest(staging/"install-manifest.txt",recorded);
    // Until every file is in place the folder's manifest also lists the previous
    // files, so an interrupted update is repaired by running Setup again.
    auto pending=recorded;pending.insert(previous_files.begin(),previous_files.end());
    write_manifest(staging/"install-manifest.pending",pending);
    std::vector<std::filesystem::path> files(recorded.begin(),recorded.end());
    files.push_back("data/local/settings.cfg");files.push_back("install-manifest.txt");cancellation();
    // Detect a running game before replacing or removing any of its application files.
    for(const auto& file:pending) if(file!="data/local/game.iso" && std::filesystem::exists(root/file)) {
        const auto handle=CreateFileW((root/file).c_str(),GENERIC_WRITE|DELETE,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(handle==INVALID_HANDLE_VALUE) throw std::runtime_error("Close the game and its diagnostics, then try installing again.");
        CloseHandle(handle);
    }
    report(4,0,"Finishing installation");
    const auto move=[&](const std::filesystem::path& from,const std::filesystem::path& to) {
        checked(MoveFileExW(from.c_str(),to.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=FALSE,"Cannot finish installing application files");
    };
    move(staging/"install-manifest.pending",root/"install-manifest.txt");
    unsigned committed=0;
    for(const auto& file:files) {
        if(reuse_disc && file=="data/local/game.iso") continue;
        if(!std::filesystem::exists(staging/file)) continue;
        std::filesystem::create_directories((root/file).parent_path());
        move(staging/file,root/file);
        report(4,unsigned(++committed*100/files.size()),"Finishing installation");
    }
    // Remove application files that this version no longer ships.
    for(const auto& file:previous_files) if(!recorded.contains(file)) {
        std::error_code error;std::filesystem::remove(root/file,error);
        for(auto parent=file.parent_path();!parent.empty();parent=parent.parent_path()) RemoveDirectoryW((root/parent).c_str());
    }
    if(registration) register_install(root,desktop);
    report(4,100,"Ready to play. Your installation is complete.");
}
void uninstall(const std::filesystem::path& chosen) {
    auto root=folder_path(chosen);
    if(root==root.root_path() || linked(root)) throw std::runtime_error("Invalid installation folder.");
    root=std::filesystem::weakly_canonical(root);
    const auto path=root/"install-manifest.txt";
    if(!std::filesystem::exists(path)) throw std::runtime_error("This folder is not an installed copy of the game.");
    SetupLock lock(root);
    if(std::filesystem::file_size(path)>256*1024) throw std::runtime_error("Invalid uninstall manifest.");
    std::ifstream input(path);std::string line;std::getline(input,line);
    if(line!=marker) throw std::runtime_error("This folder is not a Super Monkey Ball Deluxe installation.");
    const auto entries=payload();std::vector<std::filesystem::path> files;
    while(std::getline(input,line)) {
        const auto file=relative_path(line,true);
        if(std::ranges::find(entries,file,&Entry::path)==entries.end() && file!="Uninstall.exe" && file!="data/local/default.xbe" && file!="data/local/game.iso")
            throw std::runtime_error("Uninstall manifest contains an unknown file. No files were removed.");
        if(linked((root/file).parent_path())) throw std::runtime_error("An uninstall path crosses a link or junction.");
        files.push_back(file);
    }
    input.close();unsigned count=0;
    for(const auto& file:files) if(std::filesystem::exists(root/file)) {
        const auto handle=CreateFileW((root/file).c_str(),GENERIC_WRITE|DELETE,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(handle==INVALID_HANDLE_VALUE) throw std::runtime_error("Close the game and its diagnostics before uninstalling. No files were removed.");
        CloseHandle(handle);
    }
    for(const auto& file:files) {
        report(1,unsigned(count++*100/files.size()),"Removing "+smbd::path_text(file));
        std::error_code error;std::filesystem::remove(root/file,error);
        if(error) throw std::runtime_error("Close the game before uninstalling. Could not remove "+smbd::path_text(file));
    }
    std::filesystem::remove(path);
    const auto initialized=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    if(SUCCEEDED(initialized)) {
        remove_registered_shortcut(known_folder(FOLDERID_Programs)/L"Super Monkey Ball Deluxe.lnk",root);
        remove_registered_shortcut(known_folder(FOLDERID_Desktop)/L"Super Monkey Ball Deluxe.lnk",root);CoUninitialize();
    }
    HKEY key=nullptr;
    if(RegOpenKeyExW(HKEY_CURRENT_USER,registry,0,KEY_QUERY_VALUE,&key)==ERROR_SUCCESS) {
        std::array<wchar_t,32768> location{};DWORD size=DWORD(sizeof(location));
        const auto status=RegGetValueW(key,nullptr,L"InstallLocation",RRF_RT_REG_SZ,nullptr,location.data(),&size);RegCloseKey(key);
        if(status==ERROR_SUCCESS && same_path(location.data(),root)) RegDeleteTreeW(HKEY_CURRENT_USER,registry);
    }
    // Remove now-empty application folders, deepest first; saves keep data\ in place.
    std::set<std::filesystem::path,std::greater<>> folders;
    for(const auto& file:files) for(auto parent=file.parent_path();!parent.empty();parent=parent.parent_path()) folders.insert(parent);
    for(const auto& folder:folders) RemoveDirectoryW((root/folder).c_str());
    RemoveDirectoryW(root.c_str());
    report(1,100,"Uninstalled. Your saved games and preferences were kept.");
}
int scale(int value) {return MulDiv(value,int(dpi),96);}
struct Defaults {
    std::filesystem::path folder,iso;
    bool copy=true,desktop=false;
};
Defaults defaults() {
    Defaults result{known_folder(FOLDERID_LocalAppData)/L"Programs"/L"Super Monkey Ball Deluxe",{},true,false};
    std::array<wchar_t,32768> previous{};DWORD size=DWORD(sizeof(previous));
    if(RegGetValueW(HKEY_CURRENT_USER,registry,L"InstallLocation",RRF_RT_REG_SZ,nullptr,previous.data(),&size)==ERROR_SUCCESS) {
        const std::filesystem::path path(previous.data());
        if(path.is_absolute() && std::filesystem::exists(path/"install-manifest.txt")) result.folder=path;
    }
    const auto manifest=result.folder/"install-manifest.txt";
    if(std::filesystem::exists(manifest) && std::filesystem::file_size(manifest)<256*1024) {
        std::ifstream input(manifest);std::string line;std::getline(input,line);
        if(line==marker) {
            result.copy=false;
            while(std::getline(input,line)) if(line=="data/local/game.iso" || line=="data\\local\\game.iso") result.copy=true;
            DWORD desktop=0;size=sizeof(desktop);
            if(RegGetValueW(HKEY_CURRENT_USER,registry,L"DesktopShortcut",RRF_RT_REG_DWORD,nullptr,&desktop,&size)==ERROR_SUCCESS) result.desktop=desktop!=0;
        }
    }
    if(std::filesystem::exists(result.folder/"data/local/game.iso") && result.copy) result.iso=result.folder/"data/local/game.iso";
    else {
        const auto settings=result.folder/"data/local/settings.cfg";
        if(std::filesystem::exists(settings) && std::filesystem::file_size(settings)<16384) {
            std::ifstream input(settings);std::string line;
            while(std::getline(input,line)) if(line.starts_with("disc=")) result.iso=result.folder/smbd::utf8_path(line.substr(5));
        }
    }
    if(!result.iso.empty() && !std::filesystem::exists(result.iso)) result.iso.clear();
    result.folder.make_preferred();return result;
}
void update_fonts() {
    for(const auto old:{font,heading,font_small}) if(old) DeleteObject(old);
    const auto make=[](int pixels) {return CreateFontW(-scale(pixels),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");};
    font=make(18);heading=make(26);font_small=make(15);
    HICON icon=nullptr;
    checked_hr(LoadIconWithScaleDown(GetModuleHandleW(nullptr),MAKEINTRESOURCEW(1),scale(56),scale(56),&icon),"Cannot load the installer icon");
    if(logo_icon) DestroyIcon(logo_icon);logo_icon=icon;
    for(const auto& entry:controls) SendMessageW(entry.first,WM_SETFONT,reinterpret_cast<WPARAM>(font),TRUE);
}
std::wstring text(HWND control) {
    const auto length=GetWindowTextLengthW(control);std::wstring value(std::size_t(length)+1,L'\0');
    GetWindowTextW(control,value.data(),length+1);value.resize(length);return value;
}
void enable_control(HWND handle,bool enabled) {if((IsWindowEnabled(handle)!=FALSE)!=enabled) EnableWindow(handle,enabled);}
void caption_control(HWND handle,const wchar_t* caption) {if(text(handle)!=caption) SetWindowTextW(handle,caption);}
bool paths_ready() {
    return uninstalling || (text(source_edit).find_first_not_of(L" \t\r\n")!=std::wstring::npos &&
        text(destination_edit).find_first_not_of(L" \t\r\n")!=std::wstring::npos);
}
void update_install_button() {
    if(!install_button) return;
    bool running,complete;
    {std::lock_guard lock(state.mutex);running=state.running;complete=state.complete;}
    enable_control(install_button,!running && (complete?!uninstalling:paths_ready()));
}
void invalidate_status(HWND handle) {
    ++status_invalidations;
    const RECT bounds{scale(status_panel.left),scale(status_panel.top),scale(status_panel.right),scale(status_panel.bottom)};
    InvalidateRect(handle,&bounds,FALSE);
}
HWND control(const wchar_t* type,const wchar_t* caption,DWORD style,int id,RECT rectangle) {
    const auto child=CreateWindowExW(0,type,caption,WS_CHILD|WS_VISIBLE|style,0,0,0,0,window,reinterpret_cast<HMENU>(INT_PTR(id)),GetModuleHandleW(nullptr),nullptr);
    checked(child!=nullptr,"Cannot create installer controls");SendMessageW(child,WM_SETFONT,reinterpret_cast<WPARAM>(font),TRUE);
    controls.emplace_back(child,rectangle);return child;
}
HWND checkbox(const wchar_t* caption,int id,RECT row) {
    InflateRect(&row,-checkbox_inset_x,-checkbox_inset_y);
    const auto child=control(L"BUTTON",caption,BS_AUTOCHECKBOX|WS_TABSTOP,id,row);
    SetWindowTheme(child,L"",L"");return child;
}
void position_controls() {
    for(const auto& [child,r]:controls) MoveWindow(child,scale(r.left),scale(r.top),scale(r.right-r.left),scale(r.bottom-r.top),TRUE);
}
void choose(bool folder) {
    ComPtr<IFileOpenDialog> dialog;checked_hr(CoCreateInstance(CLSID_FileOpenDialog,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&dialog)),"Cannot open the file picker");
    DWORD options=0;dialog->GetOptions(&options);
    dialog->SetOptions(options|FOS_FORCEFILESYSTEM|(folder?FOS_PICKFOLDERS:FOS_FILEMUSTEXIST));
    dialog->SetTitle(folder?L"Choose an installation folder":L"Select your Super Monkey Ball Deluxe ISO");
    const COMDLG_FILTERSPEC filter[]={{L"Xbox disc images (*.iso)",L"*.iso"},{L"All files",L"*.*"}};
    if(!folder) dialog->SetFileTypes(2,filter);
    const auto result=dialog->Show(window);if(result==HRESULT_FROM_WIN32(ERROR_CANCELLED)) return;checked_hr(result,"Cannot select a path");
    ComPtr<IShellItem> item;checked_hr(dialog->GetResult(&item),"Cannot read the selected path");
    PWSTR value=nullptr;checked_hr(item->GetDisplayName(SIGDN_FILESYSPATH,&value),"Cannot read the selected path");
    SetWindowTextW(folder?destination_edit:source_edit,value);CoTaskMemFree(value);
}
void start() {
    if(state.worker.joinable()) state.worker.join();
    const auto iso=text(source_edit);
    const auto destination=uninstalling?uninstall_root:std::filesystem::path(text(destination_edit));
    const bool copy=SendMessageW(copy_check,BM_GETCHECK,0,0)==BST_CHECKED,desktop=SendMessageW(desktop_check,BM_GETCHECK,0,0)==BST_CHECKED;
    if(!paths_ready()) throw std::runtime_error("Choose your game image and installation folder first.");
    {std::lock_guard lock(state.mutex);state.running=true;state.complete=false;state.error.clear();state.stage=0;state.percent=0;++state.revision;}
    state.cancel=false;started=GetTickCount64();
    for(const auto input:inputs) EnableWindow(input,FALSE);EnableWindow(install_button,FALSE);SetWindowTextW(close_button,L"Cancel");
    state.worker=std::jthread([iso,destination,copy,desktop] {
        try {
            if(uninstalling) uninstall(destination);else install(iso,destination,copy,desktop,true);
            std::lock_guard lock(state.mutex);state.complete=true;++state.revision;
        } catch(const std::exception& error) {
            smbd::diagnostic_record("{\"type\":\"install_error\",\"message\":"+smbd::json(error.what())+'}');
            std::lock_guard lock(state.mutex);state.error=smbd::wide(error.what());state.phase=L"Setup did not finish. You can correct the problem and try again.";++state.revision;
        }
        std::lock_guard lock(state.mutex);state.running=false;++state.revision;
    });
}
void draw_text(HDC dc,std::wstring value,RECT rectangle,COLORREF color,HFONT selected,UINT flags=DT_LEFT|DT_WORDBREAK) {
    for(auto* p:{&rectangle.left,&rectangle.top,&rectangle.right,&rectangle.bottom}) *p=scale(*p);
    const auto old=SelectObject(dc,selected);SetTextColor(dc,color);SetBkMode(dc,TRANSPARENT);
    DrawTextW(dc,value.c_str(),int(value.size()),&rectangle,flags);SelectObject(dc,old);
}
void paint(HDC dc) {
    SetStretchBltMode(dc,HALFTONE);SetBrushOrgEx(dc,0,0,nullptr);
    BITMAPINFO bitmap{};bitmap.bmiHeader={sizeof(BITMAPINFOHEADER),512,-256,1,32,BI_RGB,0,0,0,0,0};
    StretchDIBits(dc,0,0,scale(client_width),scale(client_height),0,0,512,256,wood.data(),&bitmap,DIB_RGB_COLORS,SRCCOPY);
    auto pen=CreatePen(PS_SOLID,scale(2),RGB(183,129,51));const auto old_pen=SelectObject(dc,pen);const auto old_brush=SelectObject(dc,GetStockObject(NULL_BRUSH));
    for(const auto r:{form_panel,status_panel}) {
        const auto saved=SaveDC(dc);const auto clip=CreateRoundRectRgn(scale(r.left),scale(r.top),scale(r.right),scale(r.bottom),scale(14),scale(14));
        ExtSelectClipRgn(dc,clip,RGN_AND);StretchDIBits(dc,scale(r.left),scale(r.top),scale(r.right-r.left),scale(r.bottom-r.top),0,0,512,256,wood_inset.data(),&bitmap,DIB_RGB_COLORS,SRCCOPY);
        RestoreDC(dc,saved);DeleteObject(clip);RoundRect(dc,scale(r.left),scale(r.top),scale(r.right),scale(r.bottom),scale(14),scale(14));
    }
    SelectObject(dc,old_brush);SelectObject(dc,old_pen);DeleteObject(pen);
    if(!uninstalling) for(const auto row:checkbox_rows) {
        const RECT background{scale(row.left),scale(row.top),scale(row.right),scale(row.bottom)};
        FillRect(dc,&background,edit_brush);
    }
    DrawIconEx(dc,scale(28),scale(28),logo_icon,scale(56),scale(56),0,nullptr,DI_NORMAL);
    draw_text(dc,uninstalling?L"Uninstallation":L"Installation",{100,28,850,65},gold,heading);
    draw_text(dc,L"v"+smbd::wide(SMBD_VERSION)+L" · Windows x64",{640,34,840,60},gold,font_small,DT_RIGHT|DT_SINGLELINE);
    draw_text(dc,uninstalling?L"Uninstall — your saved games and preferences will be kept.":L"Select your copy of Super Monkey Ball Deluxe and get ready to play!",{100,70,850,106},cream,font_small);
    if(!uninstalling) {
        draw_text(dc,L"YOUR GAME IMAGE",{form_left,140,form_right,162},gold,font_small);
        draw_text(dc,L"Use a US Xbox XISO image. Game files are verified before installation.",{form_left,218,form_right,242},cream,font_small);
        draw_text(dc,L"INSTALLATION FOLDER",{form_left,254,form_right,276},gold,font_small);
    } else draw_text(dc,L"Remove the installed program and its copied game files. Your original ISO, saved games, and preferences are kept.",{40,140,824,215},cream,font);
    std::wstring phase,error;unsigned percent,stage;bool running,complete;
    {std::lock_guard lock(state.mutex);phase=state.phase;error=state.error;percent=state.percent;stage=state.stage;running=state.running;complete=state.complete;}
    const auto caption=running?std::format(L"STEP {} OF {}  ·  {}%",stage,uninstalling?1:4,percent):complete?L"COMPLETE":L"INSTALLATION PROGRESS";
    draw_text(dc,caption,{status_left,status_top+12,status_right,status_top+36},gold,font_small);
    draw_text(dc,phase,{status_left,status_top+43,status_right,status_top+77},cream,font);
    if(!error.empty()) draw_text(dc,error,{status_left,status_top+112,status_right,status_top+178},RGB(255,200,134),font_small);
    else if(running) draw_text(dc,std::format(L"Elapsed: {}:{:02}  ·  You can leave this window open while setup works.",(GetTickCount64()-started)/60000,((GetTickCount64()-started)/1000)%60),{status_left,status_top+112,status_right,status_top+152},cream,font_small);
    else draw_text(dc,uninstalling?L"Only files recorded by this installer are removed.":L"Your original game image and saved games are kept.",{status_left,status_top+112,status_right,status_top+152},cream,font_small);
}
LRESULT CALLBACK procedure(HWND handle,UINT message,WPARAM wparam,LPARAM lparam) {
    try {
        if(message==WM_PAINT) {PAINTSTRUCT ps{};const auto dc=BeginPaint(handle,&ps);paint(dc);EndPaint(handle,&ps);return 0;}
        if(message==WM_PRINTCLIENT) {paint(reinterpret_cast<HDC>(wparam));return 0;}
        if(message==WM_ERASEBKGND) return 1;
        if(message==WM_CTLCOLORSTATIC || message==WM_CTLCOLOREDIT) {
            const auto dc=reinterpret_cast<HDC>(wparam);SetTextColor(dc,cream);SetBkColor(dc,brown);return reinterpret_cast<LRESULT>(edit_brush);
        }
        if(message==WM_DRAWITEM) {
            const auto& item=*reinterpret_cast<DRAWITEMSTRUCT*>(lparam);const bool primary=item.hwndItem==install_button;
            const auto saved=SaveDC(item.hDC);
            DrawThemeParentBackground(item.hwndItem,item.hDC,&item.rcItem);
            RestoreDC(item.hDC,saved);
            const bool disabled=(item.itemState&ODS_DISABLED)!=0,pressed=(item.itemState&ODS_SELECTED)!=0;
            const auto brush=CreateSolidBrush(primary?(disabled?RGB(147,115,37):pressed?RGB(242,178,10):yellow):RGB(102,61,20));
            const auto old=SelectObject(item.hDC,brush);const auto pen=CreatePen(PS_SOLID,1,RGB(183,129,51));const auto old_pen=SelectObject(item.hDC,pen);
            RoundRect(item.hDC,item.rcItem.left,item.rcItem.top,item.rcItem.right,item.rcItem.bottom,scale(8),scale(8));SelectObject(item.hDC,old);SelectObject(item.hDC,old_pen);DeleteObject(brush);DeleteObject(pen);
            const auto old_font=SelectObject(item.hDC,font);SetBkMode(item.hDC,TRANSPARENT);SetTextColor(item.hDC,primary?RGB(30,30,9):cream);
            auto bounds=item.rcItem;const auto value=text(item.hwndItem);DrawTextW(item.hDC,value.c_str(),int(value.size()),&bounds,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
            if(item.itemState&ODS_FOCUS) {InflateRect(&bounds,-scale(4),-scale(4));DrawFocusRect(item.hDC,&bounds);}SelectObject(item.hDC,old_font);return TRUE;
        }
        if(message==WM_COMMAND) {
            switch(LOWORD(wparam)) {
            case 101:choose(false);break;case 102:choose(true);break;
            case 110:case 111:if(HIWORD(wparam)==EN_CHANGE) update_install_button();break;
            case 103: {
                bool complete;{std::lock_guard lock(state.mutex);complete=state.complete;}
                if(complete && !uninstalling) checked(reinterpret_cast<INT_PTR>(ShellExecuteW(window,L"open",(std::filesystem::path(text(destination_edit))/"smbd.exe").c_str(),nullptr,nullptr,SW_SHOWNORMAL))>32,"Cannot launch the game");
                else start();break;
            }
            case 104:SendMessageW(handle,WM_CLOSE,0,0);break;
            }return 0;
        }
        if(message==WM_TIMER) {
            bool running,complete;unsigned percent,stage;
            std::uint64_t revision;
            {std::lock_guard lock(state.mutex);running=state.running;complete=state.complete;percent=state.percent;stage=state.stage;revision=state.revision;}
            static std::uint64_t seen_revision=0,seen_second=0;
            const auto second=running?(GetTickCount64()-started)/1000:0;
            if(revision==seen_revision && second==seen_second) return 0;
            seen_revision=revision;seen_second=second;
            if(SendMessageW(progress_bar,PBM_GETPOS,0,0)!=percent) SendMessageW(progress_bar,PBM_SETPOS,percent,0);
            if(!running) {
                enable_control(close_button,true);
                for(const auto input:inputs) enable_control(input,!complete);
                update_install_button();caption_control(install_button,complete&&!uninstalling?L"Play game":uninstalling?L"Uninstall":L"Install");
                caption_control(close_button,L"Close");
            } else enable_control(close_button,!uninstalling && stage<4 && !state.cancel.load());  // Uninstall cannot be cancelled.
            invalidate_status(handle);return 0;
        }
        if(message==WM_DPICHANGED) {
            dpi=HIWORD(wparam);update_fonts();const auto& rectangle=*reinterpret_cast<RECT*>(lparam);
            SetWindowPos(handle,nullptr,rectangle.left,rectangle.top,rectangle.right-rectangle.left,rectangle.bottom-rectangle.top,SWP_NOZORDER);position_controls();return 0;
        }
        if(message==WM_CLOSE) {
            bool running;{std::lock_guard lock(state.mutex);running=state.running;}
            if(running && uninstalling) return 0;
            if(running) {state.cancel=true;SetWindowTextW(close_button,L"Cancelling…");EnableWindow(close_button,FALSE);return 0;}
            DestroyWindow(handle);return 0;
        }
        if(message==WM_DESTROY) {PostQuitMessage(0);return 0;}
    } catch(const std::exception& error) {std::lock_guard lock(state.mutex);state.error=smbd::wide(error.what());++state.revision;invalidate_status(handle);return 0;}
    return DefWindowProcW(handle,message,wparam,lparam);
}
void capture_preview(const std::filesystem::path& path) {
    bool idle_repaint=false;std::uint64_t idle_invalidations=0;
    if(!state.running) {
        SendMessageW(window,WM_TIMER,1,0);
        RedrawWindow(window,nullptr,nullptr,RDW_VALIDATE|RDW_ALLCHILDREN|RDW_NOERASE|RDW_NOINTERNALPAINT);
        const auto before=status_invalidations;
        for(unsigned i=0;i<10;++i) SendMessageW(window,WM_TIMER,1,0);
        idle_invalidations=status_invalidations-before;
        idle_repaint=GetUpdateRect(window,nullptr,FALSE)!=FALSE;
        if(idle_invalidations) throw std::runtime_error("Idle installer requested an unnecessary repaint");
    }
    const int width=scale(client_width),height=scale(client_height);const auto dc=GetDC(window),memory=CreateCompatibleDC(dc);const auto bitmap=CreateCompatibleBitmap(dc,width,height);const auto old=SelectObject(memory,bitmap);
    paint(memory);
    for(const auto& [child,r]:controls) if(IsWindowVisible(child) || !uninstalling) {
        const auto saved=SaveDC(memory);SetViewportOrgEx(memory,scale(r.left),scale(r.top),nullptr);
        IntersectClipRect(memory,0,0,scale(r.right-r.left),scale(r.bottom-r.top));
        SendMessageW(child,WM_PRINT,reinterpret_cast<WPARAM>(memory),PRF_CLIENT|PRF_NONCLIENT|PRF_ERASEBKGND);
        RestoreDC(memory,saved);
    }
    SelectObject(memory,old);
    BITMAPINFO info{};info.bmiHeader={sizeof(BITMAPINFOHEADER),width,height,1,32,BI_RGB,0,0,0,0,0};std::vector<std::byte> pixels(std::size_t(width)*height*4);
    checked(GetDIBits(memory,bitmap,0,height,pixels.data(),&info,DIB_RGB_COLORS)!=0,"Cannot capture the installer preview");
    BITMAPFILEHEADER file{0x4D42,DWORD(sizeof(BITMAPFILEHEADER)+sizeof(BITMAPINFOHEADER)+pixels.size()),0,0,DWORD(sizeof(BITMAPFILEHEADER)+sizeof(BITMAPINFOHEADER))};
    std::ofstream output(path,std::ios::binary);output.exceptions(std::ios::failbit|std::ios::badbit);
    output.write(reinterpret_cast<const char*>(&file),sizeof(file));output.write(reinterpret_cast<const char*>(&info.bmiHeader),sizeof(info.bmiHeader));output.write(reinterpret_cast<const char*>(pixels.data()),std::streamsize(pixels.size()));
    SelectObject(memory,old);DeleteObject(bitmap);DeleteDC(memory);ReleaseDC(window,dc);
    ICONINFO icon{};checked(GetIconInfo(logo_icon,&icon)!=FALSE,"Cannot inspect the installer icon");
    BITMAP image{};checked(GetObjectW(icon.hbmColor,sizeof(image),&image)!=0,"Cannot inspect icon dimensions");
    DeleteObject(icon.hbmColor);DeleteObject(icon.hbmMask);
    smbd::write_text(path.native()+L".json",std::format("{{\"format\":\"smbd-installer-preview-v1\",\"render_dpi\":{},\"size\":[{},{}],\"icon_size\":[{},{}],\"composited\":{},\"clips_children\":{},\"idle_timer_invalidations\":{},\"pending_window_paint\":{},\"install_enabled\":{}}}\n",
        dpi,width,height,image.bmWidth,image.bmHeight,(GetWindowLongPtrW(window,GWL_EXSTYLE)&WS_EX_COMPOSITED)!=0,
        (GetWindowLongPtrW(window,GWL_STYLE)&WS_CLIPCHILDREN)!=0,idle_invalidations,idle_repaint,IsWindowEnabled(install_button)!=FALSE),true);
}
int wizard(const std::filesystem::path& preview={}) {
    checked_hr(CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED),"Cannot initialize Setup");
    const INITCOMMONCONTROLSEX common{sizeof(common),ICC_PROGRESS_CLASS|ICC_STANDARD_CLASSES};InitCommonControlsEx(&common);
    dpi=preview_dpi?preview_dpi:GetDpiForSystem();update_fonts();
    edit_brush=CreateSolidBrush(brown);wood.resize(512*256);wood_inset.resize(wood.size());
    for(unsigned y=0;y<256;++y) for(unsigned x=0;x<512;++x) {
        const auto p=smbd::theme::wood_pixel(x,y);wood[y*512+x]=(unsigned(p[0])<<16)|(unsigned(p[1])<<8)|p[2];
        wood_inset[y*512+x]=((unsigned(p[0])*153/255)<<16)|((unsigned(p[1])*139/255)<<8)|(unsigned(p[2])*118/255);
    }
    WNDCLASSW cls{};cls.lpfnWndProc=procedure;cls.hInstance=GetModuleHandleW(nullptr);cls.hCursor=LoadCursorW(nullptr,IDC_ARROW);cls.hIcon=LoadIconW(cls.hInstance,MAKEINTRESOURCEW(1));cls.lpszClassName=L"SMBDInstaller";
    checked(RegisterClassW(&cls)!=0,"Cannot register the installer window");
    RECT bounds{0,0,scale(client_width),scale(client_height)};AdjustWindowRectExForDpi(&bounds,WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX,FALSE,0,dpi);
    window=CreateWindowExW(WS_EX_CONTROLPARENT|WS_EX_COMPOSITED,cls.lpszClassName,uninstalling?L"Super Monkey Ball Deluxe — Uninstall":L"Super Monkey Ball Deluxe — Setup",WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX|WS_CLIPCHILDREN,CW_USEDEFAULT,CW_USEDEFAULT,bounds.right-bounds.left,bounds.bottom-bounds.top,nullptr,nullptr,cls.hInstance,nullptr);
    checked(window!=nullptr,"Cannot open the installer");
    const auto initial=uninstalling?Defaults{uninstall_root,{},true,false}:defaults();
    source_edit=control(L"EDIT",initial.iso.c_str(),ES_AUTOHSCROLL|WS_BORDER|WS_TABSTOP,110,{form_left,172,form_right-116,206});
    const auto browse_iso=control(L"BUTTON",L"Browse…",BS_OWNERDRAW|WS_TABSTOP,101,{form_right-104,172,form_right,206});
    const auto& folder=initial.folder;
    destination_edit=control(L"EDIT",folder.c_str(),ES_AUTOHSCROLL|WS_BORDER|WS_TABSTOP,111,{form_left,286,form_right-116,320});
    const auto browse_folder=control(L"BUTTON",L"Browse…",BS_OWNERDRAW|WS_TABSTOP,102,{form_right-104,286,form_right,320});
    copy_check=checkbox(L"Keep a copy of my ISO with the game (uses additional disk space)",112,checkbox_rows[0]);SendMessageW(copy_check,BM_SETCHECK,initial.copy?BST_CHECKED:BST_UNCHECKED,0);
    desktop_check=checkbox(L"Create a desktop shortcut",113,checkbox_rows[1]);SendMessageW(desktop_check,BM_SETCHECK,initial.desktop?BST_CHECKED:BST_UNCHECKED,0);
    progress_bar=control(PROGRESS_CLASSW,L"",PBS_SMOOTH,114,{status_left,status_top+84,status_right,status_top+104});SetWindowTheme(progress_bar,L"",L"");
    SendMessageW(progress_bar,PBM_SETRANGE32,0,100);SendMessageW(progress_bar,PBM_SETBARCOLOR,0,yellow);SendMessageW(progress_bar,PBM_SETBKCOLOR,0,brown);
    install_button=control(L"BUTTON",uninstalling?L"Uninstall":L"Install",BS_OWNERDRAW|WS_TABSTOP|WS_DISABLED,103,{620,buttons_top,748,buttons_top+38});
    close_button=control(L"BUTTON",L"Close",BS_OWNERDRAW|WS_TABSTOP,104,{760,buttons_top,848,buttons_top+38});
    inputs={source_edit,browse_iso,destination_edit,browse_folder,copy_check,desktop_check};position_controls();update_install_button();
    if(uninstalling) for(const auto input:inputs) ShowWindow(input,SW_HIDE);
    SetTimer(window,1,100,nullptr);
    if(!preview.empty()) {SendMessageW(progress_bar,PBM_SETPOS,state.percent,0);capture_preview(preview);DestroyWindow(window);return 0;}
    ShowWindow(window,SW_SHOWNORMAL);UpdateWindow(window);SetFocus(uninstalling?install_button:source_edit);
    MSG message{};while(GetMessageW(&message,nullptr,0,0)>0) if(!IsDialogMessageW(window,&message)) {TranslateMessage(&message);DispatchMessageW(&message);}
    if(state.worker.joinable()) state.worker.join();DestroyIcon(logo_icon);DeleteObject(edit_brush);DeleteObject(font);DeleteObject(heading);DeleteObject(font_small);CoUninitialize();return 0;
}
}
int wmain(int argc,wchar_t** argv) {
    // Setup logs go to the user's temporary folder, not beside the downloaded program.
    {std::error_code error;const auto temporary=std::filesystem::temp_directory_path(error);if(!error) smbd::initialize_diagnostics(temporary/"smbd-setup-logs");}
    bool interactive=argc==1;
    try {
        if((argc==3 || (argc==5 && std::wstring_view(argv[3])==L"--dpi")) && (std::wstring_view(argv[1])==L"--preview" || std::wstring_view(argv[1])==L"--preview-progress")) {
            if(argc==5) {preview_dpi=unsigned(std::stoul(argv[4]));if(preview_dpi<96 || preview_dpi>384) throw std::runtime_error("Preview DPI must be 96..384");}
            if(std::wstring_view(argv[1])==L"--preview-progress") {state.running=true;state.stage=3;state.percent=47;state.phase=L"Copying your game image";started=GetTickCount64();}
            return wizard(argv[2]);
        }
        if(argc>=4 && std::wstring_view(argv[1])==L"--silent-install") {
            bool copy=true,registration=true;
            for(int i=4;i<argc;++i) {
                const auto option=std::wstring_view(argv[i]);
                if(option==L"--reference-disc") copy=false;
                else if(option==L"--no-register") registration=false;
                else if(option==L"--cancel-after-percent" && i+1<argc) {
                    cancellation_percent=unsigned(std::stoul(argv[++i]));
                    if(cancellation_percent>100) throw std::runtime_error("Cancellation percentage must be 0..100");
                } else throw std::runtime_error("Unknown installer option");
            }
            install(argv[2],argv[3],copy,std::nullopt,registration);return 0;
        }
        std::vector<std::wstring> arguments(argv+1,argv+argc);
        // Opening the installed Uninstall.exe directly starts uninstall, not a second installation.
        const auto self=module_path();
        if(arguments.empty() && same_path(self.filename(),L"Uninstall.exe") && std::filesystem::exists(self.parent_path()/"install-manifest.txt"))
            arguments={L"--uninstall",self.parent_path().native()};
        if(arguments.size()>=2 && arguments[0]==L"--uninstall") {
            uninstall_root=folder_path(arguments[1]);
            bool silent=false;DWORD parent_id=0;
            for(std::size_t i=2;i<arguments.size();++i) {
                if(arguments[i]==L"--silent") silent=true;
                else if(arguments[i]==L"--wait" && i+1<arguments.size()) parent_id=DWORD(std::stoul(arguments[++i]));
                else throw std::runtime_error("Unknown uninstaller option");
            }
            interactive=!silent;
            remove_stale_uninstallers();
            if(same_path(self.parent_path(),uninstall_root)) {
                // Windows cannot delete a running program: a temporary copy removes this one.
                // A silent request therefore returns once that copy has started.
                const auto temporary=std::filesystem::temp_directory_path()/std::format("smbd-uninstall-{}.exe",GetCurrentProcessId());
                checked(CopyFileW(self.c_str(),temporary.c_str(),FALSE)!=FALSE,"Cannot start the uninstaller");
                auto parameters=L"--uninstall \""+uninstall_root.native()+L"\" --wait "+std::to_wstring(GetCurrentProcessId());
                if(silent) parameters+=L" --silent";
                checked(reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr,L"open",temporary.c_str(),parameters.c_str(),nullptr,silent?SW_HIDE:SW_SHOWNORMAL))>32,"Cannot start the uninstaller");return 0;
            }
            if(parent_id) {
                const auto parent=OpenProcess(SYNCHRONIZE,FALSE,parent_id);if(parent) {WaitForSingleObject(parent,10000);CloseHandle(parent);}
            }
            if(silent) {uninstall(uninstall_root);return 0;}
            uninstalling=true;
        } else if(!arguments.empty()) throw std::runtime_error("Unknown installer arguments");
        return wizard();
    } catch(const std::exception& error) {
        smbd::diagnostic_record("{\"type\":\"install_error\",\"message\":"+smbd::json(error.what())+'}');
        if(interactive) MessageBoxW(nullptr,smbd::wide(error.what()).c_str(),title,MB_OK|MB_ICONERROR);
        return 1;
    }
}
