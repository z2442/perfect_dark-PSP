#ifndef _IN_PLATFORM_H
#define _IN_PLATFORM_H


// detect OS
#if defined(_WIN32)
	#define PLATFORM_WIN32 1
#elif defined(__SWITCH__)
	// Nintendo Switch specific flags
	#define PLATFORM_POSIX 1     // Has POSIX-like APIs
	#define PLATFORM_NSWITCH 1
#elif defined(__PSP__)

#elif defined(__linux__)
	#define PLATFORM_POSIX 1
	#define PLATFORM_LINUX 1
#elif defined(__APPLE__)
	#define PLATFORM_POSIX 1
	#define PLATFORM_OSX 1
#elif defined(unix) || defined(__unix__) || defined(__unix) // More generic Unix check
	// Generic POSIX/Unix fallback
	#define PLATFORM_POSIX 1
	#warning "Unknown Unix OS assumed POSIX. Please add specific detection to platform.h if needed."
#else
	// Cannot determine OS, error out or provide a default
	#error "Unknown OS. Please add detection for it to platform.h."
#endif

// detect arch
#if defined(__x86_64__) || defined(_M_X64)
	#define PLATFORM_X86_64 1
	#define PLATFORM_64BIT 1
#elif defined(__i386__) || defined(_X86_) || defined(_M_IX86)
	#define PLATFORM_X86 1
	#define PLATFORM_32BIT 1 // Explicitly define 32bit if needed elsewhere
#elif defined(__aarch64__) || defined(_M_ARM64)
	#define PLATFORM_ARM64 1  // More specific name
	#define PLATFORM_ARM 8    // Keep PLATFORM_ARM for potential version checks
	#define PLATFORM_64BIT 1
#elif defined(__arm__) || defined(_M_ARM)
	// assume armv7 or similar 32-bit ARM
	#define PLATFORM_ARM 7
	#define PLATFORM_32BIT 1 // Explicitly define 32bit
#elif defined(__mips__) || defined(__mips) || defined(_MIPS_ARCH)
    // MIPS architecture (covers PSP)
	#define PLATFORM_MIPS 1
	#define PLATFORM_32BIT 1 // PSP is 32-bit
#else
	#error "Unknown CPU arch."
#endif

// detect endianness
// Standard GCC/Clang way (should work for psp-gcc)
#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__) && defined(__ORDER_BIG_ENDIAN__)
	#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
		#define PLATFORM_LITTLE_ENDIAN 1
	#elif __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
		#define PLATFORM_BIG_ENDIAN 1
	#else
		#error "Could not determine endianness using __BYTE_ORDER__."
	#endif
// Fallback for other compilers or older standards
#elif defined(__LITTLE_ENDIAN__) || defined(_LITTLE_ENDIAN) || defined(__ORDER_LITTLE_ENDIAN__) || \
      (defined(__mips__) && defined(__MIPSEL__)) || /* MIPS Little Endian (PSP) */ \
      defined(__i386__) || defined(_X86_) || defined(_M_IX86) || \
      defined(__x86_64__) || defined(_M_X64) || \
      defined(__arm__) || defined(_M_ARM) || defined(__aarch64__) || defined(_M_ARM64) // Assuming common ARM/x86 are LE
	#define PLATFORM_LITTLE_ENDIAN 1
#elif defined(__BIG_ENDIAN__) || defined(_BIG_ENDIAN) || defined(__ORDER_BIG_ENDIAN__) || \
      (defined(__mips__) && defined(__MIPSEB__)) /* MIPS Big Endian */
	#define PLATFORM_BIG_ENDIAN 1
#else
	#error "Could not determine endianness."
#endif

// Ensure only one endianness is defined
#if defined(PLATFORM_LITTLE_ENDIAN) && defined(PLATFORM_BIG_ENDIAN)
	#error "Both PLATFORM_LITTLE_ENDIAN and PLATFORM_BIG_ENDIAN are defined!"
#endif
#if !defined(PLATFORM_LITTLE_ENDIAN) && !defined(PLATFORM_BIG_ENDIAN)
	#error "Neither PLATFORM_LITTLE_ENDIAN nor PLATFORM_BIG_ENDIAN is defined!"
#endif


// byteswap macros
#if defined(__GNUC__) || defined(__clang__) // This covers psp-gcc
	#define PD_BSWAP16(x) __builtin_bswap16(x)
	#define PD_BSWAP32(x) __builtin_bswap32(x)
	#define PD_BSWAP64(x) __builtin_bswap64(x)
#elif defined(_MSC_VER) // Microsoft Visual C++
    #include <stdlib.h>
    #define PD_BSWAP16(x) _byteswap_ushort(x)
    #define PD_BSWAP32(x) _byteswap_ulong(x)
    #define PD_BSWAP64(x) _byteswap_uint64(x)
#else
	#error "Implement PD_BSWAP macros for your compiler."
#endif

// Endian conversion macros based on detected platform endianness
#ifdef PLATFORM_BIG_ENDIAN
	#define PD_BE16(x) (x)
	#define PD_BE32(x) (x)
	#define PD_BE64(x) (x)
	#define PD_LE16(x) PD_BSWAP16(x)
	#define PD_LE32(x) PD_BSWAP32(x)
	#define PD_LE64(x) PD_BSWAP64(x)
#else // PLATFORM_LITTLE_ENDIAN is defined
	#define PD_BE16(x) PD_BSWAP16(x)
	#define PD_BE32(x) PD_BSWAP32(x)
	#define PD_BE64(x) PD_BSWAP64(x)
	#define PD_LE16(x) (x)
	#define PD_LE32(x) (x)
	#define PD_LE64(x) (x)
#endif

// TODO: use uintptr_t or something more robust based on compiler defines
#ifdef PLATFORM_64BIT
	#define PD_BEPTR(x) PD_BE64((uint64_t)(x)) // Cast to ensure correct size
	#define PD_LEPTR(x) PD_LE64((uint64_t)(x))
	#define PD_PTR_SIZE 8
#elif defined(PLATFORM_32BIT)
	#define PD_BEPTR(x) PD_BE32((uint32_t)(x)) // Cast to ensure correct size
	#define PD_LEPTR(x) PD_LE32((uint32_t)(x))
	#define PD_PTR_SIZE 4
#else
    #error "Platform bit width (32/64) not defined."
#endif


// module constructor function attribute
#if defined(__GNUC__) || defined(__clang__) // This covers psp-gcc
	#define PD_CONSTRUCTOR __attribute__((constructor))
#elif defined(_MSC_VER)
    // MSVC uses different mechanisms, often involving DllMain or static initializers.
    // No direct equivalent attribute. You might need specific linker sections or CRT init.
    // Define as empty or error, depending on how you handle module constructors on Windows.
    #define PD_CONSTRUCTOR /* Not directly supported */
    #pragma message("Warning: PD_CONSTRUCTOR equivalent not implemented for MSVC.")
#else
	#error "Implement PD_CONSTRUCTOR macro for your compiler."
#endif

#endif // _IN_PLATFORM_H
