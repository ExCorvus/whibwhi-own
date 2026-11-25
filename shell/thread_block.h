// Interface of whiwhi_tls.dll: one per-thread storage block that Windows allocates for every thread, handed to
// a loaded image as its thread-local storage.
//
// Windows only gives thread-local storage to modules its own loader loaded. This library is such a module and
// its thread-local section is nothing but one empty block, so its storage index and its per-thread blocks can
// be lent to an image the shell placed itself.
#ifndef WHIWHI_SHELL_THREAD_BLOCK_H
#define WHIWHI_SHELL_THREAD_BLOCK_H

#define WHIWHI_THREAD_BLOCK_BYTES 0x10000
#define WHIWHI_THREAD_BLOCK_ALIGNMENT 64
#define WHIWHI_THREAD_BLOCK_NO_INDEX 0xFFFFFFFFul

#ifdef WHIWHI_THREAD_BLOCK_BUILD
#define WHIWHI_THREAD_BLOCK_API __declspec(dllexport)
#else
#define WHIWHI_THREAD_BLOCK_API __declspec(dllimport)
#endif

#ifdef __cplusplus
extern "C" {
#endif

// Same signature as a PE thread-storage callback.
typedef void(__stdcall* WhiwhiThreadCallback)(void* image_base, unsigned long reason, void* reserved);

typedef struct WhiwhiThreadImage {
    const void* initial_data;              // copied to the start of each thread's block
    unsigned long long initial_bytes;
    void* image_base;                      // passed to the callbacks
    const WhiwhiThreadCallback* callbacks; // null-terminated; may be null
} WhiwhiThreadImage;

// The storage index Windows assigned to the block, or WHIWHI_THREAD_BLOCK_NO_INDEX if Windows assigned none.
WHIWHI_THREAD_BLOCK_API unsigned long whiwhi_thread_block_index(void);

// The calling thread's block (WHIWHI_THREAD_BLOCK_BYTES long), or null if Windows assigned no index.
WHIWHI_THREAD_BLOCK_API void* whiwhi_thread_block(void);

// Gives the block to an image: clears the calling thread's block and copies the image's initial data into it;
// every thread that starts afterwards gets the same. Returns 0 when another image holds the block, the initial
// data does not fit, or Windows assigned no index.
WHIWHI_THREAD_BLOCK_API int whiwhi_thread_block_claim(const WhiwhiThreadImage* image);

// For a thread that already existed when the block was claimed: clears its block and copies the initial data.
WHIWHI_THREAD_BLOCK_API void whiwhi_thread_block_prepare(void);

// Calls the image's callbacks as a process start on the calling thread; from then on they are also called for
// every thread that starts or ends. This runs the image's own code.
WHIWHI_THREAD_BLOCK_API void whiwhi_thread_block_start(void);

// Takes the block back from the image. Its callbacks are not called again.
WHIWHI_THREAD_BLOCK_API void whiwhi_thread_block_release(void);

#ifdef __cplusplus
}
#endif

#endif // WHIWHI_SHELL_THREAD_BLOCK_H
