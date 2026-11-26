// XboxBvhBuilder.dll implementation: skeleton. Each export logs once to stderr ([bvh]) and answers failure.
// The image imports the seven names mangled; the /export aliases below map them onto the plain names.
// See xbox_bvh_builder.h and docs/exe-map/GRAPHICS-CALLS.md.
#include "xbox_bvh_builder.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>

// The mangled names from the image's import table, exported onto the plain functions below.
#pragma comment(linker, "/export:??0XboxBvhBuilder@@QEAA@PEAUID3D12Device11@@_N@Z=bvh_ctor")
#pragma comment(linker, "/export:??1XboxBvhBuilder@@QEAA@XZ=bvh_dtor")
#pragma comment(linker, "/export:?BuildRaytracingAccelerationStructureBatched2X@XboxBvhBuilder@@QEAAXPEAUID3D12GraphicsCommandList6@@IPEBUD3D12XBOX_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_BATCHED_DESC2@@@Z=bvh_build_batched2")
#pragma comment(linker, "/export:?CopyRaytracingAccelerationStructure@XboxBvhBuilder@@QEAAXPEAUID3D12GraphicsCommandList4@@_K1W4D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE@@@Z=bvh_copy")
#pragma comment(linker, "/export:?GetRaytracingAccelerationStructurePrebuildInfo@XboxBvhBuilder@@QEAAXPEBUD3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS@@PEAUD3D12XBOX_XDXR_STREAM_DESC@@PEAUD3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO@@@Z=bvh_prebuild_info")
#pragma comment(linker, "/export:?RehydrateRaytracingAccelerationStructureBatched2X@XboxBvhBuilder@@QEAAJPEAUID3D12GraphicsCommandList8@@IPEBUD3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC@@PEB_K@Z=bvh_rehydrate_batched2")
#pragma comment(linker, "/export:?Set64BitMortonCodes@XboxBvhBuilder@@QEAAX_N@Z=bvh_set_morton")

namespace {

void log_once(long* flag, const char* name) {
    if (InterlockedCompareExchange(flag, 1, 0) == 0) {
        std::fprintf(stderr, "[bvh] %s: ray-tracing builder not provided on a PC yet; answering failure\n", name);
        std::fflush(stderr);
    }
}

long g_once[7] = {};

} // namespace

extern "C" {

void bvh_ctor(void*, void*, int) {
    log_once(&g_once[0], "XboxBvhBuilder::XboxBvhBuilder");
}
void bvh_dtor(void*) {
    log_once(&g_once[1], "XboxBvhBuilder::~XboxBvhBuilder");
}
void bvh_build_batched2(void*, void*, void*) {
    log_once(&g_once[2], "BuildRaytracingAccelerationStructureBatched2X");
}
void bvh_copy(void*, void*, unsigned long long, unsigned long long, int) {
    log_once(&g_once[3], "CopyRaytracingAccelerationStructure");
}
void bvh_prebuild_info(void*, void*, void*, void*) {
    log_once(&g_once[4], "GetRaytracingAccelerationStructurePrebuildInfo");
}
long bvh_rehydrate_batched2(void*, void*, void*, unsigned long long, int) {
    log_once(&g_once[5], "RehydrateRaytracingAccelerationStructureBatched2X");
    return 0x80004001L;
}
void bvh_set_morton(void*, int) {
    log_once(&g_once[6], "Set64BitMortonCodes");
}

} // extern "C"
