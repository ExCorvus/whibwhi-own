# Every channel that can hand a memory / GPU address to the GPU

Read from the original executable's own calls (see MAP.md). The shell answers all of them through one model:
the game's memory is host memory on the PC, and the shell shows it to the GPU when graphics calls name it. What
is banned everywhere: sparse buffers with `VK_BUFFER_CREATE_DEVICE_ADDRESS_CAPTURE_REPLAY_BIT` (it crashes the
AMD driver / whole system) and off-thread async pipeline builds.

## A. System memory (the image's xmem.dll calls, answered by shell/xmem.*)

| # | channel | what it produces | who reads it |
|---|---------|------------------|--------------|
| 1 | XMemVirtualAlloc (reserve+commit) | fixed pools, CPU view, GPU-only view | both; the GPU view is read by the GPU |
| 2 | XMemAlloc / XMemAllocDefault | sub-alloc inside a fixed pool | both |
| 3 | XMemAllocatePhysicalPages + XMemMapPhysicalPages / Scatter | linear pool + re-mappable windows | CPU view by the CPU; pool by the GPU |
| 4 | XMemTransferMemory | moves working set | indirect |

## B. Console graphics (the device/queue vtable calls, answered by shell/d3d12_xs.*)

| # | channel | what it produces | who reads it |
|---|---------|------------------|--------------|
| 6 | queue slot 31 CopyPageMappingsBatchX | GPU-view page <- pool page | the GPU |
| 7 | device slot 11 CreateDescriptorHeap | console descriptor heap | both |
| 8 | device slots 13-18 Create*View | V#/T# naming a title address | both |
| 9 | device slot 25 CreatePlacedResource | buffer/texture at a title address | both |
| 10 | device slot 74 | pool registration | the GPU |
| 11 | device slot 148 AllocateUploadMemory | upload arena | the GPU |
| 12 | device slots 78-81 frame events, queue slots 30/32 PresentX | frame cadence and present | the display |

## How the shell covers them

- The memory library keeps every region record with its type, protect (graphics bits included) and attributes,
  and can enumerate them (`whiwhi_xmem_regions`): that registry is where the GPU side plugs in.
- The graphics library wraps a real PC D3D12 device; the console-only vtable slots are answered by the shell
  (logged the first time each is called), so a boot shows exactly which channels the game uses.
- No sparse capture/replay anywhere, no async pipeline builds: the two paths that crash this driver.
