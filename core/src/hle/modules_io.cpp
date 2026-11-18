// Module SceIofilemgr : fichiers, avec les périphériques Vita (ux0:, app0:...)
// associés à des dossiers de l'hôte.
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cctype>
#include <cstring>
#include <vector>

#include "hle_util.h"
#include "nids.h"
#include "vita/common/log.h"
#include "vita/hle/kernel.h"
#include "vita/memory/memory.h"

namespace vita {

using hle::arg;
using hle::read_cstr;

namespace {

constexpr u32 kStdoutFd = 0x3E000001;
constexpr u32 kStderrFd = 0x3E000002;
constexpr size_t kMaxFiles = 128;

// Drapeaux d'ouverture SCE_O_* (psp2common/kernel/iofilemgr.h).
constexpr u32 kRdOnly = 0x0001, kWrOnly = 0x0002, kAppend = 0x0100, kCreat = 0x0200, kTrunc = 0x0400, kExcl = 0x0800;

// Les codes errno courants sont identiques sur la Vita (newlib) et sur l'hôte.
u32 errno_to_sce(int e) {
    switch (e) {
        case ENOENT: case EBADF: case ENOMEM: case EACCES: case EEXIST: case ENOTDIR: case EISDIR:
        case EINVAL: case EMFILE: case ENOSPC: case ENOTEMPTY:
            return 0x80010000u | static_cast<u32>(e);
        default:
            return sce_error::kGeneric;
    }
}

}  // namespace

void Kernel::close_all_files() {
    for (auto& [uid, file] : files_) ::close(file->fd);
    files_.clear();
}

std::string Kernel::resolve_path(const std::string& vita_path, u32& error) const {
    const size_t colon = vita_path.find(':');
    if (colon == std::string::npos) {
        error = sce_error::kNoEntry;
        return {};
    }
    std::string device = vita_path.substr(0, colon + 1);
    for (char& c : device) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    auto it = mounts_.find(device);
    if (it == mounts_.end()) {
        error = sce_error::kNoDevice;
        return {};
    }
    // Chemin relatif au périphérique, sans « .. » pour ne pas sortir du dossier monté.
    std::string out = it->second;
    size_t pos = colon + 1;
    while (pos <= vita_path.size()) {
        size_t next = vita_path.find('/', pos);
        if (next == std::string::npos) next = vita_path.size();
        const std::string part = vita_path.substr(pos, next - pos);
        if (part == "..") {
            error = sce_error::kInvalid;
            return {};
        }
        if (!part.empty() && part != ".") out += "/" + part;
        pos = next + 1;
    }
    error = 0;
    return out;
}

void Kernel::register_io() {
    // SceUID sceIoOpen(const char *file, int flags, SceMode mode)
    register_function(nid::sceIoOpen, "sceIoOpen", [this](CpuState& s) {
        u32 error;
        const std::string path = resolve_path(read_cstr(memory_, s.r[0]), error);
        if (error) { s.r[0] = error; return; }
        if (files_.size() >= kMaxFiles) { s.r[0] = sce_error::kTooManyFiles; return; }
        const u32 flags = s.r[1];
        int host = (flags & (kRdOnly | kWrOnly)) == (kRdOnly | kWrOnly) ? O_RDWR : (flags & kWrOnly) ? O_WRONLY : O_RDONLY;
        if (flags & kAppend) host |= O_APPEND;
        if (flags & kCreat) host |= O_CREAT;
        if (flags & kTrunc) host |= O_TRUNC;
        if (flags & kExcl) host |= O_EXCL;
        const int fd = ::open(path.c_str(), host | O_CLOEXEC, 0644);
        if (fd < 0) { s.r[0] = errno_to_sce(errno); return; }
        const u32 uid = new_uid();
        files_[uid] = std::make_unique<File>(File{fd});
        s.r[0] = uid;
    });
    register_function(nid::sceIoClose, "sceIoClose", [this](CpuState& s) {
        auto it = files_.find(s.r[0]);
        if (it == files_.end()) { s.r[0] = sce_error::kBadFile; return; }
        ::close(it->second->fd);
        files_.erase(it);
        s.r[0] = 0;
    });
    // int sceIoRead(SceUID fd, void *buf, SceSize nbyte)
    register_function(nid::sceIoRead, "sceIoRead", [this](CpuState& s) {
        auto it = files_.find(s.r[0]);
        if (it == files_.end()) { s.r[0] = sce_error::kBadFile; return; }
        u8* buf = s.r[2] ? memory_.host_ptr(s.r[1], s.r[2]) : nullptr;
        if (s.r[2] && !buf) { s.r[0] = sce_error::kInvalid; return; }
        const ssize_t n = s.r[2] ? ::read(it->second->fd, buf, s.r[2]) : 0;
        s.r[0] = n < 0 ? errno_to_sce(errno) : static_cast<u32>(n);
    });
    // int sceIoWrite(SceUID fd, const void *buf, SceSize nbyte)
    register_function(nid::sceIoWrite, "sceIoWrite", [this](CpuState& s) {
        const u8* buf = s.r[2] ? memory_.host_ptr(s.r[1], s.r[2]) : nullptr;
        if (s.r[2] && !buf) { s.r[0] = sce_error::kInvalid; return; }
        if (s.r[0] == kStdoutFd || s.r[0] == kStderrFd) {
            write_console(reinterpret_cast<const char*>(buf), s.r[2]);
            s.r[0] = s.r[2];
            return;
        }
        auto it = files_.find(s.r[0]);
        if (it == files_.end()) { s.r[0] = sce_error::kBadFile; return; }
        const ssize_t n = s.r[2] ? ::write(it->second->fd, buf, s.r[2]) : 0;
        s.r[0] = n < 0 ? errno_to_sce(errno) : static_cast<u32>(n);
    });
    // SceOff sceIoLseek(SceUID fd, SceOff offset, int whence) : position 64 bits dans r2:r3, whence sur la pile.
    register_function(nid::sceIoLseek, "sceIoLseek", [this](CpuState& s) {
        auto it = files_.find(s.r[0]);
        if (it == files_.end()) { hle::ret64(s, static_cast<u64>(static_cast<s64>(static_cast<s32>(sce_error::kBadFile)))); return; }
        const s64 offset = static_cast<s64>((static_cast<u64>(s.r[3]) << 32) | s.r[2]);
        const u32 whence = arg(memory_, s, 4);
        const int host_whence = whence == 1 ? SEEK_CUR : whence == 2 ? SEEK_END : SEEK_SET;
        const off_t r = ::lseek(it->second->fd, static_cast<off_t>(offset), host_whence);
        hle::ret64(s, r < 0 ? static_cast<u64>(static_cast<s64>(static_cast<s32>(errno_to_sce(errno)))) : static_cast<u64>(r));
    });
    register_function(nid::sceIoRemove, "sceIoRemove", [this](CpuState& s) {
        u32 error;
        const std::string path = resolve_path(read_cstr(memory_, s.r[0]), error);
        if (error) { s.r[0] = error; return; }
        s.r[0] = ::unlink(path.c_str()) == 0 ? 0 : errno_to_sce(errno);
    });
    register_function(nid::sceIoMkdir, "sceIoMkdir", [this](CpuState& s) {
        u32 error;
        const std::string path = resolve_path(read_cstr(memory_, s.r[0]), error);
        if (error) { s.r[0] = error; return; }
        s.r[0] = ::mkdir(path.c_str(), 0755) == 0 ? 0 : errno_to_sce(errno);
    });
}

}  // namespace vita
