// Modules SceDisplay (tampon d'image, synchronisation verticale) et SceCtrl (commandes).
#include "hle_util.h"
#include "nids.h"
#include "vita/hle/kernel.h"
#include "vita/memory/memory.h"

namespace vita {

void Kernel::register_display_ctrl() {
    // int sceDisplaySetFrameBuf(const SceDisplayFrameBuf *pParam, int sync) ; structure de 0x18 octets.
    register_function(nid::sceDisplaySetFrameBuf, "sceDisplaySetFrameBuf", [this](CpuState& s) {
        FrameBuffer fb;
        if (s.r[0]) {
            if (memory_.read<u32>(s.r[0]) != 0x18) { memory_.take_fault(); s.r[0] = sce_error::kInvalid; return; }
            fb.base = memory_.read<u32>(s.r[0] + 4);
            fb.pitch = memory_.read<u32>(s.r[0] + 8);
            fb.pixel_format = memory_.read<u32>(s.r[0] + 12);
            fb.width = memory_.read<u32>(s.r[0] + 16);
            fb.height = memory_.read<u32>(s.r[0] + 20);
            if (memory_.take_fault() || (fb.base && (fb.width == 0 || fb.height == 0 || fb.pitch < fb.width ||
                                                     fb.width > 1920 || fb.height > 1088))) {
                s.r[0] = sce_error::kInvalid;
                return;
            }
        }
        std::lock_guard<std::mutex> lock(shared_mutex_);
        framebuffer_ = fb;
        s.r[0] = 0;
    });
    register_function(nid::sceDisplayGetFrameBuf, "sceDisplayGetFrameBuf", [this](CpuState& s) {
        const FrameBuffer fb = framebuffer();
        if (memory_.read<u32>(s.r[0]) != 0x18) { memory_.take_fault(); s.r[0] = sce_error::kInvalid; return; }
        memory_.write<u32>(s.r[0] + 4, fb.base);
        memory_.write<u32>(s.r[0] + 8, fb.pitch);
        memory_.write<u32>(s.r[0] + 12, fb.pixel_format);
        memory_.write<u32>(s.r[0] + 16, fb.width);
        memory_.write<u32>(s.r[0] + 20, fb.height);
        s.r[0] = memory_.take_fault() ? sce_error::kInvalid : 0;
    });
    // Attend le début de la prochaine image (60 Hz).
    register_function(nid::sceDisplayWaitVblankStart, "sceDisplayWaitVblankStart", [this](CpuState& s) {
        const u64 next = (now_us() / kVblankPeriodUs + 1) * kVblankPeriodUs;
        s.r[0] = 0;
        block(Thread::Wait::Vblank, 0, 0, next);
    });

    register_function(nid::sceCtrlSetSamplingMode, "sceCtrlSetSamplingMode", [this](CpuState& s) {
        const u32 previous = ctrl_sampling_mode_;
        ctrl_sampling_mode_ = s.r[0];
        s.r[0] = previous;
    });
    // int sceCtrl{Peek,Read}BufferPositive(int port, SceCtrlData *pad_data, int count) ; SceCtrlData : 0x20 octets.
    // La lecture n'attend pas l'échantillon suivant : elle renvoie l'état courant.
    auto read_pad = [this](CpuState& s) {
        const s32 count = static_cast<s32>(s.r[2]);
        if (count <= 0 || count > 64) { s.r[0] = sce_error::kInvalid; return; }
        InputState in;
        {
            std::lock_guard<std::mutex> lock(shared_mutex_);
            in = input_;
        }
        const u64 stamp = now_us();
        for (s32 i = 0; i < count; ++i) {
            const GuestAddr p = s.r[1] + 0x20 * static_cast<u32>(i);
            memory_.write<u64>(p, stamp);
            memory_.write<u32>(p + 8, in.buttons);
            memory_.write<u8>(p + 12, in.lx);
            memory_.write<u8>(p + 13, in.ly);
            memory_.write<u8>(p + 14, in.rx);
            memory_.write<u8>(p + 15, in.ry);
            for (u32 b = 16; b < 0x20; ++b) memory_.write<u8>(p + b, 0);
        }
        s.r[0] = memory_.take_fault() ? sce_error::kInvalid : static_cast<u32>(count);
    };
    register_function(nid::sceCtrlPeekBufferPositive, "sceCtrlPeekBufferPositive", read_pad);
    register_function(nid::sceCtrlReadBufferPositive, "sceCtrlReadBufferPositive", read_pad);
}

}  // namespace vita
