// Implementation of our XCRDAPI.dll. See xcrdapi.h and docs/console-libs/xcrdapi-dstorage.md.
//
// DStorageGetFactory answers with OUR factory object in the console vtable layout (the image asks for the
// console IID and calls +0x20 OpenFile, +0x28 CreateStatusArray, +0x40 SetStagingBufferSize and +0x50
// CreateQueue; the PC factory ends at +0x38 with CreateQueue at +0x18, so a forwarded PC pointer would be
// undefined behaviour). Our queue reads each request synchronously straight into the request's destination
// address - host memory the xmem library owns - so completion is real (status array: IsComplete = true,
// GetHResult = S_OK), not faked. No staging buffer, no GPU resource, no copy of the file.
//
// Every object logs its first calls to stderr with the "[xcrdapi]" prefix. Unknown vtable slots log their
// offset once and answer E_NOTIMPL. Paths are logged and mapped onto the read-only game folder
// (WHIWHI_GAME_DIR, default the XboxGames install path).
#include "xcrdapi.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

namespace {

std::mutex g_log;

void logf(const char* format, ...) {
    std::lock_guard<std::mutex> lock(g_log);
    std::fprintf(stderr, "[xcrdapi] ");
    va_list ap;
    va_start(ap, format);
    std::vfprintf(stderr, format, ap);
    va_end(ap);
    std::fflush(stderr);
}

// The factory IID the retail image passes (measured at its call site). The console interface is the one publicly
// documented as IDStorageFactoryX (Microsoft Game Development Kit, dstorage_xs.h).
const GUID kConsoleFactoryIid = {0x62959d11, 0xd45f, 0x4888, {0xa0, 0xa3, 0xf7, 0x86, 0x2e, 0xa2, 0x43, 0xae}};
const GUID kPcFactoryIid = {0x6924ea0c, 0xc3cd, 0x4826, {0xb1, 0x0a, 0xf6, 0x4f, 0x4e, 0xd9, 0x27, 0xc1}};
const GUID kIUnknownIid = {0x00000000, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};

bool same_iid(const GUID* a, const GUID* b) {
    return a && b && IsEqualGUID(*a, *b) != FALSE;
}

std::wstring game_folder() {
    wchar_t buf[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"WHIWHI_GAME_DIR", buf, MAX_PATH) > 0) {
        return buf;
    }
    return L"C:\\XboxGames\\GTA6.Xbox.v1.0.2.1";
}

bool path_exists(const wchar_t* p) {
    return p && GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES;
}

// Maps a console-style path onto the read-only game folder. The raw path is logged; the candidates are tried
// in order and the first that exists wins.
std::wstring map_path(const wchar_t* raw, bool& existed) {
    existed = false;
    if (!raw) {
        return std::wstring();
    }
    std::wstring s = raw;
    std::wstring folder = game_folder();
    // The console mounts the package as drive G: (the image asks for "G:\<file>").
    if ((s.size() >= 3) && (s[0] == L'G' || s[0] == L'g') && s[1] == L':' && (s[2] == L'\\' || s[2] == L'/')) {
        std::wstring mapped = folder + L"\\" + s.substr(3);
        for (wchar_t& c : mapped) {
            if (c == L'/') {
                c = L'\\';
            }
        }
        existed = path_exists(mapped.c_str());
        return mapped;
    }
    if (path_exists(s.c_str())) {
        existed = true;
        return s;
    }
    std::wstring stripped = s;
    const std::size_t colon = s.find(L":/");
    if (colon != std::wstring::npos) {
        stripped = s.substr(colon + 2);
    }
    for (wchar_t& c : stripped) {
        if (c == L'/') {
            c = L'\\';
        }
    }
    std::wstring candidate = folder + L"\\" + stripped;
    if (path_exists(candidate.c_str())) {
        existed = true;
        return candidate;
    }
    candidate = folder + L"\\" + s;
    if (path_exists(candidate.c_str())) {
        existed = true;
        return candidate;
    }
    return folder + L"\\" + stripped;  // best guess; the open will fail and be logged
}

// ---- COM objects ----------------------------------------------------------------------------------------

struct Com {
    void** vt;  // must be first: the COM vtable pointer is the object's first field
    std::atomic<long> refs{1};
};

template <typename T>
long qi_impl(T* self, const GUID* riid, void** pp, const GUID* own1, const GUID* own2) {
    if (!riid || !pp) {
        return 0x80004003L;  // E_POINTER
    }
    if (same_iid(riid, &kIUnknownIid) || (own1 && same_iid(riid, own1)) || (own2 && same_iid(riid, own2))) {
        *pp = self;
        self->com.refs.fetch_add(1);
        return 0;
    }
    *pp = nullptr;
    return 0x80004002L;  // E_NOINTERFACE
}

template <typename T>
unsigned long addref_impl(T* self) {
    return static_cast<unsigned long>(self->com.refs.fetch_add(1) + 1);
}

// ---- factory --------------------------------------------------------------------------------------------

struct Factory {
    Com com;
};

struct File {
    Com com;
    HANDLE handle = nullptr;
};

struct StatusArray {
    Com com;
    unsigned long capacity = 0;
};

struct Queue {
    Com com;
    unsigned long long capacity = 0;
    unsigned long priority = 0;
    std::atomic<unsigned long long> enqueued{0};
};

File* make_file(HANDLE h);
StatusArray* make_status_array(unsigned long capacity);
Queue* make_queue(const unsigned char* desc);

// Console factory vtable: 0-2 IUnknown, +0x18 (unused, logged), +0x20 OpenFile, +0x28 CreateStatusArray,
// +0x30/+0x38 (unused, logged), +0x40 SetStagingBufferSize, +0x48 (unused), +0x50 CreateQueue.
long __stdcall f_qi(Factory* self, const GUID* riid, void** pp) {
    return qi_impl(self, riid, pp, &kConsoleFactoryIid, &kPcFactoryIid);
}
unsigned long __stdcall f_addref(Factory* self) {
    return addref_impl(self);
}
unsigned long __stdcall f_release(Factory* self) {
    const long r = self->com.refs.fetch_sub(1) - 1;
    if (r == 0) {
        delete self;
    }
    return static_cast<unsigned long>(r);
}
long __stdcall f_unused(Factory* self, void*, void*, void*, void*) {
    static std::once_flag once;
    std::call_once(once, [] { logf("factory: an unobserved vtable slot was called (answered E_NOTIMPL)\n"); });
    (void)self;
    return 0x80004001L;
}
long __stdcall f_openfile(Factory* self, const wchar_t* path, const GUID* riid, void** out) {
    (void)self;
    static std::atomic<int> shown{0};
    if (!out) {
        return 0x80004003L;
    }
    *out = nullptr;
    if (!path || !riid) {
        return 0x80004003L;
    }
    bool existed = false;
    const std::wstring mapped = map_path(path, existed);
    if (shown.fetch_add(1) < 5) {
        logf("OpenFile: \"%ls\" -> \"%ls\" (%s)\n", path, mapped.c_str(), existed ? "found" : "NOT found");
    }
    HANDLE h = CreateFileW(mapped.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        if (shown.load() <= 5) {
            logf("OpenFile: CreateFileW failed, error %lu\n", GetLastError());
        }
        return 0x80004005L;  // E_FAIL
    }
    *out = make_file(h);
    return 0;
}
long __stdcall f_createstatusarray(Factory* self, unsigned long capacity, const wchar_t* name, const GUID* riid,
                                   void** out) {
    (void)self;
    (void)name;
    if (!out) {
        return 0x80004003L;
    }
    *out = make_status_array(capacity);
    static std::once_flag once;
    std::call_once(once, [capacity] { logf("CreateStatusArray: capacity %lu (all entries complete)\n", capacity); });
    (void)riid;
    return 0;
}
long __stdcall f_setstaging(Factory* self, unsigned long long bytes) {
    (void)self;
    static std::once_flag once;
    std::call_once(once, [bytes] {
        logf("SetStagingBufferSize: %llu bytes - no staging buffer on a PC (reads go straight into the "
             "destination); answered success\n",
             bytes);
    });
    return 0;
}
long __stdcall f_createqueue(Factory* self, const unsigned char* desc, const GUID* riid, void** out) {
    (void)self;
    (void)riid;
    if (!out || !desc) {
        return 0x80004003L;
    }
    *out = make_queue(desc);
    return 0;
}

void** factory_vtable() {
    static void* vt[] = {reinterpret_cast<void*>(&f_qi),
                         reinterpret_cast<void*>(&f_addref),
                         reinterpret_cast<void*>(&f_release),
                         reinterpret_cast<void*>(&f_unused),   // +0x18
                         reinterpret_cast<void*>(&f_openfile),  // +0x20
                         reinterpret_cast<void*>(&f_createstatusarray),  // +0x28
                         reinterpret_cast<void*>(&f_unused),   // +0x30
                         reinterpret_cast<void*>(&f_unused),   // +0x38
                         reinterpret_cast<void*>(&f_setstaging),         // +0x40
                         reinterpret_cast<void*>(&f_unused),   // +0x48
                         reinterpret_cast<void*>(&f_createqueue)};       // +0x50
    return vt;
}

Factory* make_factory() {
    auto* f = new Factory{};
    f->com.vt = factory_vtable();
    return f;
}

// ---- file -----------------------------------------------------------------------------------------------

long __stdcall file_qi(File* self, const GUID* riid, void** pp) {
    return qi_impl(self, riid, pp, nullptr, nullptr);
}
unsigned long __stdcall file_addref(File* self) {
    return addref_impl(self);
}
unsigned long __stdcall file_release(File* self) {
    const long r = self->com.refs.fetch_sub(1) - 1;
    if (r == 0) {
        if (self->handle) {
            CloseHandle(self->handle);
        }
        delete self;
    }
    return static_cast<unsigned long>(r);
}
long __stdcall file_close(File* self) {
    if (self->handle) {
        CloseHandle(self->handle);
        self->handle = nullptr;
    }
    return 0;
}
long __stdcall file_getsize(File* self, unsigned long long* size) {
    if (!size) {
        return 0x80004003L;
    }
    LARGE_INTEGER li{};
    *size = GetFileSizeEx(self->handle, &li) ? static_cast<unsigned long long>(li.QuadPart) : 0;
    return 0;
}

void** file_vtable() {
    static void* vt[] = {reinterpret_cast<void*>(&file_qi), reinterpret_cast<void*>(&file_addref),
                         reinterpret_cast<void*>(&file_release), reinterpret_cast<void*>(&file_close),  // +0x18
                         reinterpret_cast<void*>(&file_getsize)};                                       // +0x20
    return vt;
}

File* make_file(HANDLE h) {
    auto* f = new File{};
    f->com.vt = file_vtable();
    f->handle = h;
    return f;
}

// ---- status array (all entries complete, because our reads finish inside EnqueueRequest) -------------------

long __stdcall sa_qi(StatusArray* self, const GUID* riid, void** pp) {
    return qi_impl(self, riid, pp, nullptr, nullptr);
}
unsigned long __stdcall sa_addref(StatusArray* self) {
    return addref_impl(self);
}
unsigned long __stdcall sa_release(StatusArray* self) {
    const long r = self->com.refs.fetch_sub(1) - 1;
    if (r == 0) {
        delete self;
    }
    return static_cast<unsigned long>(r);
}
long __stdcall sa_gethresult(StatusArray*, unsigned long, long* out) {
    if (out) {
        *out = 0;
    }
    return 0;
}
int __stdcall sa_iscomplete(StatusArray*, unsigned long) {
    return 1;
}
void __stdcall sa_getrequest(StatusArray*, unsigned long, void* out) {
    if (out) {
        std::memset(out, 0, 0x40);
    }
}

void** status_array_vtable() {
    static void* vt[] = {reinterpret_cast<void*>(&sa_qi),  reinterpret_cast<void*>(&sa_addref),
                         reinterpret_cast<void*>(&sa_release), reinterpret_cast<void*>(&sa_gethresult),  // +0x18
                         reinterpret_cast<void*>(&sa_iscomplete),                                        // +0x20
                         reinterpret_cast<void*>(&sa_getrequest)};                                       // +0x28
    return vt;
}

StatusArray* make_status_array(unsigned long capacity) {
    auto* s = new StatusArray{};
    s->com.vt = status_array_vtable();
    s->capacity = capacity;
    return s;
}

// ---- queue ----------------------------------------------------------------------------------------------

unsigned long rd32(const void* p) {
    unsigned long v;
    std::memcpy(&v, p, 4);
    return v;
}
unsigned long long rd64(const void* p) {
    unsigned long long v;
    std::memcpy(&v, p, 8);
    return v;
}

// The request is 0x40 bytes. Observed fields (docs/console-libs/xcrdapi-dstorage.md §4): +0x00 options
// (0 = file, 0x400000 = memory source), +0x08 destination VA, +0x10 file object (file) or source pointer
// (memory), +0x18 file offset, +0x20 destination size, +0x28 source size. The first few requests are logged in
// full so the field map is confirmed against the real image.
long do_request(Queue* q, const unsigned char* r) {
    const unsigned long options = rd32(r + 0x00);
    const unsigned long long dest = rd64(r + 0x08);
    const unsigned long long source = rd64(r + 0x10);
    const unsigned long long offset = rd64(r + 0x18);
    const unsigned long dest_size = rd32(r + 0x20);
    const unsigned long source_size = rd32(r + 0x28);
    const unsigned long long n = (dest_size && source_size) ? (dest_size < source_size ? dest_size : source_size)
                                                            : (source_size ? source_size : dest_size);

    const unsigned long long count = q->enqueued.fetch_add(1);
    if (count < 5) {
        logf("EnqueueRequest #%llu: options 0x%lx dest 0x%llx source 0x%llx offset 0x%llx destsize %lu srcsize %lu "
             "(full request: %016llx %016llx %016llx %016llx %016llx %016llx %016llx %016llx)\n",
             count + 1, options, dest, source, offset, dest_size, source_size, rd64(r), rd64(r + 8), rd64(r + 0x10),
             rd64(r + 0x18), rd64(r + 0x20), rd64(r + 0x28), rd64(r + 0x30), rd64(r + 0x38));
    }
    if (!dest || !n) {
        logf("EnqueueRequest #%llu: empty destination or size - skipped\n", count + 1);
        return 0;
    }
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(reinterpret_cast<void*>(dest), &mbi, sizeof(mbi)) != sizeof(mbi) ||
        mbi.State != MEM_COMMIT || (mbi.Protect & 0xFF) == 0x01 /*PAGE_NOACCESS*/) {
        logf("EnqueueRequest #%llu: destination 0x%llx is not committed writable memory - skipped, not written "
             "(logged, not hidden)\n",
             count + 1, dest);
        return 0;
    }
    if (options & 0x400000ul) {
        // Memory-sourced: a plain copy into the destination.
        std::memcpy(reinterpret_cast<void*>(dest), reinterpret_cast<const void*>(source),
                    static_cast<size_t>(n));
        return 0;
    }
    auto* file = reinterpret_cast<File*>(source);
    if (!file || !file->handle) {
        logf("EnqueueRequest #%llu: no file object - skipped\n", count + 1);
        return 0;
    }
    OVERLAPPED ov{};
    ov.Offset = static_cast<unsigned long>(offset & 0xFFFFFFFFull);
    ov.OffsetHigh = static_cast<unsigned long>(offset >> 32);
    ov.hEvent = nullptr;
    DWORD read_bytes = 0;
    const BOOL ok = ReadFile(file->handle, reinterpret_cast<void*>(dest), static_cast<DWORD>(n), &read_bytes, &ov);
    if (!ok) {
        const unsigned long err = GetLastError();
        if (err != ERROR_IO_PENDING) {
            logf("EnqueueRequest #%llu: ReadFile at offset 0x%llx into 0x%llx failed, error %lu\n", count + 1,
                 offset, dest, err);
            return static_cast<long>(0x80004005L);
        }
        while (!HasOverlappedIoCompleted(&ov)) {
            Sleep(0);
        }
        read_bytes = 0;
        GetOverlappedResult(file->handle, &ov, &read_bytes, FALSE);
    }
    if (count < 5) {
        logf("EnqueueRequest #%llu: read %lu of %llu bytes at offset 0x%llx into 0x%llx\n", count + 1, read_bytes, n,
             offset, dest);
    }
    return 0;
}

long __stdcall q_qi(Queue* self, const GUID* riid, void** pp) {
    return qi_impl(self, riid, pp, nullptr, nullptr);
}
unsigned long __stdcall q_addref(Queue* self) {
    return addref_impl(self);
}
unsigned long __stdcall q_release(Queue* self) {
    const long r = self->com.refs.fetch_sub(1) - 1;
    if (r == 0) {
        delete self;
    }
    return static_cast<unsigned long>(r);
}
long __stdcall q_enqueuerequest(Queue* self, const void* request) {
    return do_request(self, static_cast<const unsigned char*>(request));
}
long __stdcall q_noop(Queue* self, void*, void*, void*) {
    (void)self;
    return 0;
}
long __stdcall q_close(Queue* self) {
    (void)self;
    static std::once_flag once;
    std::call_once(once, [] { logf("queue Close\n"); });
    return 0;
}

void** queue_vtable() {
    static void* vt[] = {reinterpret_cast<void*>(&q_qi),           reinterpret_cast<void*>(&q_addref),
                         reinterpret_cast<void*>(&q_release),        reinterpret_cast<void*>(&q_enqueuerequest),  // +0x18
                         reinterpret_cast<void*>(&q_noop),           // +0x20 Submit
                         reinterpret_cast<void*>(&q_noop),           // +0x28 EnqueueStatus
                         reinterpret_cast<void*>(&q_noop),           // +0x30 EnqueueSignal
                         reinterpret_cast<void*>(&q_noop),           // +0x38 CancelRequestsWithTag
                         reinterpret_cast<void*>(&q_close)};         // +0x40 Close
    return vt;
}

Queue* make_queue(const unsigned char* desc) {
    auto* q = new Queue{};
    q->com.vt = queue_vtable();
    // Descriptor (observed): +0x00 source type, +0x08 u16 capacity, +0x0a priority byte, +0x10 name, +0x18 u64.
    if (desc) {
        q->capacity = rd32(desc + 0x08) & 0xFFFF;
        q->priority = (rd32(desc + 0x08) >> 16) & 0xFF;
    }
    return q;
}

} // namespace

extern "C" __declspec(dllexport) long __stdcall DStorageGetFactory(const GUID* riid, void** factory) {
    static std::atomic<int> calls{0};
    const int call = calls.fetch_add(1);
    if (!factory) {
        return 0x80004003L;  // E_POINTER
    }
    *factory = nullptr;
    if (call == 0) {
        if (riid) {
            logf("DStorageGetFactory: riid {%08lx-%04hx-%04hx-%02x%02x-%02x%02x%02x%02x%02x%02x}%s - answering "
                 "with our own factory (console vtable layout)\n",
                 static_cast<unsigned long>(riid->Data1), riid->Data2, riid->Data3, riid->Data4[0], riid->Data4[1],
                 riid->Data4[2], riid->Data4[3], riid->Data4[4], riid->Data4[5], riid->Data4[6], riid->Data4[7],
                 same_iid(riid, &kConsoleFactoryIid) ? " (the console factory IID the image asks for)" : "");
        } else {
            logf("DStorageGetFactory: null riid - answering with our own factory\n");
        }
    }
    (void)kPcFactoryIid;
    *factory = make_factory();
    return 0;
}
