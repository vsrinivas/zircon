// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fbl/function.h>
#include <string.h>
#include <threads.h>
#include <zircon/process.h>
#include <zircon/stack.h>
#include <zircon/syscalls.h>
#include <zircon/syscalls/debug.h>
#include <zircon/syscalls/port.h>
#include <zircon/threads.h>

#ifndef BUILD_COMBINED_TESTS
#include <inspector/inspector.h>
#endif

#include "test_thread.h"

namespace pager_tests {

static int test_thread_fn(void* arg) {
    reinterpret_cast<TestThread*>(arg)->Run();
    return 0;
}

TestThread::TestThread(fbl::Function<bool()> fn) : fn_(std::move(fn)) {
#if !defined(__x86_64__) and !defined(__aarch64__)
    static_assert(false, "Unsupported architecture");
#endif
}

TestThread::~TestThread() {
    // TODO: UserPagers need to be destroyed before TestThreads to ensure threads aren't blocked
    if (killed_) {
        // Killing the thread leaves the thread support library in a somewhat
        // undefined state, but it should be okay as long as we don't touch
        // the thread again (and don't do it with too many threads).
        ZX_ASSERT(zx_thread_.wait_one(ZX_TASK_TERMINATED, zx::time::infinite(), nullptr) == ZX_OK);
    } else {
        ZX_ASSERT(thrd_join(thrd_, nullptr) == thrd_success);
    }

    zx_handle_close(port_);
}

bool TestThread::Start() {
    constexpr const char* kName = "test_thread";

    if (thrd_create_with_name(&thrd_, test_thread_fn, this, kName) != thrd_success) {
        return false;
    }
    if (zx_handle_duplicate(thrd_get_zx_handle(thrd_),
                            ZX_RIGHT_SAME_RIGHTS,
                            zx_thread_.reset_and_get_address()) != ZX_OK) {
        return false;
    }

    if (zx_port_create(0, &port_) != ZX_OK) {
        return false;
    }
    if (zx_task_bind_exception_port(zx_thread_.get(), port_, 0, 0) != ZX_OK) {
        return false;
    }
    if (zx_object_wait_async(zx_thread_.get(), port_, 0,
                             ZX_THREAD_TERMINATED, ZX_WAIT_ASYNC_ONCE) != ZX_OK) {
        return false;
    }

    sync_completion_signal(&startup_sync_);

    return true;
}

bool TestThread::Wait() {
    return Wait(false /* expect_failure */, false /* expect_crash */, 0);
}

bool TestThread::WaitForFailure() {
    return Wait(true /* expect_failure */, false /* expect_crash */, 0);
}

bool TestThread::WaitForCrash(uintptr_t crash_addr) {
    return Wait(false /* expect_failure */, true /* expect_crash */, crash_addr);
}

bool TestThread::Wait(bool expect_failure, bool expect_crash, uintptr_t crash_addr) {
    zx_port_packet_t packet;
    ZX_ASSERT(zx_port_wait(port_, ZX_TIME_INFINITE, &packet) == ZX_OK);

    if (ZX_PKT_IS_SIGNAL_ONE(packet.type)) {
        // We got a ZX_THREAD_TERMINATED signal.
        return !expect_crash && success_ != expect_failure;
    } else if (ZX_PKT_IS_EXCEPTION(packet.type)) {
        zx_exception_report_t report;
        ZX_ASSERT(zx_object_get_info(zx_thread_.get(), ZX_INFO_THREAD_EXCEPTION_REPORT,
                                     &report, sizeof(report), NULL, NULL) == ZX_OK);
        bool res = expect_crash && report.header.type == ZX_EXCP_FATAL_PAGE_FAULT;
        if (res) {
#if defined(__x86_64__)
            uintptr_t actual_crash_addr = report.context.arch.u.x86_64.cr2;
#else
            uintptr_t actual_crash_addr = report.context.arch.u.arm_64.far;
#endif
            res &= crash_addr == actual_crash_addr;
        }

        if (!res) {
            // Print debug info if the crash wasn't expected.
            PrintDebugInfo(report);
        }

        // thrd_exit takes a parameter, but we don't actually read it when we join
        zx_thread_state_general_regs_t regs;
        ZX_ASSERT(zx_thread_.read_state(ZX_THREAD_STATE_GENERAL_REGS,
                                        &regs, sizeof(regs)) == ZX_OK);
#if defined(__x86_64__)
        regs.rip = reinterpret_cast<uintptr_t>(thrd_exit);
#else
        regs.pc = reinterpret_cast<uintptr_t>(thrd_exit);
#endif
        ZX_ASSERT(zx_thread_.write_state(ZX_THREAD_STATE_GENERAL_REGS,
                                         &regs, sizeof(regs)) == ZX_OK);

        ZX_ASSERT(zx_task_resume_from_exception(zx_thread_.get(), port_, 0) == ZX_OK);
        return res;
    } else {
        ZX_ASSERT_MSG(false, "Unexpceted port message");
    }
}

void TestThread::PrintDebugInfo(const zx_exception_report_t& report) {
    // The crash library isn't available when running as part of core-tests
#ifndef BUILD_COMBINED_TESTS
    printf("\nCrash info:\n");

    zx_thread_state_general_regs_t regs;
    zx_vaddr_t pc = 0, sp = 0, fp = 0;

    ZX_ASSERT(inspector_read_general_regs(zx_thread_.get(), &regs) == ZX_OK);
    // Delay setting this until here so Fail will know we now have the regs.
#if defined(__x86_64__)
    inspector_print_general_regs(stdout, &regs, &report.context.arch.u.x86_64);
    pc = regs.rip;
    sp = regs.rsp;
    fp = regs.rbp;
#else
    inspector_print_general_regs(stdout, &regs, &report.context.arch.u.arm_64);
    pc = regs.pc;
    sp = regs.sp;
    fp = regs.r[29];
#endif
    inspector_dsoinfo_t* dso_list = inspector_dso_fetch_list(zx_process_self());
    inspector_dso_print_list(stdout, dso_list);
    inspector_print_backtrace(stdout, zx_process_self(), zx_thread_.get(), dso_list,
                              pc, sp, fp, true);
#endif
}

bool TestThread::WaitForBlocked() {
    while (1) {
        zx_info_thread_t info;
        uint64_t actual, actual_count;
        if (zx_thread_.get_info(ZX_INFO_THREAD, &info,
                                sizeof(info), &actual, &actual_count) != ZX_OK) {
            return false;
        } else if (info.state == ZX_THREAD_STATE_BLOCKED_PAGER) {
            return true;
        }
        // There's no signal to wait on, so just poll
        zx_nanosleep(zx_deadline_after(ZX_USEC(100)));
    }
}

void TestThread::Run() {
    if (sync_completion_wait(&startup_sync_, ZX_TIME_INFINITE) == ZX_OK) {
        success_ = fn_();
    }
}

} // namespace pager_tests
