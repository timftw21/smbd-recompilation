#include "diagnostics.h"
#if defined(__x86_64__)
#include "cpu.h"
#endif
#include "file.h"
#include "target.h"
#include <array>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <ctime>
#include <format>
#include <fcntl.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#include <pthread.h>
#include <sys/ucontext.h>
#else
#include <sys/syscall.h>
#include <ucontext.h>
#endif
#include <unistd.h>
namespace smbd {
namespace {
std::filesystem::path executable_path() {
    std::array<char,32768> path{};
#ifdef __APPLE__
    auto size=static_cast<std::uint32_t>(path.size());
    if(_NSGetExecutablePath(path.data(),&size)) throw std::runtime_error("Executable path exceeds diagnostic limit");
#else
    const auto size=readlink("/proc/self/exe",path.data(),path.size()-1);
    if(size<=0 || static_cast<std::size_t>(size)>=path.size()-1) throw std::runtime_error("Cannot locate executable for diagnostics");
#endif
    return std::filesystem::path(path.data());
}
int log_file=-1;std::filesystem::path log_path;
thread_local const Cpu* guest=nullptr;
std::atomic_flag handling=ATOMIC_FLAG_INIT;
alignas(16) thread_local std::array<std::byte,64*1024> signal_stack;
#ifdef __APPLE__
thread_local std::uint64_t diagnostic_thread=0;
#endif
void install_signal_stack() noexcept {
    thread_local bool attempted=false;if(attempted) return;attempted=true;
#ifdef __APPLE__
    pthread_threadid_np(nullptr,&diagnostic_thread);
#endif
    stack_t stack{};stack.ss_sp=signal_stack.data();stack.ss_size=signal_stack.size();sigaltstack(&stack,nullptr);
}
void append(std::string_view text) noexcept {
    if(log_file<0) return;
    while(!text.empty()) {const auto size=write(log_file,text.data(),text.size());if(size<=0) {if(size<0 && errno==EINTR) continue;break;}text.remove_prefix(size);}
}
void shutdown() {append("{\"type\":\"session_end\"}\n");if(log_file>=0) close(log_file);log_file=-1;}
void fatal(int signal,siginfo_t* info,void* context) {
    if(handling.test_and_set()) _exit(128+signal);
    // Extraction also builds on hosts whose native CPU backend is not ready.
#if defined(__x86_64__)
    const bool cpu_available=guest!=nullptr;
    const auto eip=guest?guest->eip:0,flags=guest?guest->flags:0;
#else
    const bool cpu_available=false;
    constexpr std::uint32_t eip=0,flags=0;
#endif
#ifdef __APPLE__
    const auto thread=diagnostic_thread;
#if defined(__aarch64__)
    const auto address=static_cast<ucontext_t*>(context)->uc_mcontext->__ss.__pc;
#else
    const auto address=static_cast<ucontext_t*>(context)->uc_mcontext->__ss.__rip;
#endif
#else
    const auto thread=static_cast<std::uint64_t>(syscall(SYS_gettid));
    const auto& registers=static_cast<ucontext_t*>(context)->uc_mcontext.gregs;
    const auto address=registers[REG_RIP];
#endif
    // All fields have fixed bounds: at most two 20-digit decimals and 48 hex digits.
    char buffer[320];char* next=buffer;
    const auto literal=[&](std::string_view chars) {for(char c:chars) *next++=c;};
    const auto decimal=[&](std::uint64_t number) {
        char digits[20];auto end=digits+sizeof(digits),first=end;
        do {*--first=char('0'+number%10);number/=10;} while(number);
        while(first!=end) *next++=*first++;
    };
    const auto hex=[&](std::uint64_t number,unsigned digits) {
        while(digits) *next++="0123456789ABCDEF"[(number>>(--digits*4))&15];
    };
    literal("{\"type\":\"native_crash\",\"signal\":");decimal(signal);
    literal(",\"thread\":");decimal(thread);
    literal(",\"fault_address\":\"0x");hex(reinterpret_cast<std::uintptr_t>(info->si_addr),16);
    literal("\",\"native_address\":\"0x");hex(address,16);
    literal("\",\"guest_available\":");literal(cpu_available?"true":"false");
    literal(",\"guest_eip\":\"0x");hex(eip,8);literal("\",\"flags\":\"0x");hex(flags,8);literal("\"}\n");
    append({buffer,static_cast<std::size_t>(next-buffer)});
    // The handler is reset before delivery, preserving the native core dump.
    raise(signal);
}
}
void initialize_diagnostics() noexcept {
    try {initialize_diagnostics(executable_path().parent_path()/"logs");} catch(...) {}
}
void initialize_diagnostics(const std::filesystem::path& directory) noexcept {
    if(log_file>=0) return;
    try {
        std::filesystem::create_directories(directory);
        const auto now=std::time(nullptr);tm utc{};gmtime_r(&now,&utc);
        log_path=directory/std::format("smbd-{:04}{:02}{:02}-{:02}{:02}{:02}-{}.log",utc.tm_year+1900,utc.tm_mon+1,utc.tm_mday,utc.tm_hour,utc.tm_min,utc.tm_sec,getpid());
        log_file=open(log_path.c_str(),O_WRONLY|O_APPEND|O_CREAT|O_EXCL|O_CLOEXEC,0600);if(log_file<0) return;
        std::atexit(shutdown);install_signal_stack();
        struct sigaction action{};action.sa_sigaction=fatal;action.sa_flags=SA_SIGINFO|SA_ONSTACK|SA_RESETHAND;sigemptyset(&action.sa_mask);
        for(int signal:{SIGSEGV,SIGBUS,SIGILL,SIGFPE,SIGABRT}) sigaction(signal,&action,nullptr);
#if defined(__APPLE__) && defined(__aarch64__)
        constexpr auto platform="macos-arm64";
#elif defined(__APPLE__)
        constexpr auto platform="macos-x86_64";
#else
        constexpr auto platform="linux-x86_64";
#endif
        diagnostic_record(std::format("{{\"type\":\"session_start\",\"pid\":{},\"platform\":{},\"target_sha256\":{}}}",getpid(),json(platform),json(target::sha256)));
    } catch(...) {}
}
void diagnostic_record(std::string_view text) noexcept {
    if(text.size()>256*1024) {append("{\"type\":\"log_error\",\"message\":\"Record exceeds 256 KiB\"}\n");return;}
    append(text);append("\n");
}
void diagnostic_guest(const Cpu* cpu) noexcept {if(cpu) install_signal_stack();guest=cpu;}
const std::filesystem::path& diagnostic_log() noexcept {return log_path;}
void diagnostic_watch(unsigned,void*) {throw std::runtime_error("Hardware memory watches are Windows-only; use a native debugger.");}
std::string check_memory_watch(bool) {throw std::runtime_error("Hardware memory-watch diagnostics are Windows-only.");}
}
