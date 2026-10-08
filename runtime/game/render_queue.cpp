// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// NFSMW's deferred render-callback queue (state at 0x82909650) is a
// lock-free single-producer, single-consumer list: while loading and during
// in-engine cutscenes one thread records render callbacks (sub_823C8210, and
// the same code inlined in sub_82444520 and sub_82462DC8) and the main
// thread plays them back (sub_823C8290). It has no barriers: it relies on the
// entry's stores becoming visible before the write pointer and count that
// publish it. x86 keeps stores in order; ARM64 doesn't, and the main thread
// saw a new count with the entry still zero (a call to a null callback at
// lr 823C82F8, about one run in three on the Mac once game time ran at the
// right speed). Mid-asm hooks (config/nfsmw.toml) add a release fence before
// each publish and an acquire fence before an entry is read.
#include <stdafx.h>
#include <atomic>

void RenderQueuePublish()
{
    std::atomic_thread_fence(std::memory_order_release);
}

void RenderQueueAcquire()
{
    std::atomic_thread_fence(std::memory_order_acquire);
}
