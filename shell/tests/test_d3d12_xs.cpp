// Unit tests for the graphics library (no GPU, no game data): the built d3d12_xs.dll beside this test must
// export the three console functions, answer D3D12ConfigureVirtualMemoryX with success, and forward
// D3D12SerializeVersionedRootSignature to the PC's d3d12.dll (serializing an empty root signature needs no
// device).
#include <cstdio>
#include <cstring>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <d3d12.h>

static int failures = 0;
#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                \
            ++failures;                                                                                                \
        }                                                                                                              \
    } while (0)

int main() {
    const HMODULE lib = LoadLibraryA("d3d12_xs.dll");
    CHECK(lib != nullptr);
    if (!lib) {
        return 1;
    }
    const auto create_device = reinterpret_cast<long(__stdcall*)(IUnknown*, const void*, const GUID*, void**)>(
        GetProcAddress(lib, "D3D12XboxCreateDevice"));
    const auto configure_vm = reinterpret_cast<long(__stdcall*)(void*)>(GetProcAddress(lib, "D3D12ConfigureVirtualMemoryX"));
    const auto serialize = reinterpret_cast<long(__stdcall*)(const void*, void**, void**)>(
        GetProcAddress(lib, "D3D12SerializeVersionedRootSignature"));
    CHECK(create_device != nullptr);
    CHECK(configure_vm != nullptr);
    CHECK(serialize != nullptr);
    if (!configure_vm || !serialize) {
        return 1;
    }

    unsigned long block[8] = {};
    block[1] = 42;
    CHECK(configure_vm(block) == 0);
    CHECK(configure_vm(nullptr) == 0);

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc{};
    desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_0;
    desc.Desc_1_0.NumParameters = 0;
    desc.Desc_1_0.NumStaticSamplers = 0;
    desc.Desc_1_0.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
    ID3DBlob* blob = nullptr;
    ID3DBlob* error_blob = nullptr;
    const long hr = serialize(&desc, reinterpret_cast<void**>(&blob), reinterpret_cast<void**>(&error_blob));
    CHECK(hr == 0);
    CHECK(blob != nullptr && blob->GetBufferSize() > 0);

    // The function the game's shipped upscaler library asks for: the serialized root signature above comes back
    // as a deserializer object (no device needed).
    const auto deserialize =
        reinterpret_cast<long(__stdcall*)(const void*, unsigned long long, const GUID*, void**)>(
            GetProcAddress(lib, "D3D12CreateRootSignatureDeserializer"));
    CHECK(deserialize != nullptr);
    if (deserialize && blob) {
        ID3D12RootSignatureDeserializer* reader = nullptr;
        CHECK(deserialize(blob->GetBufferPointer(), blob->GetBufferSize(), &__uuidof(ID3D12RootSignatureDeserializer),
                          reinterpret_cast<void**>(&reader)) == 0);
        CHECK(reader != nullptr);
        if (reader) {
            CHECK(reader->GetRootSignatureDesc() != nullptr && reader->GetRootSignatureDesc()->NumParameters == 0);
            reader->Release();
        }
    }
    if (blob) {
        blob->Release();
    }
    if (error_blob) {
        error_blob->Release();
    }

    // The record of which pool page each page of the GPU-only view shows (the queue's page-mapping call), with
    // made-up addresses. No device is needed for the record itself.
    struct Range {
        unsigned int kind, first_pool_page, count;
    };
    struct Batch {
        unsigned long long destination;
        unsigned int range_count;
        const Range* ranges;
    };
    CHECK(sizeof(Range) == 12 && sizeof(Batch) == 24);
    const auto apply = reinterpret_cast<long(__stdcall*)(unsigned int, const Batch*, unsigned long long)>(
        GetProcAddress(lib, "whiwhi_d3d12_apply_page_mappings"));
    const auto view_page = reinterpret_cast<int(__stdcall*)(unsigned long long, unsigned int*)>(
        GetProcAddress(lib, "whiwhi_d3d12_view_page"));
    const auto pages_shown =
        reinterpret_cast<unsigned long long(__stdcall*)(void)>(GetProcAddress(lib, "whiwhi_d3d12_view_pages_shown"));
    CHECK(apply != nullptr && view_page != nullptr && pages_shown != nullptr);
    if (apply && view_page && pages_shown) {
        const unsigned long long view = 0x7000000000ull;  // a made-up view address on a 64 KiB boundary
        const unsigned long long pool_pages = 100;
        CHECK(pages_shown() == 0);
        // Three pages shown the way the image does it: one range of kind 1 with count 1 per page.
        const Range show[3] = {{1, 7, 1}, {1, 42, 1}, {1, 3, 1}};
        const Batch one = {view + 5 * 0x10000ull, 3, show};
        CHECK(apply(1, &one, pool_pages) == 0);
        CHECK(pages_shown() == 3);
        unsigned int pool = 999;
        CHECK(view_page(view + 5 * 0x10000ull, &pool) == 1 && pool == 7);
        CHECK(view_page(view + 6 * 0x10000ull + 0x1234, &pool) == 1 && pool == 42);  // any address in the page
        CHECK(view_page(view + 7 * 0x10000ull + 0xFFFF, &pool) == 1 && pool == 3);
        CHECK(view_page(view + 8 * 0x10000ull, &pool) == 0);
        CHECK(view_page(view + 4 * 0x10000ull + 0xFFFF, &pool) == 0);
        // A page shown again shows the newer pool page.
        const Range again[1] = {{1, 50, 1}};
        const Batch second = {view + 6 * 0x10000ull, 1, again};
        CHECK(apply(1, &second, pool_pages) == 0);
        CHECK(pages_shown() == 3);
        CHECK(view_page(view + 6 * 0x10000ull, &pool) == 1 && pool == 50);
        // Two batches in one call: remove two pages (one range of kind 2), and show one elsewhere.
        const Range remove[1] = {{2, 0, 2}};
        const Range far_show[1] = {{1, 99, 1}};
        const Batch two[2] = {{view + 5 * 0x10000ull, 1, remove}, {view + 64 * 0x10000ull, 1, far_show}};
        CHECK(apply(2, two, pool_pages) == 0);
        CHECK(pages_shown() == 2);
        CHECK(view_page(view + 5 * 0x10000ull, &pool) == 0);
        CHECK(view_page(view + 6 * 0x10000ull, &pool) == 0);
        CHECK(view_page(view + 7 * 0x10000ull, &pool) == 1 && pool == 3);
        CHECK(view_page(view + 64 * 0x10000ull, &pool) == 1 && pool == 99);
        // Refusals change nothing, even when an earlier batch of the same call was sound: a pool page past the
        // end, a destination off the 64 KiB boundary, a kind never seen, a showing range with a count above 1,
        // a removing range with no count, a batch with a count but no ranges.
        const Range past[1] = {{1, 100, 1}};
        const Range sound[1] = {{1, 1, 1}};
        const Batch bad_pool[2] = {{view, 1, sound}, {view + 0x10000ull, 1, past}};
        CHECK(apply(2, bad_pool, pool_pages) != 0);
        CHECK(view_page(view, &pool) == 0);
        const Batch off_boundary = {view + 0x8000ull, 1, sound};
        CHECK(apply(1, &off_boundary, pool_pages) != 0);
        const Range unknown_kind[1] = {{3, 1, 1}};
        const Batch bad_kind = {view, 1, unknown_kind};
        CHECK(apply(1, &bad_kind, pool_pages) != 0);
        const Range many[1] = {{1, 1, 2}};
        const Batch bad_count = {view, 1, many};
        CHECK(apply(1, &bad_count, pool_pages) != 0);
        const Range none[1] = {{2, 0, 0}};
        const Batch bad_remove = {view, 1, none};
        CHECK(apply(1, &bad_remove, pool_pages) != 0);
        const Batch no_ranges = {view, 1, nullptr};
        CHECK(apply(1, &no_ranges, pool_pages) != 0);
        CHECK(apply(1, nullptr, pool_pages) != 0);
        CHECK(pages_shown() == 2);
        // Nothing to do is not a failure; removing pages that show nothing is not one either.
        CHECK(apply(0, nullptr, pool_pages) == 0);
        const Range remove_all[1] = {{2, 0, 80}};
        const Batch clear = {view, 1, remove_all};
        CHECK(apply(1, &clear, pool_pages) == 0);
        CHECK(pages_shown() == 0);
    }

    if (failures == 0) {
        std::printf("all checks passed\n");
    }
    return failures ? 1 : 0;
}
