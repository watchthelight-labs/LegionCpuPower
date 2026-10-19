// <wtl> project: legioncpupower | file: src/kmod_info.c | author: watchthelight </wtl>
// kmod_info for a Lilu plugin: like ../common/kmod_info.c, but libkmod's _start/_stop call the plugin's
// LegionCpuPower_kern_start / _kern_stop (Lilu Library/plugin_start.cpp), as Xcode's MODULE_START does.
#include <mach/mach_types.h>

extern kern_return_t _start(kmod_info_t *ki, void *data);
extern kern_return_t _stop(kmod_info_t *ki, void *data);
extern kern_return_t LegionCpuPower_kern_start(kmod_info_t *ki, void *data);
extern kern_return_t LegionCpuPower_kern_stop(kmod_info_t *ki, void *data);

#define LEGION_KMOD_DECL(id, ver, start, stop) KMOD_EXPLICIT_DECL(id, ver, start, stop)
__attribute__((visibility("default"))) LEGION_KMOD_DECL(KEXT_ID, KEXT_VERSION, _start, _stop)
__private_extern__ kmod_start_func_t *_realmain = LegionCpuPower_kern_start;
__private_extern__ kmod_stop_func_t *_antimain = LegionCpuPower_kern_stop;
__private_extern__ int _kext_apple_cc = __APPLE_CC__;
