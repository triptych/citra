// Copyright 2014 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <memory>
#include <utility>
#include "audio_core/dsp_interface.h"
#include "audio_core/hle/hle.h"
#include "audio_core/lle/lle.h"
#include "common/logging/log.h"
#include "common/texture.h"
#include "core/arm/arm_interface.h"
#if defined(ARCHITECTURE_x86_64) || defined(ARCHITECTURE_ARM64)
#include "core/arm/dynarmic/arm_dynarmic.h"
#endif
#include "core/arm/dyncom/arm_dyncom.h"
#include "core/cheats/cheats.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/custom_tex_cache.h"
#include "core/file_sys/archive_source_sd_savedata.h"
#include "core/gdbstub/gdbstub.h"
#include "core/hle/kernel/client_port.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/process.h"
#include "core/hle/kernel/thread.h"
#include "core/hle/service/fs/archive.h"
#include "core/hle/service/service.h"
#include "core/hle/service/sm/sm.h"
#include "core/hw/hw.h"
#include "core/loader/loader.h"
#include "core/movie.h"
#include "core/rpc/rpc_server.h"
#include "core/settings.h"
#include "network/network.h"
#include "video_core/video_core.h"

namespace Core {

/*static*/ System System::s_instance;

System::ResultStatus System::RunLoop(bool tight_loop) {
    // All cores should have executed the same amount of ticks. If this is not the case an event was
    // scheduled with a cycles_into_future smaller then the current downcount.
    // So we have to get those cores to the same global time first
    s64 max_delay = 0;
    std::shared_ptr<ARM_Interface> current_core_to_execute;
    for (auto& cpu_core : cpu_cores) {
        s64 delay = timing->GetGlobalTicks() - cpu_core->GetTimer()->GetTicks();
        if (delay > 0) {
            cpu_core->GetTimer()->Advance(delay);
            if (max_delay < delay) {
                max_delay = delay;
                current_core_to_execute = cpu_core;
            }
        }
    }

    if (max_delay > 4096) {
        running_core = current_core_to_execute.get();
        kernel->SetRunningCPU(current_core_to_execute);
        if (kernel->GetCurrentThreadManager().GetCurrentThread() == nullptr) {
            LOG_TRACE(Core_ARM11, "Core {} idling", current_core_to_execute->GetID());
            current_core_to_execute->GetTimer()->Idle();
            PrepareReschedule();
        } else {
            current_core_to_execute->Run();
        }
    } else {
        // Now all cores are at the same global time. So we will run them one after the other
        // with a max slice that is the minimum of all max slices of all cores
        // TODO: Make special check for idle since we can easily revert the time of idle cores
        s64 max_slice = Timing::MAX_SLICE_LENGTH;
        for (const auto& cpu_core : cpu_cores) {
            max_slice = std::min(max_slice, cpu_core->GetTimer()->GetMaxSliceLength());
        }
        for (auto& cpu_core : cpu_cores) {
            cpu_core->GetTimer()->Advance(max_slice);
        }
        for (auto& cpu_core : cpu_cores) {
            running_core = cpu_core.get();
            kernel->SetRunningCPU(cpu_core);
            // If we don't have a currently active thread then don't execute instructions,
            // instead advance to the next event and try to yield to the next thread
            if (kernel->GetCurrentThreadManager().GetCurrentThread() == nullptr) {
                LOG_TRACE(Core_ARM11, "Core {} idling", cpu_core->GetID());
                cpu_core->GetTimer()->Idle();
                PrepareReschedule();
            } else {
                cpu_core->Run();
            }
        }
        timing->AddToGlobalTicks(max_slice);
    }

    HW::Update();
    Reschedule();

    if (reset_requested.exchange(false)) {
        Reset();
    } else if (shutdown_requested.exchange(false)) {
        return ResultStatus::ShutdownRequested;
    }

    return status;
}

System::ResultStatus System::SingleStep() {
    return RunLoop(false);
}

System::ResultStatus System::Load(Frontend::EmuWindow& emu_window, const std::string& filepath) {
    app_loader = Loader::GetLoader(filepath);
    if (!app_loader) {
        LOG_CRITICAL(Core, "Failed to obtain loader for {}!", filepath);
        return ResultStatus::ErrorGetLoader;
    }
    std::pair<std::optional<u32>, Loader::ResultStatus> system_mode =
        app_loader->LoadKernelSystemMode();

    if (system_mode.second != Loader::ResultStatus::Success) {
        LOG_CRITICAL(Core, "Failed to determine system mode (Error {})!",
                     static_cast<int>(system_mode.second));

        switch (system_mode.second) {
        case Loader::ResultStatus::ErrorEncrypted:
            return ResultStatus::ErrorLoader_ErrorEncrypted;
        case Loader::ResultStatus::ErrorInvalidFormat:
            return ResultStatus::ErrorLoader_ErrorInvalidFormat;
        default:
            return ResultStatus::ErrorSystemMode;
        }
    }

    ASSERT(system_mode.first);
    auto n3ds_mode = app_loader->LoadKernelN3dsMode();
    ASSERT(n3ds_mode.first);
    ResultStatus init_result{Init(emu_window, *system_mode.first, *n3ds_mode.first)};
    if (init_result != ResultStatus::Success) {
        LOG_CRITICAL(Core, "Failed to initialize system (Error {})!",
                     static_cast<u32>(init_result));
        System::Shutdown();
        return init_result;
    }

    telemetry_session->AddInitialInfo(*app_loader);
    std::shared_ptr<Kernel::Process> process;
    const Loader::ResultStatus load_result{app_loader->Load(process)};
    kernel->SetCurrentProcess(process);
    if (Loader::ResultStatus::Success != load_result) {
        LOG_CRITICAL(Core, "Failed to load ROM (Error {})!", static_cast<u32>(load_result));
        System::Shutdown();

        switch (load_result) {
        case Loader::ResultStatus::ErrorEncrypted:
            return ResultStatus::ErrorLoader_ErrorEncrypted;
        case Loader::ResultStatus::ErrorInvalidFormat:
            return ResultStatus::ErrorLoader_ErrorInvalidFormat;
        default:
            return ResultStatus::ErrorLoader;
        }
    }
    cheat_engine = std::make_unique<Cheats::CheatEngine>(*this);
    u64 title_id{0};
    if (app_loader->ReadProgramId(title_id) != Loader::ResultStatus::Success) {
        LOG_ERROR(Core, "Failed to find title id for ROM (Error {})",
                  static_cast<u32>(load_result));
    }
    perf_stats = std::make_unique<PerfStats>();

    custom_tex_cache = std::make_unique<Core::CustomTexCache>();
    if (Settings::values.custom_textures) {
        FileUtil::CreateFullPath(fmt::format("{}textures/{:016X}/",
                                             FileUtil::GetUserPath(FileUtil::UserPath::LoadDir),
                                             title_id));
        custom_tex_cache->FindCustomTextures();
    }
    if (Settings::values.preload_textures)
        custom_tex_cache->PreloadTextures();

    status = ResultStatus::Success;
    m_emu_window = &emu_window;
    m_filepath = filepath;

    if (title_id == 0x0004000000068B00 || title_id == 0x0004000000061300 || title_id == 0x000400000004A700) {
        // hack for Tales of the Abyss / Pac Man Party 3D
        Settings::values.display_transfer_hack = true;
        // crash on `g_state.geometry_pipeline.Reconfigure();`
        // state.regs.pipeline.gs_unit_exclusive_configuration = 0
        // state.regs.gs.max_input_attribute_index = 0
        Settings::values.skip_slow_draw = true;
        // may cause display issues
        Settings::values.texture_load_hack = false;
    } else if (title_id == 0x00040000001D3A00) {
        // hack for Bloodstained: Curse of the Moon
        std::string sdmc_dir = FileUtil::GetUserPath(FileUtil::UserPath::SDMCDir);
        std::string path = FileSys::ArchiveSource_SDSaveData::GetSaveDataPathFor(sdmc_dir, title_id);
        FileUtil::CreateFullPath(path);
        FileUtil::IOFile(path + "SystemData.bin", "wb");
    }

    // Reset counters and set time origin to current frame
    GetAndResetPerfStats();
    perf_stats->BeginSystemFrame();
    return status;
}

void System::PrepareReschedule() {
    running_core->PrepareReschedule();
    reschedule_pending = true;
}

PerfStats::Results System::GetAndResetPerfStats() {
    return perf_stats->GetAndResetStats(timing->GetGlobalTimeUs());
}

void System::Reschedule() {
    if (!reschedule_pending) {
        return;
    }

    reschedule_pending = false;
    for (const auto& core : cpu_cores) {
        LOG_TRACE(Core_ARM11, "Reschedule core {}", core->GetID());
        kernel->GetThreadManager(core->GetID()).Reschedule();
    }
}

System::ResultStatus System::Init(Frontend::EmuWindow& emu_window, u32 system_mode, u8 n3ds_mode) {
    LOG_DEBUG(HW_Memory, "initialized OK");

    std::size_t num_cores = 2;
    if (Settings::values.is_new_3ds) {
        num_cores = 4;
    }

    memory = std::make_unique<Memory::MemorySystem>();

    timing = std::make_unique<Timing>(num_cores);

    kernel = std::make_unique<Kernel::KernelSystem>(
        *memory, *timing, [this] { PrepareReschedule(); }, system_mode, num_cores, n3ds_mode);

    if (Settings::values.use_cpu_jit) {
#if defined(ARCHITECTURE_x86_64) || defined(ARCHITECTURE_ARM64)
        for (std::size_t i = 0; i < num_cores; ++i) {
            cpu_cores.push_back(
                std::make_shared<ARM_Dynarmic>(this, *memory, USER32MODE, i, timing->GetTimer(i)));
        }
#else
        for (std::size_t i = 0; i < num_cores; ++i) {
            cpu_cores.push_back(
                std::make_shared<ARM_DynCom>(this, *memory, USER32MODE, i, timing->GetTimer(i)));
        }
        LOG_WARNING(Core, "CPU JIT requested, but Dynarmic not available");
#endif
    } else {
        for (std::size_t i = 0; i < num_cores; ++i) {
            cpu_cores.push_back(
                std::make_shared<ARM_DynCom>(this, *memory, USER32MODE, i, timing->GetTimer(i)));
        }
    }
    running_core = cpu_cores[0].get();

    kernel->SetCPUs(cpu_cores);
    kernel->SetRunningCPU(cpu_cores[0]);

    if (Settings::values.enable_dsp_lle) {
        dsp_core = std::make_unique<AudioCore::DspLle>(*memory,
                                                       Settings::values.enable_dsp_lle_multithread);
    } else {
        dsp_core = std::make_unique<AudioCore::DspHle>(*memory);
    }

    memory->SetDSP(*dsp_core);

    dsp_core->SetSink(Settings::values.sink_id, Settings::values.audio_device_id);
    dsp_core->EnableStretching(Settings::values.enable_audio_stretching);

    telemetry_session = std::make_unique<Core::TelemetrySession>();

    rpc_server = std::make_unique<RPC::RPCServer>();

    service_manager = std::make_shared<Service::SM::ServiceManager>(*this);
    archive_manager = std::make_unique<Service::FS::ArchiveManager>(*this);

    HW::Init(*memory);
    Service::Init(*this);
    GDBStub::Init();

    ResultStatus result = VideoCore::Init(emu_window, *memory);
    if (result != ResultStatus::Success) {
        return result;
    }

    LOG_DEBUG(Core, "Initialized OK");

    initalized = true;

    return ResultStatus::Success;
}

RendererBase& System::Renderer() {
    return *VideoCore::g_renderer;
}

Service::SM::ServiceManager& System::ServiceManager() {
    return *service_manager;
}

const Service::SM::ServiceManager& System::ServiceManager() const {
    return *service_manager;
}

Service::FS::ArchiveManager& System::ArchiveManager() {
    return *archive_manager;
}

const Service::FS::ArchiveManager& System::ArchiveManager() const {
    return *archive_manager;
}

Kernel::KernelSystem& System::Kernel() {
    return *kernel;
}

const Kernel::KernelSystem& System::Kernel() const {
    return *kernel;
}

Timing& System::CoreTiming() {
    return *timing;
}

const Timing& System::CoreTiming() const {
    return *timing;
}

Memory::MemorySystem& System::Memory() {
    return *memory;
}

const Memory::MemorySystem& System::Memory() const {
    return *memory;
}

Cheats::CheatEngine& System::CheatEngine() {
    return *cheat_engine;
}

const Cheats::CheatEngine& System::CheatEngine() const {
    return *cheat_engine;
}

Core::CustomTexCache& System::CustomTexCache() {
    return *custom_tex_cache;
}

const Core::CustomTexCache& System::CustomTexCache() const {
    return *custom_tex_cache;
}

void System::RegisterMiiSelector(std::shared_ptr<Frontend::MiiSelector> mii_selector) {
    registered_mii_selector = std::move(mii_selector);
}

void System::RegisterSoftwareKeyboard(std::shared_ptr<Frontend::SoftwareKeyboard> swkbd) {
    registered_swkbd = std::move(swkbd);
}

void System::RegisterImageInterface(std::shared_ptr<Frontend::ImageInterface> image_interface) {
    registered_image_interface = std::move(image_interface);
}

void System::Shutdown() {
    // Log last frame performance stats
    const auto perf_results = GetAndResetPerfStats();
    telemetry_session->AddField(Telemetry::FieldType::Performance, "Shutdown_EmulationSpeed",
                                perf_results.emulation_speed * 100.0);
    telemetry_session->AddField(Telemetry::FieldType::Performance, "Shutdown_Framerate",
                                perf_results.game_fps);
    telemetry_session->AddField(Telemetry::FieldType::Performance, "Shutdown_Frametime",
                                perf_results.frametime * 1000.0);
    telemetry_session->AddField(Telemetry::FieldType::Performance, "Mean_Frametime_MS", 20.0);

    // Shutdown emulation session
    GDBStub::Shutdown();
    VideoCore::Shutdown();
    HW::Shutdown();
    telemetry_session.reset();
    perf_stats.reset();
    rpc_server.reset();
    cheat_engine.reset();
    archive_manager.reset();
    service_manager.reset();
    dsp_core.reset();
    cpu_cores.clear();
    kernel.reset();
    timing.reset();
    memory.reset();
    app_loader.reset();
    custom_tex_cache.reset();

    if (auto room_member = Network::GetRoomMember().lock()) {
        Network::GameInfo game_info{};
        room_member->SendGameInfo(game_info);
    }

    LOG_DEBUG(Core, "Shutdown OK");
}

void System::Reset() {
    // This is NOT a proper reset, but a temporary workaround by shutting down the system and
    // reloading.
    // TODO: Properly implement the reset

    Shutdown();
    // Reload the system with the same setting
    Load(*m_emu_window, m_filepath);
}

} // namespace Core
