// Copyright (c)2019 Black Sphere Studios
// For conditions of distribution and use, see copyright notice in innative.h

#include "innative/export.h"

#ifdef IN_PLATFORM_WIN32
#include "../innative/win32.h"
#elif defined(IN_PLATFORM_POSIX)
#include <unistd.h>
#include <sys/mman.h>
#else
#error unknown platform!
#endif

#ifdef IN_PLATFORM_WIN32
HANDLE heap     = 0;
DWORD heapcount = 0;
#elif defined(IN_PLATFORM_POSIX)
const int SYSCALL_WRITE  = 1;
const int SYSCALL_MMAP   = 9;
const int SYSCALL_MUNMAP = 11;
const int SYSCALL_MREMAP = 25;
const int SYSCALL_EXIT   = 60;
const int MREMAP_MAYMOVE = 1;

#ifdef IN_CPU_x86_64
IN_COMPILER_DLLEXPORT extern IN_COMPILER_NAKED void* _innative_syscall(size_t syscall_number, const void* p1, size_t p2,
                                                                       size_t p3, size_t p4, size_t p5, size_t p6)
{
  __asm volatile("movq %rdi, %rax\n\t"
                 "movq %rsi, %rdi\n\t"
                 "movq %rdx, %rsi\n\t"
                 "movq %rcx, %rdx\n\t"
                 "movq %r8, %r10\n\t"
                 "movq %r9, %r8\n\t"
                 "movq 8(%rsp), %r9\n\t"
                 "syscall\n\t"
                 "ret");
}
#elif defined(IN_CPU_x86)
#error unsupported architecture!
#elif defined(IN_CPU_ARM)
IN_COMPILER_DLLEXPORT extern IN_COMPILER_NAKED void* _innative_syscall(size_t syscall_number, const void* p1, size_t p2,
                                                                       size_t p3, size_t p4, size_t p5, size_t p6)
{
  __asm volatile("mov	ip, sp\n\t"
                 "push{ r4, r5, r6, r7 }\n\t"
                 "cfi_adjust_cfa_offset(16)\n\t"
                 "cfi_rel_offset(r4, 0)\n\t"
                 "cfi_rel_offset(r5, 4)\n\t"
                 "cfi_rel_offset(r6, 8)\n\t"
                 "cfi_rel_offset(r7, 12)\n\t"
                 "mov	r7, r0\n\t"
                 "mov	r0, r1\n\t"
                 "mov	r1, r2\n\t"
                 "mov	r2, r3\n\t"
                 "ldmfd	ip, { r3, r4, r5, r6 }\n\t"
                 "swi	0x0\n\t"
                 "pop{ r4, r5, r6, r7 }\n\t"
                 "cfi_adjust_cfa_offset(-16)\n\t"
                 "cfi_restore(r4)\n\t"
                 "cfi_restore(r5)\n\t"
                 "cfi_restore(r6)\n\t"
                 "cfi_restore(r7)\n\t"
                 "cmn	r0, #4096\n\t"
                 "it	cc\n\t"
#ifdef ARCH_HAS_BX
                 "bxcc lr\n\t"
#else
                 "movcc pc, lr\n\t"
#endif
  );
}
#else
#error unsupported architecture!
#endif
#endif

// Writes a buffer to the standard output using system calls
void _innative_internal_write_out(const void* buf, size_t num)
{
#ifdef IN_PLATFORM_WIN32
  DWORD out;
  WriteConsoleA(GetStdHandle(STD_OUTPUT_HANDLE), buf, (DWORD)num, &out, NULL);
#elif defined(IN_PLATFORM_POSIX)
  size_t cast = 1;
  _innative_syscall(SYSCALL_WRITE, (void*)cast, (size_t)buf, num, 0, 0, 0);
#else
#error unknown platform!
#endif
}

static const char lookup[16] = "0123456789ABCDEF";

IN_COMPILER_DLLEXPORT extern void _innative_internal_env_print(uint64_t n)
{
  int i = 0;
  do
  {
    // This is inefficient, but avoids using the stack (since i gets optimized out), which can segfault if other functions
    // are misbehaving.
    _innative_internal_write_out(&lookup[(n >> 60) & 0xF], 1);
    i++;
    n <<= 4;
  } while(i < 16);
  _innative_internal_write_out("\n", 1);
}

// Very simple memcpy implementation because we don't have access to the C library
IN_COMPILER_DLLEXPORT extern void _innative_internal_env_memcpy(char* dest, const char* src, uint64_t sz)
{
  // Align dest pointer
  while((size_t)dest % sizeof(uint64_t) && sz)
  {
    *dest = *src;
    dest += 1;
    src += 1;
    sz -= 1;
  }

  while(sz > sizeof(uint64_t))
  {
    *((uint64_t*)dest) = *((uint64_t*)src);
    dest += sizeof(uint64_t);
    src += sizeof(uint64_t);
    sz -= sizeof(uint64_t);
  }

  while(sz)
  {
    *dest = *src;
    dest += 1;
    src += 1;
    sz -= 1;
  }
}

// Platform-specific implementation of the mem.grow instruction, except it works in bytes
IN_COMPILER_DLLEXPORT extern void* _innative_internal_env_grow_memory(void* p, uint64_t i, uint64_t max)
{
  uint64_t* info = (uint64_t*)p;
  if(info != 0)
  {
    i += info[-1];
    if(max > 0 && i > max)
      return 0;
#ifdef IN_PLATFORM_WIN32
    info = HeapReAlloc(heap, HEAP_ZERO_MEMORY, info - 1, (SIZE_T)i + sizeof(uint64_t));
#elif defined(IN_PLATFORM_POSIX)
    info =
      _innative_syscall(SYSCALL_MREMAP, info - 1, info[-1] + sizeof(uint64_t), i + sizeof(uint64_t), MREMAP_MAYMOVE, 0, 0);
    if((void*)info >= (void*)0xfffffffffffff001) // This is a syscall error from -4095 to -1
      return 0;
#else
#error unknown platform!
#endif
  }
  else if(!max || i <= max)
  {
#ifdef IN_PLATFORM_WIN32
    if(!heap)
      heap = HeapCreate(0, (SIZE_T)i, 0);
    if(!heap)
      return 0;
    ++heapcount;
    info = HeapAlloc(heap, HEAP_ZERO_MEMORY, (SIZE_T)i + sizeof(uint64_t));
#elif defined(IN_PLATFORM_POSIX)
    info = _innative_syscall(SYSCALL_MMAP, NULL, i + sizeof(uint64_t), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS,
                             -1, 0);
    if((void*)info >= (void*)0xfffffffffffff001) // This is a syscall error from -4095 to -1
      return 0;
#else
#error unknown platform!
#endif
  }

  if((size_t)info % sizeof(uint64_t))
    i = i / 0; // Force error (we have no standard library so we can't abort)

  if(!info)
    return 0;
  info[0] = i;
  return info + 1;
}

// Platform-specific memory free, called by the exit function to clean up memory allocations
IN_COMPILER_DLLEXPORT extern void _innative_internal_env_free_memory(void* p)
{
  if(p)
  {
    uint64_t* info = (uint64_t*)p;

#ifdef IN_PLATFORM_WIN32
    HeapFree(heap, 0, info - 1);
    if(--heapcount == 0)
      HeapDestroy(heap);
#elif defined(IN_PLATFORM_POSIX)
    _innative_syscall(SYSCALL_MUNMAP, info - 1, info[-1], 0, 0, 0, 0);
#else
#error unknown platform!
#endif
  }
}

// You cannot return from the entry point of a program, you must instead call a platform-specific syscall to terminate it.
IN_COMPILER_DLLEXPORT extern void _innative_internal_env_exit(int status)
{
#ifdef IN_PLATFORM_WIN32
  ExitProcess(status);
#elif defined(IN_PLATFORM_POSIX)
  size_t cast = status;
  _innative_syscall(SYSCALL_EXIT, (void*)cast, 0, 0, 0, 0, 0);
#endif
}

IN_COMPILER_DLLEXPORT extern void _innative_internal_env_memdump(const unsigned char* mem, uint64_t sz)
{
  static const char prefix[] = "\n --- MEMORY DUMP ---\n\n";
  char buf[256];

  _innative_internal_write_out(prefix, sizeof(prefix));
  for(uint64_t i = 0; i < sz;)
  {
    uint64_t j;
    for(j = 0; j < (sizeof(buf) / 2) && i < sz; ++j, ++i)
    {
      buf[j * 2]     = lookup[(mem[i] & 0xF0) >> 4];
      buf[j * 2 + 1] = lookup[mem[i] & 0x0F];
    }
    _innative_internal_write_out(buf, (size_t)j * 2);
  }
  _innative_internal_write_out("\n", 1);
}

// This function exists only to test the _WASM_ C export code path
IN_COMPILER_DLLEXPORT extern void _innative_internal_WASM_print(int32_t a) { _innative_internal_env_print(a); }