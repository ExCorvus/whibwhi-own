// The sound chip object that AcpHalCreate hands out (acp_chip.cpp). It decodes the program's XMA2 voices into the
// voices' rings through FFmpeg's XMA2 decoder (scripts/setup_ffmpeg.ps1); the program mixes and plays them itself.
#ifndef WHIWHI_WEAVE_ACP_CHIP_HPP
#define WHIWHI_WEAVE_ACP_CHIP_HPP

namespace whiwhi::acp {

// The program's voice records (AcpHalAllocateShapeContexts): 64 bytes each.
void set_records(void* records, unsigned long long count);

// AcpHalCreate: writes the chip object and answers 0, or answers not implemented and writes nothing.
long create_chip(void** handle);

}  // namespace whiwhi::acp

#endif
