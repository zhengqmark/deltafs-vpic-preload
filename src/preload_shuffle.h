/*
 * Copyright (c) 2017, Carnegie Mellon University.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT
 * HOLDERS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY
 * WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * preload_shuffle.h  abstract shuffle service api.
 */

#pragma once

#include <stddef.h>

typedef struct shuffle_ctx {
} shuffle_ctx_t;

/*
 * shuffle_write: shuffle a write request through an underlying transport.
 *
 * shuffle may be bypassed if destination is local.
 * write requests may be buffered and batched with other write
 * requests to amortize the cost of native rpcs.
 * batched write requests may be forwarded through multiple
 * hops to reduce per core memory for network buffers.
 *
 * return 0 on success, or EOF or errors.
 */
extern int shuffle_write(shuffle_ctx_t* ctx, const char* fn, char* d, size_t n,
                         int epoch);

/*
 * shuffle_epoch_start: perform necessary flushes at the
 * beginning of an epoch.
 *
 * abort on errors.
 */
extern void shuffle_epoch_start(shuffle_ctx_t* ctx);

/*
 * shuffle_epoch_end: perform necessary flushes at the
 * end of an epoch.
 *
 * abort on errors.
 */
extern void shuffle_epoch_end(shuffle_ctx_t* ctx);

/*
 * shuffle_finalize: shutdown the shuffle service and release resources.
 */
extern void shuffle_finalize(shuffle_ctx_t* ctx);

/*
 * shuffle_init: initialize the shuffle service or die.
 */
extern void shuffle_init(shuffle_ctx_t* ctx);

/*
 * shuffle_msg_sent: callback for a shuffle sender to
 * notify the main system of the sending of an rpc request.
 *
 * note: the main system may pass 1 or 2 opaque
 * arguments back to the shuffler for
 * temporary storage.
 */
extern void shuffle_msg_sent(size_t n, void** arg1, void** arg2);

/*
 * shuffle_msg_replied: callback for a shuffler sender to
 * notify the main system of the reception of an rpc response.
 *
 * note: a shuffler must return any arguments
 * previously obtained from the main system.
 */
extern void shuffle_msg_replied(void* arg1, void* arg2);

/*
 * shuffle_msg_received: callback for a shuffler receiver
 * to notify the main system of the reception of
 * an rpc request.
 */
extern void shuffle_msg_received();