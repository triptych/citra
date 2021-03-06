// Copyright 2017 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <array>
#include <memory>
#include <vector>
#include "audio_core/audio_types.h"
#include "audio_core/dsp_interface.h"
#include "common/common_types.h"
#include "core/hle/service/dsp/dsp_dsp.h"
#include "core/memory.h"

namespace Memory {
class MemorySystem;
}

namespace AudioCore {

class DspHle final : public DspInterface {
public:
    explicit DspHle(Memory::MemorySystem& memory);
    ~DspHle();

    u16 RecvData(u32 register_number) override;
    bool RecvDataIsReady(u32 register_number) const override;
    void SetSemaphore(u16 semaphore_value) override;
    void PipeRead(DspPipe pipe_number, u32 length, std::vector<u8>& output) override;
    std::size_t GetPipeReadableSize(DspPipe pipe_number) const override;
    void PipeWrite(DspPipe pipe_number, const std::vector<u8>& buffer) override;

    std::array<u8, Memory::DSP_RAM_SIZE>& GetDspMemory() override;

    void SetServiceToInterrupt(std::weak_ptr<Service::DSP::DSP_DSP> dsp) override;

    void LoadComponent(const std::vector<u8>& buffer) override;
    void UnloadComponent() override;

private:
    struct Impl;
    friend struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace AudioCore
