#include "pocketwalker.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <fstream>
#include <thread>

#include "core/utils/logger.h"
#include "core/soc/defines.h"
#include "core/soc/memory/regions/io.h"
#include "core/soc/memory/regions/ram.h"

namespace
{
constexpr std::array<char, 8> STATE_MAGIC = {'P', 'W', 'S', 'T', 'A', 'T', '0', '6'};
constexpr std::array<char, 8> LEGACY_STATE_MAGIC = {'P', 'W', 'S', 'T', 'A', 'T', '0', '5'};
constexpr uint16_t LEGACY_SSD1854_TOTAL_PAGES = 22;
constexpr uint16_t LEGACY_SSD1854_MEM_SIZE = 128 * 2 * LEGACY_SSD1854_TOTAL_PAGES;
constexpr uint32_t MAX_REASONABLE_STEPS = 9999999;
constexpr auto RTC_CATCH_UP_STEP_STABLE_DELAY = std::chrono::milliseconds(1200);
constexpr auto RTC_CATCH_UP_STEP_MAX_WAIT = std::chrono::seconds(8);
constexpr auto RTC_CATCH_UP_SETTLE_SAMPLE_INTERVAL = std::chrono::milliseconds(1);
constexpr auto RTC_CATCH_UP_SETTLE_STABLE_DELAY = std::chrono::milliseconds(3);
constexpr auto RTC_CATCH_UP_SETTLE_MAX_WAIT = std::chrono::milliseconds(25);
constexpr uint16_t RTC_CATCH_UP_TURBO_BATCH = 512;
#ifdef __ANDROID__
constexpr uint16_t NORMAL_EMULATION_BATCH = 512;
#else
constexpr uint16_t NORMAL_EMULATION_BATCH = 1;
#endif
constexpr uint16_t RTC_CATCH_UP_RAM_HASH_START = 0xF780;
constexpr uint16_t RTC_CATCH_UP_RAM_HASH_SIZE = 0x100;
constexpr auto PEER_COOLDOWN_NORMALIZE_DELAY = std::chrono::milliseconds(1500);
constexpr uint16_t EEPROM_MET_PEER_LIST_START = 0xDE24;
constexpr uint16_t EEPROM_MET_PEER_ENTRY_SIZE = 0x224;
constexpr uint16_t EEPROM_MET_PEER_COMPARE_OFFSET = 8;
constexpr uint16_t EEPROM_MET_PEER_COMPARE_SIZE = 0x28;
constexpr uint8_t EEPROM_MET_PEER_ENTRY_COUNT = 10;

template <typename T>
void WriteValue(std::ostream& stream, const T& value)
{
    stream.write(reinterpret_cast<const char*>(&value), sizeof(T));
}

template <typename T>
bool ReadValue(std::istream& stream, T& value)
{
    stream.read(reinterpret_cast<char*>(&value), sizeof(T));
    return static_cast<bool>(stream);
}

void WriteMemoryRange(std::ostream& stream, const std::shared_ptr<MemoryInterface>& memory, const uint16_t start, const size_t size)
{
    for (size_t i = 0; i < size; i++)
    {
        auto ptr = memory->Ptr8(start + static_cast<uint16_t>(i));
        const uint8_t value = ptr.ptr ? *ptr : 0xFF;
        WriteValue(stream, value);
    }
}

bool ReadMemoryRange(std::istream& stream, const std::shared_ptr<MemoryInterface>& memory, const uint16_t start, const size_t size)
{
    for (size_t i = 0; i < size; i++)
    {
        uint8_t value = 0;
        if (!ReadValue(stream, value))
            return false;

        auto ptr = memory->Ptr8(start + static_cast<uint16_t>(i));
        if (ptr.ptr)
            ptr = value;
    }

    return true;
}

void HashByte(uint64_t& hash, const uint8_t value)
{
    hash ^= value;
    hash *= 1099511628211ULL;
}

void WriteCpuState(std::ostream& stream, const std::shared_ptr<CPU>& cpu)
{
    WriteValue(stream, cpu->reg.PC);
    WriteValue(stream, cpu->reg.flags.CCR);
    for (uint8_t i = 0; i < 8; i++)
        WriteValue(stream, *cpu->reg.Reg32(i));
    WriteValue(stream, cpu->sleep);
}

bool ReadCpuState(std::istream& stream, const std::shared_ptr<CPU>& cpu)
{
    if (!ReadValue(stream, cpu->reg.PC))
        return false;

    if (!ReadValue(stream, cpu->reg.flags.CCR))
        return false;

    for (uint8_t i = 0; i < 8; i++)
    {
        uint32_t value = 0;
        if (!ReadValue(stream, value))
            return false;
        *cpu->reg.Reg32(i) = value;
    }

    return ReadValue(stream, cpu->sleep);
}

void WriteDisplayState(std::ostream& stream, const std::shared_ptr<NT7508>& display)
{
    NT7508DrawInfo draw_info = display->GetDrawInfoSnapshot();
    for (uint16_t i = 0; i < NT7508_MEM_SIZE; i++)
        WriteValue(stream, draw_info.vram.Read8(i));

    WriteValue(stream, draw_info.initial_display_line);
    WriteValue(stream, draw_info.contrast);
    WriteValue(stream, draw_info.power_save);
}

bool ReadDisplayState(std::istream& stream, const std::shared_ptr<NT7508>& display, const bool legacy_state)
{
    const uint16_t stored_size = legacy_state ? LEGACY_SSD1854_MEM_SIZE : NT7508_MEM_SIZE;
    for (uint16_t i = 0; i < stored_size; i++)
    {
        uint8_t value = 0;
        if (!ReadValue(stream, value))
            return false;
        if (i < NT7508_MEM_SIZE)
            display->draw_info.vram.Write8(i, value);
    }

    uint8_t stored_display_line = 0;
    if (!ReadValue(stream, stored_display_line) ||
        !ReadValue(stream, display->draw_info.contrast) ||
        !ReadValue(stream, display->draw_info.power_save))
    {
        return false;
    }

    display->draw_info.initial_display_line = legacy_state
        ? static_cast<uint8_t>((stored_display_line * 8) & NT7508_INITIAL_DISPLAY_LINE_MAX)
        : static_cast<uint8_t>(stored_display_line & NT7508_INITIAL_DISPLAY_LINE_MAX);
    display->draw_info.contrast &= 0x3F;
    return true;
}
}

PocketWalker::PocketWalker(RomBuffer rom_buffer)
{
    this->soc = std::make_shared<H838606>(rom_buffer);


    this->bma150 = std::make_shared<BMA150>();
    this->soc->ssu->RegisterPeripheral(this->bma150, SSU_ADDR_PDR9, 0);
    this->soc->ssu->RegisterOutputPin(this->bma150, BMA150_PIN_INT, SSU_ADDR_PDRB, 1);

    this->step_provider = std::make_shared<StepSampleProvider>();
    this->bma150->SetSampleProvider(this->step_provider);

    this->m95512 = std::make_shared<M95512>();
    this->soc->ssu->RegisterPeripheral(this->m95512, SSU_ADDR_PDR1, 2);

    this->nt7508 = std::make_shared<NT7508>();
    this->soc->ssu->RegisterPeripheral(this->nt7508, SSU_ADDR_PDR1, 0);
    this->soc->ssu->RegisterInputPin(this->nt7508, SSU_ADDR_PDR1, 1, NT7508_PIN_DC);

    this->buzzer = std::make_shared<Buzzer>(this->soc->timer_w);

    this->activity_timer_bypass = std::make_shared<ActivityTimerBypass>(this->soc->memory);
}

void PocketWalker::Start()
{
    constexpr std::chrono::duration<long long, std::nano> CYCLE_DURATION(1'000'000'000LL / PHI_CLK);

    auto next = std::chrono::high_resolution_clock::now();
    bool prev_fast_mode = is_fast_mode;
    bool prev_paused = is_paused;
    bool prev_rtc_catch_up_active = false;
    const auto rtc_catch_up_step_wait_started = std::chrono::steady_clock::now();
    last_observed_total_days = soc->memory->Read16(PW_ADDR_TOTAL_DAYS);
    last_logged_total_steps = soc->memory->Read32(PW_ADDR_TOTAL_STEPS);
    last_logged_session_steps = soc->memory->Read32(PW_ADDR_SESSION_STEPS);
    last_logged_synthetic_steps = step_provider->is_enabled;
    last_logged_synthetic_sleep_probe = false;
    rtc_catch_up_was_active = soc->rtc->IsCatchUpActive();
    rtc_catch_up_settle_tracking = false;
    rtc_catch_up_settle_hash = 0;
    rtc_catch_up_settle_started_at = {};
    rtc_catch_up_settle_since = {};
    rtc_catch_up_next_settle_check = {};

    this->is_running = true;
    while (this->is_running)
    {
        if (this->is_paused)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            prev_paused = true;
            continue;
        }

        if (prev_paused)
            next = std::chrono::high_resolution_clock::now();

        prev_paused = false;

        uint32_t elapsed_cycles = 0;
        const bool catch_up_turbo = soc->rtc->IsCatchUpActive();
        const uint16_t batch_size = catch_up_turbo ? RTC_CATCH_UP_TURBO_BATCH : NORMAL_EMULATION_BATCH;

        for (uint16_t i = 0; i < batch_size && this->is_running && !this->is_paused; i++)
        {
            const uint16_t exec_pc = soc->cpu->reg.PC;
            switch (exec_pc)
            {
                case 0x945A:
                    motion_batches_started.fetch_add(1, std::memory_order_relaxed);
                    break;
                case 0x970C:
                    motion_last_candidate_scaled.store(
                        *soc->cpu->reg.Reg16(0), std::memory_order_relaxed);
                    motion_last_spectrum_maximum.store(
                        *soc->cpu->reg.Reg16(9), std::memory_order_relaxed);
                    break;
                case 0x971E:
                    motion_batches_rejected.fetch_add(1, std::memory_order_relaxed);
                    break;
                case 0x9732:
                    motion_batches_accepted.fetch_add(1, std::memory_order_relaxed);
                    motion_last_accepted_steps_q9.store(
                        *soc->cpu->reg.Reg16(0), std::memory_order_relaxed);
                    break;
                case 0x95D0:
                    motion_step_awards.fetch_add(1, std::memory_order_relaxed);
                    break;
                default:
                    break;
            }

            const uint8_t cycles = soc->Cycle();
            elapsed_cycles += cycles;
            CyclePeripherals(cycles);
            CycleEnhancements(cycles);
            UpdateRtcCatchUpFirmwareSettle();
            UpdateDailyPeerCooldownNormalization();

            const uint32_t total_steps = soc->memory->Read32(PW_ADDR_TOTAL_STEPS);
            const uint32_t session_steps = soc->memory->Read32(PW_ADDR_SESSION_STEPS);
            const bool synthetic_steps = step_provider->is_enabled;
            if (total_steps != last_logged_total_steps ||
                session_steps != last_logged_session_steps ||
                synthetic_steps != last_logged_synthetic_steps)
            {
                soc->rtc->DebugMessage("step_debug synthetic=" + std::string(synthetic_steps ? "true" : "false") +
                                       " total_steps=" + std::to_string(total_steps) +
                                       " session_steps=" + std::to_string(session_steps) +
                                       " cpu_sleep=" + (soc->cpu->sleep ? std::string("true") : std::string("false")) +
                                       " ssu_clock_running=" + (soc->CKSTPR2.SSUCKSTP ? std::string("true") : std::string("false")) +
                                       " pdrb=" + std::to_string(soc->ssu->PDRB.VALUE));
                last_logged_total_steps = total_steps;
                last_logged_session_steps = session_steps;
                last_logged_synthetic_steps = synthetic_steps;
            }

            const bool synthetic_sleep_probe = synthetic_steps && soc->cpu->sleep;
            if (synthetic_sleep_probe && !last_logged_synthetic_sleep_probe)
            {
                soc->rtc->DebugMessage("synthetic_sleep_probe total_steps=" + std::to_string(total_steps) +
                                       " session_steps=" + std::to_string(session_steps) +
                                       " ienr1=" + std::to_string(soc->interrupts->IENR1.VALUE) +
                                       " irr1=" + std::to_string(soc->interrupts->IRR1.VALUE) +
                                       " pfcr=" + std::to_string(soc->ssu->PFCR.VALUE) +
                                       " pdrb=" + std::to_string(soc->ssu->PDRB.VALUE) +
                                       " bma_control1=" + std::to_string(*bma150->control1));
            }
            last_logged_synthetic_sleep_probe = synthetic_sleep_probe;

            if (catch_up_turbo && !soc->rtc->IsCatchUpActive())
                break;
        }

        if (rtc_catch_up_waiting_for_steps)
        {
            const auto now = std::chrono::steady_clock::now();
            const uint32_t total_steps = soc->memory->Read32(PW_ADDR_TOTAL_STEPS);
            const uint32_t session_steps = soc->memory->Read32(PW_ADDR_SESSION_STEPS);

            if (total_steps != rtc_catch_up_last_total_steps || session_steps != rtc_catch_up_last_session_steps)
            {
                rtc_catch_up_last_total_steps = total_steps;
                rtc_catch_up_last_session_steps = session_steps;
                rtc_catch_up_steps_stable_since = now;
            }

            if (now - rtc_catch_up_steps_stable_since >= RTC_CATCH_UP_STEP_STABLE_DELAY ||
                now - rtc_catch_up_step_wait_started >= RTC_CATCH_UP_STEP_MAX_WAIT)
            {
                rtc_catch_up_waiting_for_steps = false;
                soc->rtc->AllowCatchUpToRun(true);
            }
        }

        next += CYCLE_DURATION * elapsed_cycles;

        const bool is_rtc_catch_up_active = soc->rtc->IsCatchUpActive();
        if (!is_fast_mode && !is_rtc_catch_up_active)
        {
            if (prev_fast_mode || prev_rtc_catch_up_active)
                next = std::chrono::high_resolution_clock::now();
            std::this_thread::sleep_until(next);
        }
        else
        {
            next = std::chrono::high_resolution_clock::now();
        }

        prev_fast_mode = is_fast_mode;
        prev_rtc_catch_up_active = is_rtc_catch_up_active;
    }
}

void PocketWalker::Stop()
{
    this->is_running = false;
}

void PocketWalker::SetWatts(uint16_t value)
{
    this->soc->memory->Write16(PW_ADDR_WATTS, value);
}

void PocketWalker::SetSessionSteps(uint32_t value)
{
    this->soc->memory->Write32(PW_ADDR_SESSION_STEPS, value);
    this->soc->memory->Write32(PW_ADDR_TOTAL_STEPS, value);
}

void PocketWalker::UseSyntheticSteps(bool value)
{
    const bool was_enabled = this->step_provider->is_enabled;
    this->step_provider->SetContinuous(value);
    soc->rtc->DebugMessage("UseSyntheticSteps requested=" + std::string(value ? "true" : "false") +
                           " previous=" + (was_enabled ? std::string("true") : std::string("false")) +
                           " total_steps=" + std::to_string(soc->memory->Read32(PW_ADDR_TOTAL_STEPS)) +
                           " session_steps=" + std::to_string(soc->memory->Read32(PW_ADDR_SESSION_STEPS)) +
                           " cpu_sleep=" + (soc->cpu->sleep ? std::string("true") : std::string("false")) +
                           " ssu_clock_running=" + (soc->CKSTPR2.SSUCKSTP ? std::string("true") : std::string("false")) +
                           " ienr1=" + std::to_string(soc->interrupts->IENR1.VALUE) +
                           " irr1=" + std::to_string(soc->interrupts->IRR1.VALUE) +
                           " pfcr=" + std::to_string(soc->ssu->PFCR.VALUE) +
                           " bma_control1=" + std::to_string(*bma150->control1));
    (void)was_enabled;
}

void PocketWalker::UseExternalAccelerometer(const bool value)
{
    step_provider->SetExternalMode(value);
}

void PocketWalker::SetExternalAcceleration(const float x, const float y, const float z)
{
    const auto convert = [](const float acceleration) {
        constexpr float STANDARD_GRAVITY = 9.80665f;
        constexpr float BMA150_COUNTS_PER_G = 256.0f;
        const long value = std::lround(
            (acceleration / STANDARD_GRAVITY) * BMA150_COUNTS_PER_G);
        return static_cast<int16_t>(std::clamp(value, -512L, 511L));
    };

    step_provider->SetExternalSample(convert(x), convert(y), convert(z));
}

bool PocketWalker::PulseExternalMotion()
{
    return step_provider->PulseExternalMotion();
}

uint64_t PocketWalker::ExternalAccelerometerReadCount() const
{
    return step_provider->ExternalReadCount();
}

uint64_t PocketWalker::MotionBatchesAccepted() const
{
    return motion_batches_accepted.load(std::memory_order_relaxed);
}

uint64_t PocketWalker::MotionStepAwards() const
{
    return motion_step_awards.load(std::memory_order_relaxed);
}

uint16_t PocketWalker::MotionLastAcceptedStepsQ9() const
{
    return motion_last_accepted_steps_q9.load(std::memory_order_relaxed);
}

std::string PocketWalker::GetMotionDiagnostics() const
{
    const auto sample = step_provider->ExternalSample();
    const auto output_sample = step_provider->ExternalOutputSample();
    const uint8_t next_sample = soc->memory->Read8(0xF7AE) & 0x3F;
    const uint8_t latest_sample = (next_sample + 0x3F) & 0x3F;
    const uint8_t prior_sample = (next_sample + 0x3E) & 0x3F;
    const auto signed_sample = [this](const uint16_t address) {
        return static_cast<int>(static_cast<int8_t>(soc->memory->Read8(address)));
    };
    const int latest_x = signed_sample(static_cast<uint16_t>(0xF826 + latest_sample));
    const int latest_y = signed_sample(static_cast<uint16_t>(0xF866 + latest_sample));
    const int latest_z = signed_sample(static_cast<uint16_t>(0xF8A6 + latest_sample));
    const int prior_x = signed_sample(static_cast<uint16_t>(0xF826 + prior_sample));
    const int prior_y = signed_sample(static_cast<uint16_t>(0xF866 + prior_sample));
    const int prior_z = signed_sample(static_cast<uint16_t>(0xF8A6 + prior_sample));
    const int firmware_activity = std::abs(latest_x - prior_x) +
                                  std::abs(latest_y - prior_y) +
                                  std::abs(latest_z - prior_z);
    uint16_t firmware_spectrum_maximum = 0;
    uint16_t firmware_walking_candidate = 0;
    uint8_t firmware_spectrum_bin = 0;
    uint8_t firmware_walking_bin = 0;
    for (uint16_t bin = 1; bin <= 29; bin++)
    {
        const uint16_t magnitude = soc->memory->Read16(
            static_cast<uint16_t>(0xF7E6 + bin * sizeof(uint16_t)));
        if (magnitude > firmware_spectrum_maximum)
        {
            firmware_spectrum_maximum = magnitude;
            firmware_spectrum_bin = static_cast<uint8_t>(bin);
        }
        if (bin >= 5 && bin <= 14 && magnitude > firmware_walking_candidate)
        {
            firmware_walking_candidate = magnitude;
            firmware_walking_bin = static_cast<uint8_t>(bin);
        }
    }

    uint32_t host_spectrum_maximum = 0;
    uint32_t host_walking_candidate = 0;
    uint8_t host_spectrum_bin = 0;
    uint8_t host_walking_bin = 0;
    constexpr double TWO_PI = 6.28318530717958647692;
    for (uint8_t bin = 1; bin < 30; bin++)
    {
        double magnitude = 0.0;
        for (const uint16_t axis_address : {uint16_t{0xF826}, uint16_t{0xF866}, uint16_t{0xF8A6}})
        {
            double real = 0.0;
            double imaginary = 0.0;
            for (uint8_t sample_index = 0; sample_index < 64; sample_index++)
            {
                const auto value = static_cast<int8_t>(
                    soc->memory->Read8(static_cast<uint16_t>(axis_address + sample_index)));
                const double angle = TWO_PI * static_cast<double>(bin) * sample_index / 64.0;
                real += static_cast<double>(value) * std::cos(angle);
                imaginary += static_cast<double>(value) * std::sin(angle);
            }
            magnitude += std::abs(real) + std::abs(imaginary);
        }

        const uint32_t rounded = static_cast<uint32_t>(std::lround(magnitude));
        if (rounded > host_spectrum_maximum)
        {
            host_spectrum_maximum = rounded;
            host_spectrum_bin = bin;
        }
        if (bin >= 5 && bin <= 14 && rounded > host_walking_candidate)
        {
            host_walking_candidate = rounded;
            host_walking_bin = bin;
        }
    }
    return "bma_reads=" + std::to_string(step_provider->ExternalReadCount()) +
           " sample=" + std::to_string(sample.x) + "," +
           std::to_string(sample.y) + "," + std::to_string(sample.z) +
           " bma_sample=" + std::to_string(output_sample.x) + "," +
           std::to_string(output_sample.y) + "," + std::to_string(output_sample.z) +
           " bma_irq_cfg=" + std::to_string(bma150->ReadRegister(0x0B)) +
           " bma_motion_threshold=" + std::to_string(bma150->ReadRegister(0x10)) +
           " bma_motion_duration=" + std::to_string(bma150->ReadRegister(0x11)) +
           " bma_range_bw=" + std::to_string(bma150->ReadRegister(0x14)) +
           " bma_mode_cfg=" + std::to_string(bma150->ReadRegister(0x15)) +
           " bma_protected_1e=" + std::to_string(bma150->ReadRegister(0x1E)) +
           " bma_enabled=" + (step_provider->is_enabled.load() ? std::string("true") : "false") +
           " motion_active=" + (step_provider->ExternalMotionActive() ? std::string("true") : "false") +
           " motion_pulses=" + std::to_string(step_provider->ExternalMotionPulseCount()) +
           " motion_remaining_ms=" + std::to_string(step_provider->ExternalMotionRemainingMilliseconds()) +
           " motion_delta=" + std::to_string(step_provider->ExternalMaximumDelta()) +
           " motion_magnitude2=" + std::to_string(step_provider->ExternalMagnitudeSquared()) +
           " bma_conversions=" + std::to_string(bma150->ConversionCount()) +
           " bma_interrupts=" + std::to_string(bma150->InterruptCount()) +
           " bma_bursts=" + std::to_string(bma150->BurstReadCount()) +
           " bma_writes=" + std::to_string(bma150->ControlWriteCount()) +
           " bma_control=" + std::to_string(*bma150->control1) +
           " firmware_sample_slot=" + std::to_string(next_sample) +
           " firmware_latest=" + std::to_string(latest_x) + "," +
           std::to_string(latest_y) + "," + std::to_string(latest_z) +
           " firmware_activity=" + std::to_string(firmware_activity) +
           " firmware_fft_candidate=" + std::to_string(firmware_walking_bin) + ":" +
           std::to_string(firmware_walking_candidate) +
           " firmware_fft_max=" + std::to_string(firmware_spectrum_bin) + ":" +
           std::to_string(firmware_spectrum_maximum) +
           " host_fft_candidate=" + std::to_string(host_walking_bin) + ":" +
           std::to_string(host_walking_candidate) +
           " host_fft_max=" + std::to_string(host_spectrum_bin) + ":" +
           std::to_string(host_spectrum_maximum) +
           " firmware_batches=" + std::to_string(motion_batches_started.load(std::memory_order_relaxed)) +
           " firmware_accepted=" + std::to_string(motion_batches_accepted.load(std::memory_order_relaxed)) +
           " firmware_rejected=" + std::to_string(motion_batches_rejected.load(std::memory_order_relaxed)) +
           " firmware_awards=" + std::to_string(motion_step_awards.load(std::memory_order_relaxed)) +
           " firmware_compare_scaled=" + std::to_string(motion_last_candidate_scaled.load(std::memory_order_relaxed)) +
           " firmware_compare_max=" + std::to_string(motion_last_spectrum_maximum.load(std::memory_order_relaxed)) +
           " firmware_not_walking=" + std::to_string(soc->memory->Read8(0xF8EF)) +
           " firmware_events=" + std::to_string(soc->memory->Read8(0xF7B5)) +
           " firmware_flags=" + std::to_string(soc->memory->Read8(0xF7B6)) +
           " session_steps=" + std::to_string(soc->memory->Read32(PW_ADDR_SESSION_STEPS)) +
           " total_steps=" + std::to_string(soc->memory->Read32(PW_ADDR_TOTAL_STEPS)) +
           " cpu_sleep=" + (soc->cpu->sleep ? std::string("true") : "false") +
           " ienr1=" + std::to_string(soc->interrupts->IENR1.VALUE) +
           " irr1=" + std::to_string(soc->interrupts->IRR1.VALUE);
}

void PocketWalker::UseFastMode(bool value)
{
    this->is_fast_mode = value;
}

void PocketWalker::SetBypassPowerSave(bool value)
{
    this->bypass_power_save = value;
}

void PocketWalker::SetPause(bool value)
{
    this->is_paused = value;
}

void PocketWalker::OnSamplePushed(const EventHandlerCallback<BuzzerInformation>& callback)
{
    this->buzzer->OnSamplePushed += callback;
}

void PocketWalker::OnTransmitIR(const EventHandlerCallback<uint8_t>& callback)
{
    this->soc->sci3->OnTransmitIR(callback);
}

void PocketWalker::ReceiveIR(const uint8_t data)
{
    this->soc->sci3->ReceiveIR(data);
}

NT7508DrawInfo PocketWalker::GetDrawInfo() const
{
    return this->nt7508->GetDrawInfoSnapshot();
}

void PocketWalker::PressButton(ButtonType button) const
{
    const uint8_t current = soc->memory->Read8(SSU_ADDR_PDRB);
    soc->memory->Write8(SSU_ADDR_PDRB, current | static_cast<uint8_t>(button));
}

void PocketWalker::ReleaseButton(ButtonType button) const
{
    const uint8_t current = soc->memory->Read8(SSU_ADDR_PDRB);
    soc->memory->Write8(SSU_ADDR_PDRB, current & ~static_cast<uint8_t>(button));
}

EepromBuffer PocketWalker::GetEepromBuffer() const
{
    return m95512->eeprom;
}

void PocketWalker::SetEepromBuffer(const EepromBuffer& buffer) const
{
    m95512->eeprom = buffer;
}

uint32_t PocketWalker::GetVolatileStepCount() const
{
    const uint32_t session_steps = this->soc->memory->Read32(PW_ADDR_SESSION_STEPS);
    const uint32_t total_steps = this->soc->memory->Read32(PW_ADDR_TOTAL_STEPS);

    if (session_steps > 0 && session_steps <= MAX_REASONABLE_STEPS)
        return session_steps;

    if (total_steps > 0 && total_steps <= MAX_REASONABLE_STEPS)
        return total_steps;

    return 0;
}

uint16_t PocketWalker::GetVolatileWatts() const
{
    return this->soc->memory->Read16(PW_ADDR_WATTS);
}

void PocketWalker::RestoreVolatileCounters(uint32_t steps, uint16_t watts) const
{
    if (steps > 0 && steps <= MAX_REASONABLE_STEPS)
    {
        this->soc->memory->Write32(PW_ADDR_SESSION_STEPS, steps);
        this->soc->memory->Write32(PW_ADDR_TOTAL_STEPS, steps);
    }

    if (watts > 0)
        this->soc->memory->Write16(PW_ADDR_WATTS, watts);
}

void PocketWalker::LoadRtcState(const std::string& path) const
{
    soc->rtc->LoadState(path);
}

NT7508DrawInfo PocketWalker::GetDrawInfoSnapshot() const
{
    return this->nt7508->GetDrawInfoSnapshot();
}

void PocketWalker::LoadRtcState(std::istream& stream, const std::filesystem::path& base_directory) const
{
    soc->rtc->LoadState(stream, base_directory);
}

void PocketWalker::SaveRtcState(const std::string& path) const
{
    soc->rtc->SaveState(path);
}

void PocketWalker::SaveRtcState(std::ostream& stream, const std::filesystem::path& base_directory) const
{
    soc->rtc->SaveState(stream, base_directory);
}

bool PocketWalker::IsRtcCatchUpActive() const
{
    return soc->rtc->IsCatchUpActive();
}

size_t PocketWalker::RtcCatchUpMidnightsCompleted() const
{
    return soc->rtc->CatchUpMidnightsCompleted();
}

size_t PocketWalker::RtcCatchUpMidnightsTotal() const
{
    return soc->rtc->CatchUpMidnightsTotal();
}

void PocketWalker::ApplyPendingRtcSyncClock() const
{
    soc->rtc->ApplyPendingSyncClock();
}

void PocketWalker::ClearPendingRtcInterruptFlagsForCatchUp() const
{
    soc->rtc->ClearPendingInterruptFlagsForCatchUp();
}

void PocketWalker::PrepareRtcCatchUp()
{
    if (!soc->rtc->IsCatchUpActive())
    {
        soc->rtc->AllowCatchUpToRun(false);
        rtc_catch_up_waiting_for_steps = false;
        return;
    }

    soc->rtc->AllowCatchUpToRun(false);
    rtc_catch_up_waiting_for_steps = true;
    rtc_catch_up_last_total_steps = soc->memory->Read32(PW_ADDR_TOTAL_STEPS);
    rtc_catch_up_last_session_steps = soc->memory->Read32(PW_ADDR_SESSION_STEPS);
    rtc_catch_up_steps_stable_since = std::chrono::steady_clock::now();
}

bool PocketWalker::LoadEmulatorState(const std::string& path) const
{
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return false;

    return LoadEmulatorState(f);
}

bool PocketWalker::LoadEmulatorState(std::istream& stream) const
{
    std::array<char, STATE_MAGIC.size()> magic = {};
    stream.read(magic.data(), magic.size());
    const bool legacy_state = magic == LEGACY_STATE_MAGIC;
    if (!stream || (magic != STATE_MAGIC && !legacy_state))
        return false;

    if (!ReadCpuState(stream, soc->cpu))
        return false;

    stream.read(reinterpret_cast<char*>(m95512->eeprom.data()), m95512->eeprom.size());
    if (!stream)
        return false;

    if (!m95512->LoadEmulatorState(stream))
        return false;

    if (!bma150->LoadEmulatorState(stream))
        return false;

    if (!ReadMemoryRange(stream, soc->memory, RAM_START, RAM_SIZE))
        return false;

    if (!ReadMemoryRange(stream, soc->memory, IO_LOW_START, IO_LOW_SIZE))
        return false;

    if (!ReadMemoryRange(stream, soc->memory, IO_HIGH_START, IO_HIGH_SIZE))
        return false;

    if (!ReadValue(stream, soc->CKSTPR1.VALUE) || !ReadValue(stream, soc->CKSTPR2.VALUE))
        return false;

    if (!ReadValue(stream, soc->interrupts->IENR1.VALUE) || !ReadValue(stream, soc->interrupts->IENR2.VALUE) ||
        !ReadValue(stream, soc->interrupts->IRR1.VALUE) || !ReadValue(stream, soc->interrupts->IRR2.VALUE) ||
        !ReadValue(stream, soc->interrupts->RTCFLG.VALUE) || !ReadValue(stream, soc->interrupts->RTCCR2.VALUE) ||
        !ReadValue(stream, soc->interrupts->TIERW.VALUE) || !ReadValue(stream, soc->interrupts->TSRW.VALUE))
        return false;

    // RTC day/week flags in a save state are edge-triggered events, not durable
    // facts. The persisted RTC metadata below decides which missed midnights
    // should be replayed, so stale saved day/week flags must not run too.
    soc->interrupts->RTCFLG.DYIFG = false;
    soc->interrupts->RTCFLG.WKIFG = false;

    if (!ReadValue(stream, soc->ssu->PDRB.VALUE) || !ReadValue(stream, soc->ssu->PMRB.VALUE) ||
        !ReadValue(stream, soc->ssu->PFCR.VALUE) || !ReadValue(stream, soc->ssu->SSMR.VALUE) ||
        !ReadValue(stream, soc->ssu->SSER.VALUE) || !ReadValue(stream, soc->ssu->SSSR.VALUE) ||
        !ReadValue(stream, soc->ssu->SSRDR) || !ReadValue(stream, soc->ssu->SSTDR))
        return false;

    if (!ReadValue(stream, soc->sci3->SMR.VALUE) || !ReadValue(stream, soc->sci3->SSR.VALUE) ||
        !ReadValue(stream, soc->sci3->SCR.VALUE) || !ReadValue(stream, soc->sci3->IRCR.VALUE) ||
        !ReadValue(stream, soc->sci3->BRR) || !ReadValue(stream, soc->sci3->TDR) ||
        !ReadValue(stream, soc->sci3->RDR))
        return false;
    if (!soc->sci3->LoadEmulatorState(stream))
        return false;

    if (!ReadValue(stream, soc->timer_b1->TMB1.VALUE) || !ReadValue(stream, soc->timer_b1->TCB1) ||
        !ReadValue(stream, soc->timer_b1->TLB1))
        return false;
    if (!soc->timer_b1->LoadEmulatorState(stream))
        return false;

    if (!ReadValue(stream, soc->timer_w->TMRW.VALUE) || !ReadValue(stream, soc->timer_w->TCRW.VALUE))
        return false;
    if (!soc->timer_w->LoadEmulatorState(stream))
        return false;

    if (!ReadValue(stream, soc->rtc->RTCCR1.VALUE) || !ReadValue(stream, soc->rtc->RSECDR) ||
        !ReadValue(stream, soc->rtc->RMINDR) || !ReadValue(stream, soc->rtc->RHRDR) ||
        !ReadValue(stream, soc->rtc->RWKDR))
        return false;
    if (!soc->rtc->LoadEmulatorState(stream))
        return false;

    if (!ReadValue(stream, soc->adc->ADSR.VALUE) || !ReadValue(stream, soc->adc->AMR.VALUE))
        return false;

    if (!ReadDisplayState(stream, nt7508, legacy_state))
        return false;
    if (!nt7508->LoadEmulatorState(stream))
        return false;

    if (!soc->ssu->LoadEmulatorState(stream))
        return false;

    return true;
}

void PocketWalker::SaveEmulatorState(const std::string& path) const
{
    std::ofstream f(path, std::ios::binary);
    if (!f)
        return;

    SaveEmulatorState(f);
}

void PocketWalker::SaveEmulatorState(std::ostream& stream) const
{
    stream.write(STATE_MAGIC.data(), STATE_MAGIC.size());

    WriteCpuState(stream, soc->cpu);
    stream.write(reinterpret_cast<const char*>(m95512->eeprom.data()), m95512->eeprom.size());
    m95512->SaveEmulatorState(stream);
    bma150->SaveEmulatorState(stream);

    WriteMemoryRange(stream, soc->memory, RAM_START, RAM_SIZE);
    WriteMemoryRange(stream, soc->memory, IO_LOW_START, IO_LOW_SIZE);
    WriteMemoryRange(stream, soc->memory, IO_HIGH_START, IO_HIGH_SIZE);

    WriteValue(stream, soc->CKSTPR1.VALUE);
    WriteValue(stream, soc->CKSTPR2.VALUE);

    WriteValue(stream, soc->interrupts->IENR1.VALUE);
    WriteValue(stream, soc->interrupts->IENR2.VALUE);
    WriteValue(stream, soc->interrupts->IRR1.VALUE);
    WriteValue(stream, soc->interrupts->IRR2.VALUE);
    WriteValue(stream, soc->interrupts->RTCFLG.VALUE);
    WriteValue(stream, soc->interrupts->RTCCR2.VALUE);
    WriteValue(stream, soc->interrupts->TIERW.VALUE);
    WriteValue(stream, soc->interrupts->TSRW.VALUE);

    WriteValue(stream, soc->ssu->PDRB.VALUE);
    WriteValue(stream, soc->ssu->PMRB.VALUE);
    WriteValue(stream, soc->ssu->PFCR.VALUE);
    WriteValue(stream, soc->ssu->SSMR.VALUE);
    WriteValue(stream, soc->ssu->SSER.VALUE);
    WriteValue(stream, soc->ssu->SSSR.VALUE);
    WriteValue(stream, soc->ssu->SSRDR);
    WriteValue(stream, soc->ssu->SSTDR);

    WriteValue(stream, soc->sci3->SMR.VALUE);
    WriteValue(stream, soc->sci3->SSR.VALUE);
    WriteValue(stream, soc->sci3->SCR.VALUE);
    WriteValue(stream, soc->sci3->IRCR.VALUE);
    WriteValue(stream, soc->sci3->BRR);
    WriteValue(stream, soc->sci3->TDR);
    WriteValue(stream, soc->sci3->RDR);
    soc->sci3->SaveEmulatorState(stream);

    WriteValue(stream, soc->timer_b1->TMB1.VALUE);
    WriteValue(stream, soc->timer_b1->TCB1);
    WriteValue(stream, soc->timer_b1->TLB1);
    soc->timer_b1->SaveEmulatorState(stream);

    WriteValue(stream, soc->timer_w->TMRW.VALUE);
    WriteValue(stream, soc->timer_w->TCRW.VALUE);
    soc->timer_w->SaveEmulatorState(stream);

    WriteValue(stream, soc->rtc->RTCCR1.VALUE);
    WriteValue(stream, soc->rtc->RSECDR);
    WriteValue(stream, soc->rtc->RMINDR);
    WriteValue(stream, soc->rtc->RHRDR);
    WriteValue(stream, soc->rtc->RWKDR);
    soc->rtc->SaveEmulatorState(stream);

    WriteValue(stream, soc->adc->ADSR.VALUE);
    WriteValue(stream, soc->adc->AMR.VALUE);

    WriteDisplayState(stream, nt7508);
    nt7508->SaveEmulatorState(stream);
    soc->ssu->SaveEmulatorState(stream);
}

void PocketWalker::CyclePeripherals(uint8_t cycles) const
{
    this->buzzer->Cycle(cycles);
}

void PocketWalker::CycleEnhancements(uint8_t cycles) const
{
    if (bypass_power_save)
        this->activity_timer_bypass->Cycle(cycles);
}

uint64_t PocketWalker::RtcCatchUpStateHash() const
{
    uint64_t hash = 14695981039346656037ULL;

    for (uint16_t offset = 0; offset < RTC_CATCH_UP_RAM_HASH_SIZE; offset++)
        HashByte(hash, soc->memory->Read8(RTC_CATCH_UP_RAM_HASH_START + offset));

    for (const uint8_t value : m95512->eeprom)
        HashByte(hash, value);

    return hash;
}

void PocketWalker::UpdateRtcCatchUpFirmwareSettle()
{
    if (!soc->rtc->IsCatchUpWaitingForFirmwareSettle())
    {
        rtc_catch_up_settle_tracking = false;
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    if (rtc_catch_up_next_settle_check != std::chrono::steady_clock::time_point{} &&
        now < rtc_catch_up_next_settle_check)
    {
        return;
    }

    rtc_catch_up_next_settle_check = now + RTC_CATCH_UP_SETTLE_SAMPLE_INTERVAL;
    const uint64_t current_hash = RtcCatchUpStateHash();

    if (!rtc_catch_up_settle_tracking || current_hash != rtc_catch_up_settle_hash)
    {
        if (!rtc_catch_up_settle_tracking)
            rtc_catch_up_settle_started_at = now;

        rtc_catch_up_settle_tracking = true;
        rtc_catch_up_settle_hash = current_hash;
        rtc_catch_up_settle_since = now;
        return;
    }

    if (now - rtc_catch_up_settle_since >= RTC_CATCH_UP_SETTLE_STABLE_DELAY ||
        now - rtc_catch_up_settle_started_at >= RTC_CATCH_UP_SETTLE_MAX_WAIT)
    {
        rtc_catch_up_settle_tracking = false;
        soc->rtc->MarkCatchUpFirmwareSettled();
    }
}

void PocketWalker::UpdateDailyPeerCooldownNormalization()
{
    const bool rtc_catch_up_active = soc->rtc->IsCatchUpActive();
    if (rtc_catch_up_was_active && !rtc_catch_up_active)
    {
        NormalizeClearedPeerCooldownSlots();
    }
    rtc_catch_up_was_active = rtc_catch_up_active;

    uint16_t total_days = soc->memory->Read16(PW_ADDR_TOTAL_DAYS);
    if (total_days != last_observed_total_days)
    {
        soc->rtc->DebugMessage("total_days_changed old_days=" + std::to_string(last_observed_total_days) +
                               " new_days=" + std::to_string(total_days) +
                               " catch_up_active=" + (rtc_catch_up_active ? std::string("true") : std::string("false")));
        last_observed_total_days = total_days;
        soc->rtc->MarkCurrentDayProcessed();
        soc->rtc->ClearPendingInterruptFlagsForCatchUp();
        peer_cooldown_normalize_pending = true;
        peer_cooldown_normalize_at = std::chrono::steady_clock::now() + PEER_COOLDOWN_NORMALIZE_DELAY;
    }

    if (peer_cooldown_normalize_pending && std::chrono::steady_clock::now() >= peer_cooldown_normalize_at)
    {
        peer_cooldown_normalize_pending = false;
        NormalizeClearedPeerCooldownSlots();
    }
}

void PocketWalker::NormalizeClearedPeerCooldownSlots() const
{
    uint8_t normalized_slots = 0;
    for (uint8_t slot = 0; slot < EEPROM_MET_PEER_ENTRY_COUNT; slot++)
    {
        const uint16_t compare_start =
            EEPROM_MET_PEER_LIST_START +
            static_cast<uint16_t>(slot) * EEPROM_MET_PEER_ENTRY_SIZE +
            EEPROM_MET_PEER_COMPARE_OFFSET;

        bool erased = true;
        for (uint16_t i = 0; i < EEPROM_MET_PEER_COMPARE_SIZE; i++)
        {
            if (m95512->eeprom[compare_start + i] != 0xFF)
            {
                erased = false;
                break;
            }
        }

        if (!erased)
            continue;

        std::fill_n(m95512->eeprom.begin() + compare_start, EEPROM_MET_PEER_COMPARE_SIZE, 0);
        normalized_slots++;
    }

    if (normalized_slots > 0)
        Log::Info("Normalized {} erased peer cooldown slot fingerprints", normalized_slots);
}
