################################################################################
#
# Copyright (c) 2025 ByteDance Ltd. and/or its affiliates
#
# Permission is hereby granted, free of charge, to any person obtaining
# a copy of this software and associated documentation files
# (the "Software"), to deal in the Software without restriction,
# including without limitation the rights to use, copy, modify, merge,
# publish, distribute, sublicense, and/or sell copies of the Software,
# and to permit persons to whom the Software is furnished to do so,
# subject to the following conditions:
#
# The above copyright notice and this permission notice shall be
# included in all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
# EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
# MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
# IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
# CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
# TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
# SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
#
################################################################################
import triton
import torch
import triton.language as tl
import triton_dist.language as dl
from triton_dist.utils import CUDA_CHECK
from cuda import cuda
from triton.language.extra.cuda.language_extra import (
    tid,
    st,
    ld,
    __syncthreads,
    atomic_add,
    ld_acquire,
    atomic_cas,
)

from triton_dist import pynvshmem
from triton_dist.utils import check_p2p_native_atomic_supported


@triton.jit
def _is_cta_master():
    thread_idx_x = tid(0)
    thread_idx_y = tid(1)
    thread_idx_z = tid(2)
    return (thread_idx_x + thread_idx_y + thread_idx_z) == 0


@triton.jit
def _is_gpu_master():
    pid_x = tl.program_id(axis=0)
    pid_y = tl.program_id(axis=1)
    pid_z = tl.program_id(axis=2)
    return (pid_x + pid_y + pid_z) == 0


@triton.jit
def barrier_on_this_grid(ptr):
    """ triton implementation of cooperative_group::thid_grid().sync() """
    __syncthreads()
    pid_size_x = tl.num_programs(axis=0)
    pid_size_y = tl.num_programs(axis=1)
    pid_size_z = tl.num_programs(axis=2)
    expected = pid_size_x * pid_size_y * pid_size_z
    if _is_cta_master():
        nb = tl.where(
            _is_gpu_master(),
            tl.cast(0x80000000, tl.uint32, bitcast=True) - (expected - 1),
            1,
        )
        old_arrive = atomic_add(ptr.to(tl.pointer_type(tl.uint32)), nb, scope="gpu", semantic="release")
    else:
        old_arrive = tl.cast(0, tl.uint32)

    if _is_cta_master():
        current_arrive = ld_acquire(ptr)
        while ((old_arrive ^ current_arrive) & 0x80000000) == 0:
            current_arrive = ld_acquire(ptr, scope=tl.constexpr("gpu"))

    __syncthreads()


@triton.jit(do_not_specialize=["local_rank", "rank", "local_world_size"])
def barrier_all_intra_node_atomic_cas_block(local_rank, rank, local_world_size, symm_flag_ptr):
    """ NOTE: this function should only be called with atomic support. memory over PCI-e does not support atomic r/w. DON'T use this function on such platforms.
    """
    with dl.simt_exec_region() as (thread_idx, block_size):
        local_rank_offset = rank - local_rank
        if thread_idx < local_world_size:  # thread_idx => local_rank
            remote_ptr = dl.symm_at(symm_flag_ptr + local_rank, thread_idx + local_rank_offset)
            while atomic_cas(remote_ptr, 0, 1, "sys", "release") != 0:
                pass

        if thread_idx < local_world_size:  # thread_idx => local_rank
            while (atomic_cas(symm_flag_ptr + thread_idx, 1, 0, "sys", "acquire") != 1):
                pass
        __syncthreads()


@triton.jit
def _barrier_all_intra_node_non_atomic_once_block(local_rank, rank, local_world_size, symm_flags, target_value):
    with dl.simt_exec_region() as (thread_idx, block_size):
        if thread_idx < local_world_size:  # thread_idx => local_rank
            local_rank_offset = rank - local_rank
            remote_ptr = dl.symm_at(symm_flags + local_rank, thread_idx + local_rank_offset)
            st(remote_ptr, target_value, scope="sys", semantic="release")
            while ld(symm_flags + thread_idx, scope="sys", semantic="acquire") != target_value:
                pass
        __syncthreads()


@triton.jit(do_not_specialize=["local_rank", "rank", "num_ranks", "target_value"])
def barrier_all_intra_node_non_atomic_block(local_rank, rank, num_ranks, symm_flags, target_value):
    """ symm_flags is expected to:
        1. of int32 dtype
        2. has at least num_ranks * 2 elements
        3. of symmetric pointer

        symm_flags [0, num_ranks * 2) is used to sync all ranks.
    """
    tl.static_assert(symm_flags.dtype.element_ty == tl.int32)
    _barrier_all_intra_node_non_atomic_once_block(local_rank, rank, num_ranks, symm_flags, target_value)

    # barrier all CTAs
    barrier_on_this_grid(symm_flags + 2 * num_ranks)

    # next iter
    _barrier_all_intra_node_non_atomic_once_block(local_rank, rank, num_ranks, symm_flags + num_ranks, target_value)

    barrier_on_this_grid(symm_flags + 2 * num_ranks)


@triton.jit(do_not_specialize=["local_rank", "rank", "num_ranks", "target_value"])
def barrier_all_intra_node_non_atomic(local_rank, rank, num_ranks, symm_flags, target_value):
    """ symm_flags is expected to:
        1. of int32 dtype
        2. has at least num_ranks * 2 + 1 elements
        3. of symmetric pointer

        symm_flags [0, num_ranks * 2) is used to sync all ranks.
        symm_flags[num_ranks * 2] is used to sync all CTAs
    """
    tl.static_assert(symm_flags.dtype.element_ty == tl.int32)
    pid = tl.program_id(axis=0)
    if pid == 0:
        _barrier_all_intra_node_non_atomic_once_block(local_rank, rank, num_ranks, symm_flags, target_value)

    # barrier all CTAs
    barrier_on_this_grid(symm_flags + 2 * num_ranks)

    # next iter
    if pid == 0:
        _barrier_all_intra_node_non_atomic_once_block(local_rank, rank, num_ranks, symm_flags + num_ranks, target_value)

    barrier_on_this_grid(symm_flags + 2 * num_ranks)


class BarrierAllContext:
    """
    You may use this to barrier all ranks in global, or just in intra-node team.

    NOTE: nvshmem_barrier_all is slower for intra-node only.
    """

    def __init__(self, is_intra_node):
        self.is_intra_node = is_intra_node
        if self.is_intra_node:
            self.rank = pynvshmem.nvshmem_my_pe()
            self.local_rank = pynvshmem.nvshmem_team_my_pe(pynvshmem.NVSHMEMX_TEAM_NODE)
            self.num_local_ranks = pynvshmem.nvshmem_team_n_pes(pynvshmem.NVSHMEMX_TEAM_NODE)
            self.symm_barrier = pynvshmem.nvshmem_create_tensor((1, ), torch.int32)
            self.symm_barrier.fill_(0)
            pynvshmem.nvshmem_barrier_all()


def barrier_all_on_stream(ctx: BarrierAllContext, stream: torch.cuda.Stream):
    """
    barrier_all_on_stream does not support CUDAGraph
    """
    if ctx is None or not ctx.is_intra_node:
        return pynvshmem.nvshmemx_barrier_all_on_stream(stream.cuda_stream)

    if check_p2p_native_atomic_supported():
        barrier_all_intra_node_atomic_cas_block[(1, )](ctx.local_rank, ctx.rank, ctx.num_local_ranks, ctx.symm_barrier)
    else:
        barrier_all_intra_node_non_atomic_block[(1, )](ctx.local_rank, ctx.rank, ctx.num_local_ranks, ctx.symm_barrier,
                                                       ctx.target_value)
        ctx.target_value += 1


def wait_eq(ptr: int, signal: int, stream: torch.cuda.Stream, require_i64=False):
    if not require_i64:
        (err, ) = cuda.cuStreamWaitValue32(
            stream.cuda_stream,
            ptr,
            signal,
            cuda.CUstreamWaitValue_flags.CU_STREAM_WAIT_VALUE_EQ,
        )
    else:
        (err, ) = cuda.cuStreamWaitValue64(
            stream.cuda_stream,
            ptr,
            signal,
            cuda.CUstreamWaitValue_flags.CU_STREAM_WAIT_VALUE_EQ,
        )
    CUDA_CHECK(err)


def set_signal(ptr: int, signal: int, stream: torch.cuda.Stream, require_i64=False):
    if not require_i64:
        (err, ) = cuda.cuStreamWriteValue32(
            stream.cuda_stream,
            ptr,
            signal,
            cuda.CUstreamWriteValue_flags.CU_STREAM_WRITE_VALUE_DEFAULT,
        )
    else:
        (err, ) = cuda.cuStreamWriteValue64(
            stream.cuda_stream,
            ptr,
            signal,
            cuda.CUstreamWriteValue_flags.CU_STREAM_WRITE_VALUE_DEFAULT,
        )
    CUDA_CHECK(err)
