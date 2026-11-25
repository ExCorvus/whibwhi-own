# Every channel that can hand a heap / GPU address to the GPU (ExCorvus's "spread the architecture" idea)

Goal: cover ALL of them with ONE safe backing mechanism, instead of patching one at a time.
"safe" = no sparse DEVICE_ADDRESS_CAPTURE_REPLAY (that is what crashes the AMD driver / whole system).

## A. System memory (xmem.dll shims, runtime/shims/xmem.cpp)
| # | channel | what it produces | GPU reads it? | current coverage | safe backing |
|---|---------|------------------|---------------|------------------|--------------|
| 1 | XMemVirtualAlloc (reserve+commit) | fixed pools, CPU view, GPU-only view | yes | sparse in-place (CRASHES) | committed: non-sparse host import; GPU-only: pool-sourced mirror |
| 2 | XMemAlloc / XMemAllocDefault | sub-alloc inside a fixed pool | yes | only if pool exposed | same as pool |
| 3 | XMemAllocatePhysicalPages + XMemMapPhysicalPages | linear pool + re-mappable windows | yes | generalized sparse (CRASHES) | windows: pool-sourced mirror, re-copy on remap |
| 4 | XMemMapPhysicalPagesScatter | CPU view pages | CPU only | n/a (GPU uses slot 31) | none needed |
| 5 | XMemTransferMemory | moves working set | indirect | none | observe |

## B. Console graphics (d3d12_xs_device.cpp / d3d12_vulkan_device.cpp)
| # | channel | what it produces | current coverage | safe backing |
|---|---------|------------------|------------------|--------------|
| 6 | queue slot 31 CopyPageMappingsBatchX | GPU-view page <- pool page | sparse in-place (CRASHES) | pool-sourced mirror keyed by view page |
| 7 | device slot 14 CreateDescriptorHeap | console descriptor heap | mirror / import (safe) | keep |
| 8 | device slot 70 Create*View | V#/T# naming a title VA | aliases resolve/import (safe) | keep; source from pool when GPU-only |
| 9 | device slot 133 placed resource | buffer/texture at title VA | shadows (safe) | keep |
| 10 | device slot 74 | pool registration | none | observe |
| 11 | device slot 148 AllocateUploadMemory | upload arena | arena (safe) | keep |
| 12 | OpenExistingHeapFromAddress | host range as heap | non-sparse import (safe) | keep |
| 13 | D3D12 CreateCommittedResource / heaps | arena buffers | arena (safe) | keep |
| 14 | DirectStorage read into destination | title memory written by kernel | commit + aliases (safe) | keep |

## The one safe mechanism
- Stable committed ranges (1,2,12,13): import the host memory as a normal (non-sparse) VkBuffer with a device
  address; register in title_va_aliases so V#s/pointers rebase to it. Never re-mapped, so the import stays valid.
- Re-mappable / GPU-only ranges (1 GPU view,3 windows,6): do NOT import the re-mappable VA. Keep an arena mirror
  whose bytes are copied from the STABLE LINEAR POOL (always committed), keyed by view page; refresh when the game
  re-maps (slot 31 / XMemMapPhysicalPages). Rebase V#s to the mirror. This is the early-boot mirror mechanism (safe),
  sourced from the pool instead of the (unreadable) GPU-only VA.
- No sparse capture/replay anywhere => no driver / system crash.

## Why the system crashed
Sparse buffers created with DEVICE_ADDRESS_CAPTURE_REPLAY at chosen addresses, bound to host imports of re-mappable
placeholder windows, exercise a niche AMD kernel path; rapid bind/unbind + stale imports trigger a GPU timeout (TDR,
LiveKernelEvent 141) that resets the whole display stack. Confirmed by: crashes began only with the sparse in-place
commits; pre-sparse boots never reset the machine.
