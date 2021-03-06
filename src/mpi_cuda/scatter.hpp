////////////////////////////////////////////////////////////////////////////////
// Copyright (c) 2018, Lawrence Livermore National Security, LLC.  Produced at the
// Lawrence Livermore National Laboratory in collaboration with University of
// Illinois Urbana-Champaign.
//
// Written by the LBANN Research Team (N. Dryden, N. Maruyama, et al.) listed in
// the CONTRIBUTORS file. <lbann-dev@llnl.gov>
//
// LLNL-CODE-756777.
// All rights reserved.
//
// This file is part of Aluminum GPU-aware Communication Library. For details, see
// http://software.llnl.gov/Aluminum or https://github.com/LLNL/Aluminum.
//
// Licensed under the Apache License, Version 2.0 (the "Licensee"); you
// may not use this file except in compliance with the License.  You may
// obtain a copy of the License at:
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
// implied. See the License for the specific language governing
// permissions and limitations under the license.
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include "cuda.hpp"
#include "mpi_cuda/communicator.hpp"
#include "progress.hpp"

namespace Al {
namespace internal {
namespace mpi_cuda {

template <typename T>
class ScatterAlState : public AlState {
public:
  ScatterAlState(const T* sendbuf, T* recvbuf, size_t count, int root,
                MPICUDACommunicator& comm, cudaStream_t stream) :
    AlState(nullptr),
    rank_(comm.rank()), root_(root), count_(count),
    host_mem_(get_pinned_memory<T>(rank_ == root_
                                   ? comm.size()*count_ : count_)),
    comm_(comm.get_comm()),
    compute_stream(comm.get_stream()) {

    bool const i_am_root = rank_ == root_;
    bool const inplace_operation = sendbuf == recvbuf;

    if (i_am_root) {
      // Transfer the data to scatter from device to host.
      AL_CHECK_CUDA(cudaMemcpyAsync(
                      host_mem_, sendbuf, sizeof(T)*count_*comm.size(),
                      cudaMemcpyDeviceToHost, stream));
      d2h_event_.record(stream);
      // Root only needs to copy its data to its final destination on the
      // device when it's not in place.
      if (!inplace_operation) {
        AL_CHECK_CUDA(cudaMemcpyAsync(
                        recvbuf, sendbuf + rank_*count_,
                        count_*sizeof(T), cudaMemcpyDeviceToDevice, stream));
      }
      gpuwait_.wait(stream);  // Block until comm is initiated.
    } else {
      d2h_event_.record(stream);  // Ensure comm not started early.
      // Block device until data can be transferred.
      gpuwait_.wait(stream);
      // Transfer completed buffer back to device.
      AL_CHECK_CUDA(cudaMemcpyAsync(recvbuf, host_mem_, sizeof(T)*count,
                                    cudaMemcpyHostToDevice, stream));
      h2d_event_.record(stream);
    }
  }

  ~ScatterAlState() override {
    release_pinned_memory(host_mem_);
  }

  PEAction step() override {
    if (!mem_xfer_done_) {
      if (d2h_event_.query()) {
        mem_xfer_done_ = true;
        return PEAction::advance;
      } else {
        return PEAction::cont;
      }
    }
    if (!scatter_started_) {
      if (root_ == rank_) {
        MPI_Iscatter(host_mem_, count_, mpi::TypeMap<T>(),
                     MPI_IN_PLACE, count_, mpi::TypeMap<T>(),
                     root_, comm_, &req_);
        // Root can unblock the device here.
        gpuwait_.signal();
      } else {
        MPI_Iscatter(host_mem_, count_, mpi::TypeMap<T>(),
                     host_mem_, count_, mpi::TypeMap<T>(),
                     root_, comm_, &req_);
      }
      scatter_started_ = true;
    }

    if (!scatter_done_) {
      // Wait for the scatter to complete
      int flag;
      MPI_Test(&req_, &flag, MPI_STATUS_IGNORE);
      if (flag) {
        scatter_done_ = true;
        if (rank_ == root_) {
          return PEAction::complete;
        } else {
          gpuwait_.signal();
        }
      }
      else {
        return PEAction::cont;
      }
    }

    // Wait for host-to-device memcopy; cleanup
    if (h2d_event_.query()) {
      return PEAction::complete;
    }
    return PEAction::cont;
  }

  bool needs_completion() const override { return false; }
  void* get_compute_stream() const override { return compute_stream; }
  std::string get_name() const override { return "HTScatter"; }

private:
  int rank_;
  int root_;
  size_t count_;
  T* host_mem_;

  cuda::GPUWait gpuwait_;

  cuda::FastEvent d2h_event_, h2d_event_;

  MPI_Comm comm_;
  MPI_Request req_ = MPI_REQUEST_NULL;

  bool mem_xfer_done_ = false;
  bool scatter_started_ = false;
  bool scatter_done_ = false;

  cudaStream_t compute_stream;
};

}  // namespace mpi_cuda
}  // namespace internal
}  // namespace Al
