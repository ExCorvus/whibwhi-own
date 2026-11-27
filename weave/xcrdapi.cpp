// Implementation of our XCRDAPI.dll. See xcrdapi.h and docs/console-libs/xcrdapi-dstorage.md.
//
// DStorageGetFactory answers with OUR factory object in the console vtable layout (the image asks for the
// console IID and calls +0x20 OpenFile, +0x28 CreateStatusArray, +0x40 SetStagingBufferSize and +0x50
// CreateQueue; the PC factory ends at +0x38 with CreateQueue at +0x18, so a forwarded PC pointer would be
// undefined behaviour). Our queue carries each request out when it arrives, straight into the request's
// destination address in the image's own memory: a plain read, or a read whose bytes are one zlib stream and
// are decoded on the way (inflate.cpp). A request it cannot carry out is logged in full and reported as a
// failure through the next status entry; nothing is reported as done that was not done. No staging buffer, no
// GPU resource, no copy of the file on disk.
//
// Every object logs its first calls to stderr with the "[xcrdapi]" prefix. Unknown vtable slots log their
// offset once and answer E_NOTIMPL. Paths are logged and mapped onto the read-only game folder
// (WHIWHI_GAME_DIR, default the XboxGames install path).
#include "xcrdapi.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <intrin.h>

#include "inflate.hpp"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <vector>

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
const GUID kConsoleFactoryIid = {0x62959d11, 0xd45f, 0x4884, {0xa0, 0xa3, 0xf7, 0x86, 0x2e, 0xa2, 0x43, 0xae}};
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

constexpr long kPending = static_cast<long>(0x8000000AL);  // E_PENDING: no result yet

struct StatusEntry {
    std::atomic<long> result{kPending};
    std::atomic<bool> complete{false};
};

struct StatusArray {
    Com com;
    unsigned long capacity = 0;
    StatusEntry* entries = nullptr;
};

struct Queue {
    Com com;
    unsigned long long capacity = 0;
    unsigned long priority = 0;
    std::atomic<unsigned long long> enqueued{0};
    std::atomic<unsigned long long> carried_out{0};
    std::atomic<unsigned long long> not_carried_out{0};
    // The first failure since the last status entry was written (0 = none). Read and cleared when the image
    // asks for a status entry.
    std::atomic<long> first_failure{0};
};

// The objects this library has handed out and not yet released, so that a pointer found in a request or an
// argument is used as one of them only when it is one.
std::mutex g_objects_lock;
std::set<const void*> g_files;
std::set<const void*> g_status_arrays;

void remember_object(std::set<const void*>& set, const void* object) {
    std::lock_guard<std::mutex> lock(g_objects_lock);
    set.insert(object);
}
void forget_object(std::set<const void*>& set, const void* object) {
    std::lock_guard<std::mutex> lock(g_objects_lock);
    set.erase(object);
}
bool is_object(const std::set<const void*>& set, const void* object) {
    std::lock_guard<std::mutex> lock(g_objects_lock);
    return set.count(object) != 0;
}

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
    std::call_once(once, [capacity] { logf("CreateStatusArray: capacity %lu (an entry has no result until the queue writes it)\n", capacity); });
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
        forget_object(g_files, self);
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
    remember_object(g_files, f);
    return f;
}

// ---- status array -----------------------------------------------------------------------------------------
//
// An entry has no result until the image asks the queue to write one (see the queue). Then it holds success
// when every request since the previous entry was carried out, or the first failure among them. Requests are
// carried out inside EnqueueRequest, so an entry is complete as soon as it is written.
//
// The method order after Release is the public one of IDStorageStatusArray (IsComplete, GetHResult,
// GetFirstFailure). The image asks for a console identifier, and no call of the image to these three methods
// has been read in its code, so the order is an inference; the first calls of each are logged.

void note_status_call(const char* name, unsigned long index, long long answer) {
    static std::atomic<int> shown{0};
    if (shown.fetch_add(1) < 12) {
        logf("status array %s(%lu) -> 0x%llx\n", name, index, static_cast<unsigned long long>(answer));
    }
}

long __stdcall sa_qi(StatusArray* self, const GUID* riid, void** pp) {
    return qi_impl(self, riid, pp, nullptr, nullptr);
}
unsigned long __stdcall sa_addref(StatusArray* self) {
    return addref_impl(self);
}
unsigned long __stdcall sa_release(StatusArray* self) {
    const long r = self->com.refs.fetch_sub(1) - 1;
    if (r == 0) {
        forget_object(g_status_arrays, self);
        delete[] self->entries;
        delete self;
    }
    return static_cast<unsigned long>(r);
}
int __stdcall sa_iscomplete(StatusArray* self, unsigned long index) {
    const int answer = index < self->capacity && self->entries[index].complete.load() ? 1 : 0;
    note_status_call("IsComplete", index, answer);
    return answer;
}
long __stdcall sa_gethresult(StatusArray* self, unsigned long index) {
    const long answer = index < self->capacity ? self->entries[index].result.load()
                                               : static_cast<long>(0x80070057L);  // E_INVALIDARG
    note_status_call("GetHResult", index, static_cast<unsigned long>(answer));
    return answer;
}
// The public method fills a record describing the first failed request. Its console layout is not known, so
// nothing is written.
void __stdcall sa_getfirstfailure(StatusArray*, unsigned long index, void*) {
    note_status_call("GetFirstFailure (nothing written: the record's layout is not known)", index, 0);
}

void** status_array_vtable() {
    static void* vt[] = {reinterpret_cast<void*>(&sa_qi),         reinterpret_cast<void*>(&sa_addref),
                         reinterpret_cast<void*>(&sa_release),     reinterpret_cast<void*>(&sa_iscomplete),   // +0x18
                         reinterpret_cast<void*>(&sa_gethresult),                                            // +0x20
                         reinterpret_cast<void*>(&sa_getfirstfailure)};                                      // +0x28
    return vt;
}

StatusArray* make_status_array(unsigned long capacity) {
    auto* s = new StatusArray{};
    s->com.vt = status_array_vtable();
    s->capacity = capacity;
    s->entries = new StatusEntry[capacity ? capacity : 1];
    remember_object(g_status_arrays, s);
    return s;
}

// ---- queue ------------------------------------------------------------------------------------------------

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

constexpr long kNotImplemented = static_cast<long>(0x80004001L);
constexpr long kFail = static_cast<long>(0x80004005L);
constexpr long kInvalidArgument = static_cast<long>(0x80070057L);
constexpr unsigned long long kOptionInflate = 1;        // the source bytes are one zlib stream
constexpr unsigned long kLargestPiece = 0x10000000ul;   // 256 MiB: more than that in one request is refused

// Whether every page of [address, address + bytes) is committed and may be written.
bool can_write(unsigned long long address, unsigned long long bytes) {
    unsigned long long at = address;
    const unsigned long long end = address + bytes;
    if (!address || end < address) {
        return false;
    }
    while (at < end) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(reinterpret_cast<void*>(at), &mbi, sizeof(mbi)) != sizeof(mbi) || mbi.State != MEM_COMMIT ||
            (mbi.Protect & PAGE_GUARD)) {
            return false;
        }
        const unsigned long access = mbi.Protect & 0xFF;
        if (access != PAGE_READWRITE && access != PAGE_WRITECOPY && access != PAGE_EXECUTE_READWRITE &&
            access != PAGE_EXECUTE_WRITECOPY) {
            return false;
        }
        at = reinterpret_cast<unsigned long long>(mbi.BaseAddress) + mbi.RegionSize;
    }
    return true;
}

// Reads exactly `bytes` at `offset`. The handle was opened for overlapped use.
bool read_exactly(HANDLE file, unsigned long long offset, void* to, unsigned long bytes, unsigned long& error) {
    unsigned long done = 0;
    while (done < bytes) {
        OVERLAPPED ov{};
        const unsigned long long at = offset + done;
        ov.Offset = static_cast<unsigned long>(at & 0xFFFFFFFFull);
        ov.OffsetHigh = static_cast<unsigned long>(at >> 32);
        DWORD got = 0;
        if (!ReadFile(file, static_cast<unsigned char*>(to) + done, bytes - done, &got, &ov)) {
            error = GetLastError();
            if (error != ERROR_IO_PENDING || !GetOverlappedResult(file, &ov, &got, TRUE)) {
                if (error == ERROR_IO_PENDING) {
                    error = GetLastError();
                }
                return false;
            }
        }
        if (got == 0) {
            error = ERROR_HANDLE_EOF;
            return false;
        }
        done += got;
    }
    error = 0;
    return true;
}

// A request is eight 64-bit words. Layout as the image fills it, read off a run (every request the image made
// for its shader package had this shape) - see docs/console-libs/xcrdapi-dstorage.md:
//   +0x00 options      0 = copy the file bytes; 1 = the file bytes are one zlib stream to be decoded
//   +0x08 destination  an address in the image's own memory
//   +0x10 (zero in every request seen)
//   +0x18 destination bytes (32 bits; the 32 bits above were zero)
//   +0x20 the file object OpenFile returned
//   +0x28 offset in the file
//   +0x30 source bytes (32 bits; the 32 bits above were zero)
//   +0x38 (zero in every request seen)
// Anything else - other option bits, a non-zero word where zero was seen, a file object that is not one of
// ours - is logged in full and not carried out. The read goes straight into the destination; for a zlib piece
// the compressed bytes pass through a temporary buffer and are decoded into the destination.
long carry_out(Queue* q, const unsigned char* r, unsigned long long number) {
    const unsigned long long options = rd64(r + 0x00);
    const unsigned long long destination = rd64(r + 0x08);
    const unsigned long long word2 = rd64(r + 0x10);
    const unsigned long destination_bytes = rd32(r + 0x18);
    const unsigned long word3_high = rd32(r + 0x1c);
    const unsigned long long file_word = rd64(r + 0x20);
    const unsigned long long offset = rd64(r + 0x28);
    const unsigned long source_bytes = rd32(r + 0x30);
    const unsigned long word6_high = rd32(r + 0x34);
    const unsigned long long word7 = rd64(r + 0x38);
    (void)q;

    const auto refuse = [&](const char* why, long code) {
        static std::atomic<int> shown{0};
        if (shown.fetch_add(1) < 24) {
            logf("request #%llu NOT carried out (%s): %016llx %016llx %016llx %016llx %016llx %016llx %016llx "
                 "%016llx\n",
                 number, why, rd64(r), rd64(r + 8), rd64(r + 0x10), rd64(r + 0x18), rd64(r + 0x20), rd64(r + 0x28),
                 rd64(r + 0x30), rd64(r + 0x38));
        }
        return code;
    };

    if (options != 0 && options != kOptionInflate) {
        return refuse("option bits this library does not carry out", kNotImplemented);
    }
    if (word2 || word3_high || word6_high || word7) {
        return refuse("a field that was zero in every request seen is not zero", kNotImplemented);
    }
    const auto* file = reinterpret_cast<const File*>(file_word);
    if (!is_object(g_files, file) || !file->handle) {
        return refuse("the file is not an open file of this library", kInvalidArgument);
    }
    if (!source_bytes || !destination_bytes || source_bytes > kLargestPiece || destination_bytes > kLargestPiece) {
        return refuse("an empty or oversized piece", kInvalidArgument);
    }
    if (options == 0 && source_bytes != destination_bytes) {
        return refuse("a plain copy whose two sizes differ", kNotImplemented);
    }
    if (!can_write(destination, destination_bytes)) {
        return refuse("the destination is not memory that can be written", kInvalidArgument);
    }

    unsigned long error = 0;
    if (options == 0) {
        if (!read_exactly(file->handle, offset, reinterpret_cast<void*>(destination), source_bytes, error)) {
            logf("request #%llu: reading %lu bytes at offset 0x%llx failed, error %lu\n", number, source_bytes,
                 offset, error);
            return kFail;
        }
        if (number <= 3) {
            logf("request #%llu: %lu bytes at offset 0x%llx read into 0x%llx\n", number, source_bytes, offset,
                 destination);
        }
        return 0;
    }

    std::vector<std::uint8_t> packed(source_bytes);
    if (!read_exactly(file->handle, offset, packed.data(), source_bytes, error)) {
        logf("request #%llu: reading %lu bytes at offset 0x%llx failed, error %lu\n", number, source_bytes, offset,
             error);
        return kFail;
    }
    const whiwhi::weave::InflateOutcome o = whiwhi::weave::inflate_zlib(
        packed.data(), packed.size(), reinterpret_cast<std::uint8_t*>(destination), destination_bytes);
    if (o.result != whiwhi::weave::InflateResult::ok) {
        logf("request #%llu: the %lu bytes at offset 0x%llx did not decode (%s; %zu bytes used, %zu written of "
             "%lu); the destination holds what was written before the failure\n",
             number, source_bytes, offset, whiwhi::weave::inflate_result_name(o.result), o.consumed, o.produced,
             destination_bytes);
        return kFail;
    }
    static std::atomic<int> sizes_shown{0};
    if (number <= 3 || (o.produced != destination_bytes && sizes_shown.fetch_add(1) < 8)) {
        logf("request #%llu: %lu bytes at offset 0x%llx decoded to %zu bytes at 0x%llx (room %lu, %zu of the "
             "source used)\n",
             number, source_bytes, offset, o.produced, destination, destination_bytes, o.consumed);
    }
    return 0;
}

// The public EnqueueRequest returns nothing: a failure is reported through the next status entry. The result
// is returned as well, in case the console's method does return one.
long __stdcall q_enqueuerequest(Queue* self, const void* request) {
    const unsigned long long number = self->enqueued.fetch_add(1) + 1;
    long result = kInvalidArgument;
    if (request) {
        result = carry_out(self, static_cast<const unsigned char*>(request), number);
    }
    if (result == 0) {
        const unsigned long long done = self->carried_out.fetch_add(1) + 1;
        if ((done & (done - 1)) == 0 && done >= 64) {
            logf("%llu requests carried out on this queue so far\n", done);
        }
    } else {
        self->not_carried_out.fetch_add(1);
        long none = 0;
        self->first_failure.compare_exchange_strong(none, result);
    }
    return result;
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

// The queue's other methods. Which offset is which is not established (the public queue has EnqueueStatus,
// EnqueueSignal, Submit, CancelRequestsWithTag, Close after EnqueueRequest; the console's order was only
// inferred), so a call is recognised by what it is given:
//  - a status array of this library and an index inside it: the entry is written - success when every request
//    since the previous entry was carried out, otherwise the first failure. That is the public EnqueueStatus.
//  - anything else is logged (the first calls per offset, with its arguments) and answered "not implemented".
//    Submit has nothing to do here because requests are carried out when they arrive, and the public Submit
//    returns nothing; a method that passes a fence or an event is NOT carried out and says so.
template <int Offset>
long __stdcall q_method(Queue* self, void* a1, void* a2, void* a3) {
    if (is_object(g_status_arrays, a1)) {
        auto* array = static_cast<StatusArray*>(a1);
        const unsigned long index = static_cast<unsigned long>(reinterpret_cast<std::uintptr_t>(a2));
        static std::atomic<int> shown{0};
        if (index >= array->capacity) {
            logf("queue method +0x%x: status array given with index %lu outside its %lu entries - refused\n",
                 Offset, index, array->capacity);
            return kInvalidArgument;
        }
        const long outcome = self->first_failure.exchange(0);
        array->entries[index].result.store(outcome);
        array->entries[index].complete.store(true);
        if (shown.fetch_add(1) < 6 || outcome != 0) {
            logf("queue method +0x%x: status entry %lu written: 0x%08lx (%llu requests carried out, %llu not, on "
                 "this queue)\n",
                 Offset, index, static_cast<unsigned long>(outcome), self->carried_out.load(),
                 self->not_carried_out.load());
        }
        return 0;
    }
    static std::atomic<int> shown{0};
    if (shown.fetch_add(1) < 6) {
        logf("queue method +0x%x called with (0x%llx, 0x%llx, 0x%llx), returns to %p - not recognised, answered "
             "not implemented\n",
             Offset, static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(a1)),
             static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(a2)),
             static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(a3)), _ReturnAddress());
    }
    return kNotImplemented;
}

void** queue_vtable() {
    static void* vt[] = {reinterpret_cast<void*>(&q_qi),
                         reinterpret_cast<void*>(&q_addref),
                         reinterpret_cast<void*>(&q_release),
                         reinterpret_cast<void*>(&q_enqueuerequest),   // +0x18
                         reinterpret_cast<void*>(&q_method<0x20>),
                         reinterpret_cast<void*>(&q_method<0x28>),
                         reinterpret_cast<void*>(&q_method<0x30>),
                         reinterpret_cast<void*>(&q_method<0x38>),
                         reinterpret_cast<void*>(&q_method<0x40>),
                         reinterpret_cast<void*>(&q_method<0x48>),
                         reinterpret_cast<void*>(&q_method<0x50>),
                         reinterpret_cast<void*>(&q_method<0x58>),
                         reinterpret_cast<void*>(&q_method<0x60>),
                         reinterpret_cast<void*>(&q_method<0x68>),
                         reinterpret_cast<void*>(&q_method<0x70>),
                         reinterpret_cast<void*>(&q_method<0x78>)};
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
