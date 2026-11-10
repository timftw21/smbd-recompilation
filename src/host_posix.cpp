#include "host.h"
#include <algorithm>
#include <array>
#include <cerrno>
#include <climits>
#include <cstring>
#include <exception>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <fcntl.h>
#include <linux/if_arp.h>
#include <openssl/rand.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/xattr.h>
#include <ucontext.h>
#include <unistd.h>

namespace smbd {
namespace {
thread_local DWORD last_error=0;
DWORD error_code(int value) {
    switch(value) {
    case 0:return 0;case ENOENT:return ERROR_FILE_NOT_FOUND;case ENOTDIR:return ERROR_PATH_NOT_FOUND;
    case EACCES:case EPERM:case ELOOP:case EISDIR:return ERROR_ACCESS_DENIED;case EEXIST:return ERROR_ALREADY_EXISTS;
    case ENOSPC:case EDQUOT:return ERROR_DISK_FULL;case EINVAL:case EOVERFLOW:return ERROR_INVALID_PARAMETER;
    case ENOTEMPTY:return ERROR_DIR_NOT_EMPTY;case EROFS:return ERROR_WRITE_PROTECT;
    case ENOMEM:return ERROR_NOT_ENOUGH_MEMORY;case ECANCELED:return ERROR_OPERATION_ABORTED;
    case EINPROGRESS:return ERROR_IO_INCOMPLETE;default:return 31; // ERROR_GEN_FAILURE; reported by the guest service boundary.
    }
}
BOOL failed(int error=errno) {last_error=error_code(error);return FALSE;}
struct Handle {
    int fd=-1;bool delete_on_close=false;DWORD access=0,share=0;
    std::filesystem::path path;
};
// Only the cooperative guest thread opens/closes these; AIO uses descriptors.
thread_local std::vector<Handle*> files;
Handle& handle(HANDLE value) {return *static_cast<Handle*>(value);}
constexpr std::int64_t epoch=11644473600LL;
FILETIME timestamp(timespec value) {
    const auto ticks=std::uint64_t(value.tv_sec+epoch)*10000000+value.tv_nsec/100;
    return {DWORD(ticks),DWORD(ticks>>32)};
}
timespec timestamp(LARGE_INTEGER value) {
    return {value.QuadPart/10000000-epoch,(value.QuadPart%10000000)*100};
}
DWORD attributes(const struct stat& value) {
    return (S_ISDIR(value.st_mode)?FILE_ATTRIBUTE_DIRECTORY:FILE_ATTRIBUTE_NORMAL)|
        (S_ISLNK(value.st_mode)?FILE_ATTRIBUTE_REPARSE_POINT:0)|((value.st_mode&0222)?0:FILE_ATTRIBUTE_READONLY);
}
FILETIME creation_time(int fd,const char* path,const struct stat& status) {
    std::uint64_t saved=0;
    const auto size=fd>=0?fgetxattr(fd,"user.smbd.creation_time",&saved,sizeof(saved)):getxattr(path,"user.smbd.creation_time",&saved,sizeof(saved));
    if(size==sizeof(saved)) return {DWORD(saved),DWORD(saved>>32)};
    struct statx value{};
    if(!statx(fd>=0?fd:AT_FDCWD,fd>=0?"":path,fd>=0?AT_EMPTY_PATH:AT_SYMLINK_NOFOLLOW,STATX_BTIME,&value) && (value.stx_mask&STATX_BTIME))
        return timestamp(timespec{value.stx_btime.tv_sec,value.stx_btime.tv_nsec});
    return timestamp(status.st_ctim);
}
struct Fiber {
    ucontext_t context{};
    alignas(16) std::array<std::byte,512> floating{};
    void* stack=nullptr;std::size_t size=0;void(*entry)(void*)=nullptr;void* argument=nullptr;
    ~Fiber(){if(stack) munmap(stack,size);}
};
thread_local Fiber* current_fiber=nullptr;
void fiber_entry() {
    auto* fiber=current_fiber;fiber->entry(fiber->argument);
    std::terminate(); // Guest thread exit must explicitly return to the scheduler.
}
}
DWORD GetLastError() {return last_error;}
HANDLE CreateFileW(const char* path,DWORD access,DWORD share,void*,DWORD mode,DWORD flags,HANDLE) {
    const auto normalized=std::filesystem::absolute(path).lexically_normal();
    constexpr auto read_mask=GENERIC_READ|GENERIC_EXECUTE|GENERIC_ALL|FILE_READ_DATA|FILE_EXECUTE;
    constexpr auto write_mask=GENERIC_WRITE|GENERIC_ALL|FILE_WRITE_DATA|FILE_APPEND_DATA;
    constexpr auto delete_mask=DELETE|GENERIC_ALL;
    const auto wants_read=bool(access&read_mask),wants_write=bool(access&write_mask);
    struct stat target_status{};const bool exists=!lstat(path,&target_status);
    for(const auto* file:files) {
        struct stat open_status{};
        if(file->path!=normalized && !(exists && !fstat(file->fd,&open_status) &&
           open_status.st_dev==target_status.st_dev && open_status.st_ino==target_status.st_ino)) continue;
        if((wants_read && !(file->share&FILE_SHARE_READ)) || (wants_write && !(file->share&FILE_SHARE_WRITE)) ||
           ((access&delete_mask) && !(file->share&FILE_SHARE_DELETE)) ||
           ((file->access&read_mask) && !(share&FILE_SHARE_READ)) ||
           ((file->access&write_mask) && !(share&FILE_SHARE_WRITE)) ||
           ((file->access&delete_mask) && !(share&FILE_SHARE_DELETE))) {
            last_error=ERROR_SHARING_VIOLATION;return INVALID_HANDLE_VALUE;
        }
    }
    int options=(wants_write?(wants_read?O_RDWR:O_WRONLY):O_RDONLY)|O_CLOEXEC|O_NOFOLLOW;
    if((access&FILE_APPEND_DATA) && !(access&(GENERIC_ALL|GENERIC_WRITE|FILE_WRITE_DATA))) options|=O_APPEND;
    if((flags&FILE_FLAG_BACKUP_SEMANTICS) && exists && S_ISDIR(target_status.st_mode)) {
        // Directory contents are changed through namespace operations, not
        // read/write on the directory descriptor. Retain the guest's access
        // mask for permission/share checks and use a native directory handle.
        options=O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW;
    } else switch(mode) {
    case CREATE_NEW:options|=O_CREAT|O_EXCL;break;case CREATE_ALWAYS:options|=O_CREAT|O_TRUNC;break;
    case OPEN_ALWAYS:options|=O_CREAT;break;case TRUNCATE_EXISTING:options|=O_TRUNC;break;
    case OPEN_EXISTING:break;default:last_error=ERROR_INVALID_PARAMETER;return INVALID_HANDLE_VALUE;
    }
    if(flags&FILE_FLAG_WRITE_THROUGH) options|=O_SYNC;
    auto result=std::make_unique<Handle>(Handle{-1,bool(flags&FILE_FLAG_DELETE_ON_CLOSE),access,share,normalized});
    files.push_back(result.get());
    const auto fd=open(path,options,0666);
    if(fd<0) {failed();files.pop_back();return INVALID_HANDLE_VALUE;}
    result->fd=fd;
    if(flags&FILE_FLAG_RANDOM_ACCESS) posix_fadvise(fd,0,0,POSIX_FADV_RANDOM);
    if(flags&FILE_FLAG_SEQUENTIAL_SCAN) posix_fadvise(fd,0,0,POSIX_FADV_SEQUENTIAL);
    return result.release();
}
BOOL CloseHandle(HANDLE value) {
    if(!value || value==INVALID_HANDLE_VALUE) return FALSE;
    auto* item=static_cast<Handle*>(value);bool okay=true;
    if(item->delete_on_close) {
        struct stat status{};const bool directory=!fstat(item->fd,&status) && S_ISDIR(status.st_mode);
        if((directory?rmdir(item->path.c_str()):unlink(item->path.c_str())) && errno!=ENOENT) okay=failed();
    }
    if(close(item->fd)) okay=failed();
    std::erase(files,item);
    delete item;return okay;
}
DWORD GetFileAttributesW(const char* path) {
    struct stat value{};if(lstat(path,&value)) {failed();return INVALID_FILE_ATTRIBUTES;}return attributes(value);
}
BOOL GetFileAttributesExW(const char* path,DWORD,WIN32_FILE_ATTRIBUTE_DATA* output) {
    struct stat value{};if(lstat(path,&value)) return failed();
    *output={attributes(value),creation_time(-1,path,value),timestamp(value.st_atim),timestamp(value.st_mtim),DWORD(std::uint64_t(value.st_size)>>32),DWORD(value.st_size)};return TRUE;
}
BOOL CreateDirectoryW(const char* path,void*) {return mkdir(path,0777)?failed():TRUE;}
BOOL GetFileInformationByHandle(HANDLE file,BY_HANDLE_FILE_INFORMATION* output) {
    struct stat value{};if(fstat(handle(file).fd,&value)) return failed();
    *output={attributes(value),creation_time(handle(file).fd,nullptr,value),timestamp(value.st_atim),timestamp(value.st_mtim),DWORD(value.st_dev),
        DWORD(std::uint64_t(value.st_size)>>32),DWORD(value.st_size),DWORD(value.st_nlink),DWORD(std::uint64_t(value.st_ino)>>32),DWORD(value.st_ino)};return TRUE;
}
BOOL GetFileSizeEx(HANDLE file,LARGE_INTEGER* output) {
    struct stat value{};if(fstat(handle(file).fd,&value)) return failed();output->QuadPart=value.st_size;return TRUE;
}
BOOL SetFilePointerEx(HANDLE file,LARGE_INTEGER position,LARGE_INTEGER* output,DWORD method) {
    const auto result=lseek(handle(file).fd,position.QuadPart,method);
    if(result<0) return failed();if(output) output->QuadPart=result;return TRUE;
}
BOOL SetFileInformationByHandle(HANDLE file,FILE_INFO_BY_HANDLE_CLASS kind,void* data,DWORD) {
    auto& item=handle(file);
    if(kind==FileEndOfFileInfo) return ftruncate(item.fd,static_cast<FILE_END_OF_FILE_INFO*>(data)->EndOfFile.QuadPart)?failed():TRUE;
    if(kind==FileAllocationInfo) {
        const auto size=static_cast<FILE_ALLOCATION_INFO*>(data)->AllocationSize.QuadPart;
        struct stat status{};if(fstat(item.fd,&status)) return failed();
        if(size<status.st_size) return ftruncate(item.fd,size)?failed():TRUE;
        // Extending the allocation reserves storage without extending EOF.
        return size && fallocate(item.fd,FALLOC_FL_KEEP_SIZE,0,size)?failed():TRUE;
    }
    if(kind==FileDispositionInfo) {
        for(const auto* other:files) if(other!=&item && other->path==item.path && !(other->share&FILE_SHARE_DELETE)) {
            last_error=ERROR_SHARING_VIOLATION;return FALSE;
        }
        item.delete_on_close=static_cast<FILE_DISPOSITION_INFO*>(data)->DeleteFile;return TRUE;
    }
    auto& value=*static_cast<FILE_BASIC_INFO*>(data);
    // Linux birth/change timestamps cannot be set through futimens. Preserve
    // explicitly supplied Xbox metadata on the save itself, across reopening.
    if(value.CreationTime.QuadPart>0 && fsetxattr(item.fd,"user.smbd.creation_time",&value.CreationTime.QuadPart,8,0)) return failed();
    if(value.ChangeTime.QuadPart>0 && fsetxattr(item.fd,"user.smbd.change_time",&value.ChangeTime.QuadPart,8,0)) return failed();
    std::array<timespec,2> times{{{0,UTIME_OMIT},{0,UTIME_OMIT}}};
    if(value.LastAccessTime.QuadPart>0) times[0]=timestamp(value.LastAccessTime);
    if(value.LastWriteTime.QuadPart>0) times[1]=timestamp(value.LastWriteTime);
    if(futimens(item.fd,times.data())) return failed();
    if(value.FileAttributes) {
        struct stat status{};if(fstat(item.fd,&status)) return failed();
        const auto mode=value.FileAttributes&FILE_ATTRIBUTE_READONLY?status.st_mode&~0222:status.st_mode|S_IWUSR;
        if(fchmod(item.fd,mode)) return failed();
    }
    return TRUE;
}
BOOL GetDiskFreeSpaceExW(const char* path,ULARGE_INTEGER* available,ULARGE_INTEGER* total,ULARGE_INTEGER* free) {
    struct statvfs value{};if(statvfs(path,&value)) return failed();
    if(available) available->QuadPart=std::uint64_t(value.f_bavail)*value.f_frsize;
    if(total) total->QuadPart=std::uint64_t(value.f_blocks)*value.f_frsize;
    if(free) free->QuadPart=std::uint64_t(value.f_bfree)*value.f_frsize;return TRUE;
}
namespace {
BOOL transfer(HANDLE file,void* buffer,DWORD count,DWORD* transferred,OVERLAPPED* overlap,bool write) {
    if(overlap) {
        auto& io=overlap->io;io={};io.aio_fildes=handle(file).fd;io.aio_buf=buffer;io.aio_nbytes=count;
        io.aio_offset=(std::uint64_t(overlap->OffsetHigh)<<32)|overlap->Offset;io.aio_sigevent.sigev_notify=SIGEV_NONE;
        if((write?aio_write(&io):aio_read(&io))) return failed();
        overlap->submitted=true;last_error=ERROR_IO_PENDING;return FALSE;
    }
    ssize_t result;do {result=write ? ::write(handle(file).fd,buffer,count) : ::read(handle(file).fd,buffer,count);} while(result<0 && errno==EINTR);
    if(result<0) return failed();if(transferred) *transferred=DWORD(result);return TRUE;
}
}
BOOL ReadFile(HANDLE file,void* buffer,DWORD count,DWORD* transferred,OVERLAPPED* overlap) {return transfer(file,buffer,count,transferred,overlap,false);}
BOOL WriteFile(HANDLE file,const void* buffer,DWORD count,DWORD* transferred,OVERLAPPED* overlap) {return transfer(file,const_cast<void*>(buffer),count,transferred,overlap,true);}
BOOL GetOverlappedResult(HANDLE,OVERLAPPED* overlap,DWORD* transferred,BOOL wait) {
    int error;
    while((error=aio_error(&overlap->io))==EINPROGRESS) {
        if(!wait) return failed(error);
        const aiocb* list[]={&overlap->io};if(aio_suspend(list,1,nullptr) && errno!=EINTR) return failed();
    }
    if(!overlap->submitted) {last_error=ERROR_NOT_FOUND;return FALSE;}
    const auto result=aio_return(&overlap->io);overlap->submitted=false;
    if(error) return failed(error);if(result<0) return failed();*transferred=DWORD(result);return TRUE;
}
BOOL CancelIoEx(HANDLE file,OVERLAPPED* overlap) {
    if(!overlap->submitted) {last_error=ERROR_NOT_FOUND;return FALSE;}
    return aio_cancel(handle(file).fd,&overlap->io)<0?failed():TRUE;
}
BOOL FlushFileBuffers(HANDLE file) {return fsync(handle(file).fd)?failed():TRUE;}
void GetSystemTimeAsFileTime(FILETIME* output) {timespec now{};clock_gettime(CLOCK_REALTIME,&now);*output=timestamp(now);}
BOOL FileTimeToSystemTime(const FILETIME* input,SYSTEMTIME* output) {
    const auto ticks=(std::uint64_t(input->dwHighDateTime)<<32)|input->dwLowDateTime;
    if(ticks>INT64_MAX) return FALSE;
    const time_t seconds=std::int64_t(ticks/10000000)-epoch;tm value{};if(!gmtime_r(&seconds,&value)) return FALSE;
    *output={WORD(value.tm_year+1900),WORD(value.tm_mon+1),WORD(value.tm_wday),WORD(value.tm_mday),WORD(value.tm_hour),WORD(value.tm_min),WORD(value.tm_sec),WORD(ticks%10000000/10000)};return TRUE;
}
BOOL SystemTimeToFileTime(const SYSTEMTIME* input,FILETIME* output) {
    if(input->wYear<1601 || input->wMonth<1 || input->wMonth>12 || input->wDay<1 || input->wDay>31 || input->wHour>23 || input->wMinute>59 || input->wSecond>59 || input->wMilliseconds>999) return FALSE;
    tm value{};value.tm_year=input->wYear-1900;value.tm_mon=input->wMonth-1;value.tm_mday=input->wDay;
    value.tm_hour=input->wHour;value.tm_min=input->wMinute;value.tm_sec=input->wSecond;
    const auto seconds=timegm(&value);
    if(value.tm_year!=input->wYear-1900 || value.tm_mon!=input->wMonth-1 || value.tm_mday!=input->wDay) return FALSE;
    *output=timestamp(timespec{seconds,long(input->wMilliseconds)*1000000});return TRUE;
}
void Sleep(DWORD milliseconds) {
    timespec duration{milliseconds/1000,long(milliseconds%1000)*1000000};while(nanosleep(&duration,&duration) && errno==EINTR) {}
}
void* CreateFiberEx(std::size_t,std::size_t size,DWORD,void(*entry)(void*),void* argument) {
    auto fiber=std::make_unique<Fiber>();const auto page=std::size_t(sysconf(_SC_PAGESIZE));
    size=(size+page-1)&~(page-1);fiber->size=size+2*page;
    fiber->stack=mmap(nullptr,fiber->size,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if(fiber->stack==MAP_FAILED) {fiber->stack=nullptr;return nullptr;}
    auto* usable=static_cast<std::byte*>(fiber->stack)+page;
    if(mprotect(usable,size,PROT_READ|PROT_WRITE)) return nullptr;
    if(getcontext(&fiber->context)) return nullptr;
    fiber->context.uc_stack={usable,0,size};fiber->entry=entry;fiber->argument=argument;
    __asm__ volatile("fxsave64 %0":"=m"(fiber->floating));
    makecontext(&fiber->context,fiber_entry,0);
    return fiber.release();
}
void DeleteFiber(void* fiber) {delete static_cast<Fiber*>(fiber);}
void* ConvertThreadToFiberEx(void*,DWORD) {
    if(current_fiber) return nullptr;current_fiber=new Fiber;
    __asm__ volatile("fxsave64 %0":"=m"(current_fiber->floating));
    return current_fiber;
}
BOOL ConvertFiberToThread() {if(!current_fiber || current_fiber->stack) return FALSE;delete current_fiber;current_fiber=nullptr;return TRUE;}
BOOL IsThreadAFiber() {return current_fiber!=nullptr;}
void* GetCurrentFiber() {return current_fiber;}
void SwitchToFiber(void* target) {
    auto* previous=current_fiber;auto* next=static_cast<Fiber*>(target);
    __asm__ volatile("fxsave64 %0":"=m"(previous->floating));
    current_fiber=next;
    if(swapcontext(&previous->context,&next->context)) std::terminate();
    __asm__ volatile("fxrstor64 %0"::"m"(previous->floating));
}
std::filesystem::path executable_path() {
    std::array<char,32768> path{};const auto length=readlink("/proc/self/exe",path.data(),path.size());
    if(length<0 || std::size_t(length)==path.size()) throw std::runtime_error("Cannot locate executable");return std::string(path.data(),length);
}
void random_bytes(void* output,std::size_t size) {
    if(size>INT_MAX || RAND_bytes(static_cast<unsigned char*>(output),static_cast<int>(size))!=1) throw std::runtime_error("Native random source failed");
}
std::uint32_t ethernet_status() {
    for(const auto& entry:std::filesystem::directory_iterator("/sys/class/net")) {
        const auto path=entry.path();unsigned type=0,carrier=0,speed=0;
        std::ifstream(path/"type")>>type;if(type!=ARPHRD_ETHER || !std::filesystem::exists(path/"device")) continue;
        std::ifstream(path/"carrier")>>carrier;if(!carrier) continue;
        std::ifstream(path/"speed")>>speed;return 1|(speed>=100?2U:speed>=10?4U:0U);
    }
    return 0;
}
std::byte* reserve_virtual_pages(std::size_t size) {
    auto* address=mmap(nullptr,size,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    return address==MAP_FAILED?nullptr:static_cast<std::byte*>(address);
}
bool commit_virtual_pages(std::byte* address,std::size_t size) {
    return mprotect(address,size,PROT_READ|PROT_WRITE)==0;
}
bool decommit_virtual_pages(std::byte* address,std::size_t size) {
    return mmap(address,size,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED,-1,0)!=MAP_FAILED;
}
void release_virtual_pages(std::byte* address,std::size_t size) {munmap(address,size);}
std::uint32_t dos_error(std::uint32_t status) {
    // Verified against RtlNtStatusToDosError on the Windows host for every
    // NTSTATUS returned by these Xbox services.
    switch(status) {
    case 0:return 0;case 0xC0000008:return 6;case 0xC000000D:return 87;case 0xC0000017:return 8;
    case 0xC0000022:return 5;case 0xC0000023:return 122;case 0xC0000034:return 2;case 0xC0000035:return 183;
    case 0xC000003A:return 3;case 0xC0000043:return 32;case 0xC000007F:return 112;case 0xC0000011:return 38;
    case 0xC00000A2:return 19;case 0xC0000101:return 145;case 0xC0000120:return 995;case 0x102:return 1460;
    case 0x103:return 997;case 0x80000005:return 234;case 0x80000006:return 18;
    case 0xC0000013:return 21;case 0xC0000018:return 487;case 0xC0000024:return 6;case 0xC0000040:return 8;
    case 0xC0000047:return 298;case 0xC00000BA:return 5;case 0xC00000BB:return 50;case 0xC0000103:return 267;case 0xC0000106:return 206;
    default:throw std::runtime_error("NTSTATUS has no verified DOS error mapping: "+std::to_string(status));
    }
}
}
