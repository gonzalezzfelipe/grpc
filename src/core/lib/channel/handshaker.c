/*
 *
 * Copyright 2016, Google Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following disclaimer
 * in the documentation and/or other materials provided with the
 * distribution.
 *     * Neither the name of Google Inc. nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include <string.h>

#include <grpc/support/alloc.h>
#include <grpc/support/log.h>

#include "src/core/lib/channel/channel_args.h"
#include "src/core/lib/channel/handshaker.h"
#include "src/core/lib/iomgr/timer.h"

//
// grpc_handshaker
//

void grpc_handshaker_init(const grpc_handshaker_vtable* vtable,
                          grpc_handshaker* handshaker) {
  handshaker->vtable = vtable;
}

static void grpc_handshaker_destroy(grpc_exec_ctx* exec_ctx,
                                    grpc_handshaker* handshaker) {
  handshaker->vtable->destroy(exec_ctx, handshaker);
}

static void grpc_handshaker_shutdown(grpc_exec_ctx* exec_ctx,
                                     grpc_handshaker* handshaker) {
  handshaker->vtable->shutdown(exec_ctx, handshaker);
}

static void grpc_handshaker_do_handshake(grpc_exec_ctx* exec_ctx,
                                         grpc_handshaker* handshaker,
                                         grpc_tcp_server_acceptor* acceptor,
                                         grpc_closure* on_handshake_done,
                                         grpc_handshaker_args* args) {
  handshaker->vtable->do_handshake(exec_ctx, handshaker, acceptor,
                                   on_handshake_done, args);
}

//
// grpc_handshake_manager
//

struct grpc_handshake_manager {
  gpr_mu mu;
  gpr_refcount refs;
  // An array of handshakers added via grpc_handshake_manager_add().
  size_t count;
  grpc_handshaker** handshakers;
  // The index of the handshaker to invoke next and closure to invoke it.
  size_t index;
  grpc_closure call_next_handshaker;
  // The acceptor to call the handshakers with.
  grpc_tcp_server_acceptor* acceptor;
  // Deadline timer across all handshakers.
  grpc_timer deadline_timer;
  // The final callback and user_data to invoke after the last handshaker.
  grpc_closure on_handshake_done;
  void* user_data;
};

grpc_handshake_manager* grpc_handshake_manager_create() {
  grpc_handshake_manager* mgr = gpr_malloc(sizeof(grpc_handshake_manager));
  memset(mgr, 0, sizeof(*mgr));
  gpr_mu_init(&mgr->mu);
  gpr_ref_init(&mgr->refs, 1);
  return mgr;
}

static bool is_power_of_2(size_t n) { return (n & (n - 1)) == 0; }

void grpc_handshake_manager_add(grpc_handshake_manager* mgr,
                                grpc_handshaker* handshaker) {
  gpr_mu_lock(&mgr->mu);
  // To avoid allocating memory for each handshaker we add, we double
  // the number of elements every time we need more.
  size_t realloc_count = 0;
  if (mgr->count == 0) {
    realloc_count = 2;
  } else if (mgr->count >= 2 && is_power_of_2(mgr->count)) {
    realloc_count = mgr->count * 2;
  }
  if (realloc_count > 0) {
    mgr->handshakers =
        gpr_realloc(mgr->handshakers, realloc_count * sizeof(grpc_handshaker*));
  }
  mgr->handshakers[mgr->count++] = handshaker;
  gpr_mu_unlock(&mgr->mu);
}

static void grpc_handshake_manager_unref(grpc_exec_ctx* exec_ctx,
                                         grpc_handshake_manager* mgr) {
  if (gpr_unref(&mgr->refs)) {
    for (size_t i = 0; i < mgr->count; ++i) {
      grpc_handshaker_destroy(exec_ctx, mgr->handshakers[i]);
    }
    gpr_free(mgr->handshakers);
    gpr_mu_destroy(&mgr->mu);
    gpr_free(mgr);
  }
}

void grpc_handshake_manager_destroy(grpc_exec_ctx* exec_ctx,
                                    grpc_handshake_manager* mgr) {
  grpc_handshake_manager_unref(exec_ctx, mgr);
}

void grpc_handshake_manager_shutdown(grpc_exec_ctx* exec_ctx,
                                     grpc_handshake_manager* mgr) {
  gpr_mu_lock(&mgr->mu);
  for (size_t i = 0; i < mgr->count; ++i) {
    grpc_handshaker_shutdown(exec_ctx, mgr->handshakers[i]);
  }
  gpr_mu_unlock(&mgr->mu);
}

static void call_next_handshaker(grpc_exec_ctx* exec_ctx, void* arg,
                                 grpc_error* error);

// Helper function to call either the next handshaker or the
// on_handshake_done callback.
static void call_next_handshaker_locked(grpc_exec_ctx* exec_ctx,
                                        grpc_handshake_manager* mgr,
                                        grpc_handshaker_args* args,
                                        grpc_error* error) {
  GPR_ASSERT(mgr->index <= mgr->count);
  // If we got an error, skip all remaining handshakers and invoke the
  // caller-supplied callback immediately.
  // Otherwise, if this is the last handshaker, then call the on_handshake_done
  // callback instead of chaining back to this function again.
  if (error != GRPC_ERROR_NONE || mgr->index == mgr->count) {
    // Cancel deadline timer, since we're invoking the on_handshake_done
    // callback now.
    grpc_timer_cancel(exec_ctx, &mgr->deadline_timer);
    args->user_data = mgr->user_data;
    grpc_exec_ctx_sched(exec_ctx, &mgr->on_handshake_done,
                        GRPC_ERROR_REF(error), NULL);
    // Since we're invoking the final callback, we won't be coming back
    // to this function, so we can release our reference to the
    // handshake manager.
    grpc_handshake_manager_unref(exec_ctx, mgr);
    return;
  }
  // Call the next handshaker.
  grpc_handshaker_do_handshake(exec_ctx, mgr->handshakers[mgr->index],
                               mgr->acceptor, &mgr->call_next_handshaker, args);
  ++mgr->index;
}

// A function used as the handshaker-done callback when chaining
// handshakers together.
static void call_next_handshaker(grpc_exec_ctx* exec_ctx, void* arg,
                                 grpc_error* error) {
  grpc_handshaker_args* args = arg;
  grpc_handshake_manager* mgr = args->user_data;
  gpr_mu_lock(&mgr->mu);
  call_next_handshaker_locked(exec_ctx, mgr, args, error);
  gpr_mu_unlock(&mgr->mu);
}

// Callback invoked when deadline is exceeded.
static void on_timeout(grpc_exec_ctx* exec_ctx, void* arg, grpc_error* error) {
  grpc_handshake_manager* mgr = arg;
  if (error == GRPC_ERROR_NONE) {  // Timer fired, rather than being cancelled.
    grpc_handshake_manager_shutdown(exec_ctx, mgr);
  }
  grpc_handshake_manager_unref(exec_ctx, mgr);
}

void grpc_handshake_manager_do_handshake(
    grpc_exec_ctx* exec_ctx, grpc_handshake_manager* mgr,
    grpc_endpoint* endpoint, const grpc_channel_args* channel_args,
    gpr_timespec deadline, grpc_tcp_server_acceptor* acceptor,
    grpc_iomgr_cb_func on_handshake_done, void* user_data) {
  // Construct handshaker args.  These will be passed through all
  // handshakers and eventually be freed by the on_handshake_done callback.
  grpc_handshaker_args* args = gpr_malloc(sizeof(*args));
  args->endpoint = endpoint;
  args->args = grpc_channel_args_copy(channel_args);
  args->read_buffer = gpr_malloc(sizeof(*args->read_buffer));
  grpc_slice_buffer_init(args->read_buffer);
  // Initialize state needed for calling handshakers.
  gpr_mu_lock(&mgr->mu);
  GPR_ASSERT(mgr->index == 0);
  mgr->acceptor = acceptor;
  grpc_closure_init(&mgr->call_next_handshaker, call_next_handshaker, args);
  grpc_closure_init(&mgr->on_handshake_done, on_handshake_done, args);
  // While chaining between handshakers, we use args->user_data to
  // store a pointer to the handshake manager.  This will be
  // changed to point to the caller-supplied user_data before calling
  // the on_handshake_done callback.
  args->user_data = mgr;
  mgr->user_data = user_data;
  // Start deadline timer, which owns a ref.
  gpr_ref(&mgr->refs);
  grpc_timer_init(exec_ctx, &mgr->deadline_timer,
                  gpr_convert_clock_type(deadline, GPR_CLOCK_MONOTONIC),
                  on_timeout, mgr, gpr_now(GPR_CLOCK_MONOTONIC));
  // Start first handshaker, which also owns a ref.
  gpr_ref(&mgr->refs);
  call_next_handshaker_locked(exec_ctx, mgr, args, GRPC_ERROR_NONE);
  gpr_mu_unlock(&mgr->mu);
}
