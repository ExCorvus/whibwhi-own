// Interface of our XboxBvhBuilder.dll: the console's ray-tracing builder library, answered on a PC.
//
// The game's shipped XboxBvhBuilder.dll cannot load on a PC (it needs console-only d3d12_xs/xmem functions), so
// the shell provides a library of the same name with the same seven exported names (the image's import table
// lists them mangled; the /export aliases in the .cpp map them). Each export logs once and answers failure -
// the BVH is real work for later, not faked here.
#ifndef WHIWHI_SHELL_XBOX_BVH_BUILDER_H
#define WHIWHI_SHELL_XBOX_BVH_BUILDER_H

#ifdef WHIWHI_XBOX_BVH_BUILDER_BUILD
#define WHIWHI_BVH_API __declspec(dllexport)
#else
#define WHIWHI_BVH_API __declspec(dllimport)
#endif

#ifdef __cplusplus
extern "C" {
#endif

// The seven exports the image imports, under plain names; the .cpp aliases the mangled names onto them.
WHIWHI_BVH_API void bvh_ctor(void* self, void* device, int flag);
WHIWHI_BVH_API void bvh_dtor(void* self);
WHIWHI_BVH_API void bvh_build_batched2(void* self, void* list, void* desc);
WHIWHI_BVH_API void bvh_copy(void* self, void* list, unsigned long long dest, unsigned long long src, int mode);
WHIWHI_BVH_API void bvh_prebuild_info(void* self, void* inputs, void* stream, void* out);
WHIWHI_BVH_API long bvh_rehydrate_batched2(void* self, void* list, void* desc, unsigned long long something, int flag);
WHIWHI_BVH_API void bvh_set_morton(void* self, int flag);

#ifdef __cplusplus
}
#endif

#endif // WHIWHI_SHELL_XBOX_BVH_BUILDER_H
