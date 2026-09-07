/* WDK 7.1 declares these MSVC intrinsics but Clang does not lower them.
 * Keep the WDK signatures and implement the corresponding x86 instructions.
 * This file is used only by the Linux Clang kernel build. */
#include <ntddk.h>

unsigned char __inbyte(unsigned short port)
{
    unsigned char value;
    __asm__ volatile ("inb %w1, %b0" : "=a"(value) : "Nd"(port));
    return value;
}
unsigned short __inword(unsigned short port)
{
    unsigned short value;
    __asm__ volatile ("inw %w1, %w0" : "=a"(value) : "Nd"(port));
    return value;
}
unsigned long __indword(unsigned short port)
{
    unsigned long value;
    __asm__ volatile ("inl %w1, %k0" : "=a"(value) : "Nd"(port));
    return value;
}
void __outbyte(unsigned short port, unsigned char value)
{
    __asm__ volatile ("outb %b0, %w1" : : "a"(value), "Nd"(port));
}
void __outword(unsigned short port, unsigned short value)
{
    __asm__ volatile ("outw %w0, %w1" : : "a"(value), "Nd"(port));
}
void __outdword(unsigned short port, unsigned long value)
{
    __asm__ volatile ("outl %k0, %w1" : : "a"(value), "Nd"(port));
}
#if defined(_AMD64_)
unsigned __int64 __readcr8(void)
{
    unsigned __int64 value;
    __asm__ volatile ("mov %%cr8, %0" : "=r"(value) : : "memory");
    return value;
}
void __writecr8(unsigned __int64 value)
{
    __asm__ volatile ("mov %0, %%cr8" : : "r"(value) : "memory");
}
#endif
