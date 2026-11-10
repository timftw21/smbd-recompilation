.intel_syntax noprefix
.text

# FNSAVE/FRSTOR use the 108-byte environment in 64-bit mode without a 66 prefix.
# Do not restore XMM registers: the surrounding C++ owns the host ABI registers.
.globl smbd_fp_enter
.type smbd_fp_enter,@function
smbd_fp_enter:
    fnsave [rdx]
    stmxcsr dword ptr [rdx+108]
    frstor [rcx]
    ldmxcsr dword ptr [rcx+108]
    ret
.size smbd_fp_enter, .-smbd_fp_enter

.globl smbd_fp_leave
.type smbd_fp_leave,@function
smbd_fp_leave:
    fnsave [rcx]
    stmxcsr dword ptr [rcx+108]
    frstor [rdx]
    ldmxcsr dword ptr [rdx+108]
    ret
.size smbd_fp_leave, .-smbd_fp_leave

# Diagnostic snapshot preserves the resident state, including all 80-bit values.
.globl smbd_fp_snapshot
.type smbd_fp_snapshot,@function
smbd_fp_snapshot:
    fnsave [rcx]
    frstor [rcx]
    ret
.size smbd_fp_snapshot, .-smbd_fp_snapshot

.macro atomic_memory symbol,operation,part
.globl \symbol
.type \symbol,@function
\symbol:
    mov eax,edx
    \operation [rcx],\part
    ret
.size \symbol, .-\symbol
.endm
atomic_memory smbd_exchange8,xchg,al
atomic_memory smbd_exchange16,xchg,ax
atomic_memory smbd_exchange32,xchg,eax

.macro atomic_add symbol,part
.globl \symbol
.type \symbol,@function
\symbol:
    mov eax,edx
    lock xadd [rcx],\part
    ret
.size \symbol, .-\symbol
.endm
atomic_add smbd_fetch_add8,al
atomic_add smbd_fetch_add16,ax
atomic_add smbd_fetch_add32,eax

.macro atomic_compare symbol,part
.globl \symbol
.type \symbol,@function
\symbol:
    mov eax,edx
    lock cmpxchg [rcx],\part
    ret
.size \symbol, .-\symbol
.endm
atomic_compare smbd_compare_exchange8,r8b
atomic_compare smbd_compare_exchange16,r8w
atomic_compare smbd_compare_exchange32,r8d
.section .note.GNU-stack,"",@progbits
