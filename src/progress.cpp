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

#include <hwloc.h>
#include <cstdlib>
#include "Al.hpp"
#include "progress.hpp"
#include "trace.hpp"

// For ancient versions of hwloc.
#if HWLOC_API_VERSION < 0x00010b00
#define HWLOC_OBJ_NUMANODE HWLOC_OBJ_NODE
// Ported from more recent hwloc versions.
hwloc_obj_t hwloc_get_numanode_obj_by_os_index(hwloc_topology_t topology, unsigned os_index) {
  hwloc_obj_t obj = NULL;
  while ((obj = hwloc_get_next_obj_by_type(topology, HWLOC_OBJ_NUMANODE, obj)) != NULL) {
    if (obj->os_index == os_index) {
      return obj;
    }
  }
  return NULL;
}
#endif

namespace Al {
namespace internal {

AlState::~AlState() {
  profiling::prof_end(prof_range);
}

void AlState::start() {
  prof_range = profiling::prof_start(get_name());
}

AlRequest get_free_request() {
  return std::make_shared<std::atomic<bool>>(false);
}

ProgressEngine::ProgressEngine() {
  stop_flag = false;
  started_flag = false;
  world_comm = new MPICommunicator(MPI_COMM_WORLD);
  // Initialze with the default stream.
  num_input_streams = 1;
  stream_to_queue[DEFAULT_STREAM] = &request_queues[0];
}

ProgressEngine::~ProgressEngine() {
  delete world_comm;
}

void ProgressEngine::run() {
#ifdef AL_HAS_CUDA
  // Capture the current CUDA device for the progress engine.
  int device;
  AL_CHECK_CUDA(cudaGetDevice(&device));
  cur_device = device;
#endif
  thread = std::thread(&ProgressEngine::engine, this);
  profiling::name_thread(thread.native_handle(), "al-progress");
  // Wait for the progress engine to start.
  std::unique_lock<std::mutex> lock(startup_mutex);
  startup_cv.wait(lock, [this] {return started_flag.load() == true;});
}

void ProgressEngine::stop() {
  if (stop_flag.load()) {
    throw_al_exception("Stop called twice on progress engine");
  }
  stop_flag.store(true, std::memory_order_release);
  thread.join();
}

void ProgressEngine::enqueue(AlState* state) {
  // Find the correct input queue for the stream, creating it if needed.
  auto iter = stream_to_queue.find(state->get_compute_stream());
  if (iter != stream_to_queue.end()) {
    iter->second->q.push(state);
  } else {
    size_t cur_stream = num_input_streams.load();
    if (cur_stream == AL_PE_NUM_STREAMS) {
      throw_al_exception("Using more streams than supported!");
    }
    request_queues[cur_stream].compute_stream = state->get_compute_stream();
    stream_to_queue[state->get_compute_stream()] = &request_queues[cur_stream];
    request_queues[cur_stream].q.push(state);
    ++num_input_streams;
  }
}

bool ProgressEngine::is_complete(AlRequest& req) {
  if (req == NULL_REQUEST) {
    return true;
  }
  if (req->load(std::memory_order_acquire)) {
    req = NULL_REQUEST;
    return true;
  }
  return false;
}

void ProgressEngine::wait_for_completion(AlRequest& req) {
  if (req == NULL_REQUEST) {
    return;
  }
  // Spin until the request has completed.
  while (!req->load(std::memory_order_acquire)) {}
  req = NULL_REQUEST;
}

std::ostream& ProgressEngine::dump_state(std::ostream& ss) {
  // Note: This pulls *directly from internal state*.
  // This is *not* thread safe, and stuff might blow up.
  // You should only be dumping state where you don't care about that anyway.
  for (auto&& stream_pipeline_pair : run_queues) {
    ss << "Pipelined run queue for stream " << stream_pipeline_pair.first << ":\n";
    auto&& pipeline = stream_pipeline_pair.second;
    for (size_t stage = 0; stage < AL_PE_NUM_PIPELINE_STAGES; ++stage) {
      const size_t stage_queue_size = pipeline[stage].size();
      ss << "Stage " << stage << " run queue (" << stage_queue_size << "):\n";
      for (size_t i = 0; i < stage_queue_size; ++i) {
        ss << i << ": ";
        if (pipeline[stage][i]) {
          ss << pipeline[stage][i]->get_name() << " "
             << pipeline[stage][i]->get_desc() << "\n";
        } else {
          ss << "(unknown)\n";
        }
      }
    }
  }
  const size_t req_queue_size = num_input_streams.load();
  ss << "Request queues (" << req_queue_size << "):\n";
  for (size_t i = 0; i < req_queue_size; ++i) {
    ss << i << ": blocked=" << request_queues[i].blocked;
    const size_t front = request_queues[i].q.front.load();
    const size_t back = request_queues[i].q.back.load();
    ss << " front=" << front << " back=" << back << "\n";
    for (size_t j = front; j < back; ++j) {
      ss << "\t" << j << ": " << request_queues[i].q.data[j]->get_name()
         << " " << request_queues[i].q.data[j]->get_desc() << "\n";
    }
  }
  return ss;
}

void ProgressEngine::bind() {
  // Determine topology information.
  hwloc_topology_t topo;
  hwloc_topology_init(&topo);
  hwloc_topology_load(topo);
  // Determine how many NUMA nodes there are.
  int num_numa_nodes = hwloc_get_nbobjs_by_type(topo, HWLOC_OBJ_NUMANODE);
  if (num_numa_nodes == -1) {
    throw_al_exception("Cannot determine number of NUMA nodes.");
  }
  // Determine the NUMA node we're currently on.
  hwloc_cpuset_t cpuset = hwloc_bitmap_alloc();
  hwloc_get_cpubind(topo, cpuset, 0);
  hwloc_nodeset_t nodeset = hwloc_bitmap_alloc();
  hwloc_cpuset_to_nodeset(topo, cpuset, nodeset);
  hwloc_bitmap_singlify(nodeset);
  hwloc_obj_t numa_node = hwloc_get_numanode_obj_by_os_index(
    topo, hwloc_bitmap_first(nodeset));
  if (numa_node == NULL) {
    throw_al_exception("Could not get NUMA node.");
  }
  int core_to_bind = -1;
  // Check if the core has been manually set.
  char* env = std::getenv("AL_PROGRESS_CORE");
  if (env) {
    // Note: This still binds within the current NUMA node.
    core_to_bind = std::atoi(env);
  } else {
    // Determine how many cores are in this NUMA node.
    int num_cores = hwloc_get_nbobjs_inside_cpuset_by_type(
      topo, numa_node->cpuset, HWLOC_OBJ_CORE);
    if (num_cores <= 0) {
      throw_al_exception("Could not determine number of cores.");
    }
    // Determine which core on this NUMA node to map us to.
    // Support specifying this in the environment too.
    int ranks_per_numa_node = -1;
    env = std::getenv("AL_PROGRESS_RANKS_PER_NUMA_NODE");
    if (env) {
      ranks_per_numa_node = std::atoi(env);
    } else {
      // Note: This doesn't handle the case where things aren't evenly divisible.
      ranks_per_numa_node = std::max(
        1, world_comm->local_size() / num_numa_nodes);
    }
    int numa_rank = world_comm->local_rank() % ranks_per_numa_node;
    if (numa_rank > num_cores) {
      throw_al_exception("Not enough cores to bind to.");
    }
    // Assume the NUMA node is partitioned among the ranks on it, and bind to
    // the last core in our chunk.
    core_to_bind = (numa_rank + 1)*(num_cores / ranks_per_numa_node) - 1;
  }
  hwloc_obj_t core = hwloc_get_obj_inside_cpuset_by_type(
    topo, numa_node->cpuset, HWLOC_OBJ_CORE, core_to_bind);
  if (core == NULL) {
    throw_al_exception("Could not get core.");
  }
  hwloc_cpuset_t coreset = hwloc_bitmap_dup(core->cpuset);
  hwloc_bitmap_singlify(coreset);
  if (hwloc_set_cpubind(topo, coreset, HWLOC_CPUBIND_THREAD) == -1) {
    throw_al_exception("Cannot bind progress engine");
  }
  hwloc_bitmap_free(cpuset);
  hwloc_bitmap_free(nodeset);
  hwloc_bitmap_free(coreset);
  hwloc_topology_destroy(topo);
}

void ProgressEngine::engine() {
#ifdef AL_HAS_CUDA
  // Set the current CUDA device for the thread.
  AL_CHECK_CUDA_NOSYNC(cudaSetDevice(cur_device.load()));
#endif
  bind();
  // Notify the main thread we're now running.
  {
    std::unique_lock<std::mutex> lock(startup_mutex);
    started_flag = true;
  }
  startup_cv.notify_one();
  while (!stop_flag.load(std::memory_order_acquire)) {
    // Check for newly-submitted requests.
    size_t cur_input_streams = num_input_streams.load();
    for (size_t i = 0; i < cur_input_streams; ++i) {
      if (!request_queues[i].blocked) {
        AlState* req = request_queues[i].q.peek();
        if (req != nullptr) {
          // Add to the correct run queue if one is available.
          bool do_start = false;
          switch (req->get_run_type()) {
          case RunType::bounded:
            if (num_bounded < AL_PE_NUM_CONCURRENT_OPS) {
              ++num_bounded;
              do_start = true;
            }
            break;
          case RunType::unbounded:
            do_start = true;
            break;
          }
          if (do_start) {
            // Add to end of first pipeline stage.
            // Create run queues if needed.
            if (!run_queues.count(req->get_compute_stream())) {
              run_queues.emplace(req->get_compute_stream(),
                                 decltype(run_queues)::mapped_type{});
            }
            run_queues[req->get_compute_stream()][0].push_back(req);
            req->start();
#ifdef AL_DEBUG_HANG_CHECK
            req->start_time = get_time();
#endif
#ifdef AL_TRACE
            trace::record_pe_start(*req);
#endif
            request_queues[i].q.pop_always();
            if (req->blocks()) {
              request_queues[i].blocked = true;
              blocking_reqs[req] = i;
            }
          }
        }
      }
    }
    // Process one step of each in-progress request.
    for (auto&& stream_pipeline_pair : run_queues) {
      auto&& pipeline = stream_pipeline_pair.second;
      for (size_t stage = 0; stage < AL_PE_NUM_PIPELINE_STAGES; ++stage) {
        // Process this stage of the pipeline.
        for (auto i = pipeline[stage].begin(); i != pipeline[stage].end();) {
          AlState* req = *i;
          // Simply skip over paused states.
          if (req->paused_for_advance) {
            ++i;
          } else {
            PEAction action = req->step();
            switch (action) {
            case PEAction::cont:
              // Nothing to do here.
#ifdef AL_DEBUG_HANG_CHECK
              // Check whether we have hung.
              if (!req->hang_reported) {
                double t = get_time();
                if (t - req->start_time > 10.0 + world_comm->rank()) {
                  std::cout << world_comm->rank()
                            << ": Progress engine detected a possible hang"
                            << " state=" << req << " " << req->get_name()
                            << " compute_stream=" << req->get_compute_stream()
                            << " run_type="
                            << (req->get_run_type() == RunType::bounded ? "bounded" : "unbounded")
                            << " blocks=" << req->blocks() << std::endl;
                  req->hang_reported = true;
                }
              }
#endif
              ++i;
              break;
            case PEAction::advance:
#ifdef AL_DEBUG
              // Ensure we don't advance too far.
              if (stage + 1 >= AL_PE_NUM_PIPELINE_STAGES) {
                throw_al_exception("Trying to advance pipeline stage too far");
              }
#endif
              // Only move if this is the head of the pipeline stage.
              if (i == pipeline[stage].begin()) {
                pipeline[stage+1].push_back(req);
                i = pipeline[stage].erase(i);
              } else {
                req->paused_for_advance = true;
                ++i;
              }
              break;
            case PEAction::complete:
              if (req->needs_completion()) {
                req->get_req()->store(true, std::memory_order_release);
              }
              if (req->get_run_type() == RunType::bounded) {
                --num_bounded;
              }
              if (req->blocks()) {
                // Unblock the associated input queue.
                request_queues[blocking_reqs[req]].blocked = false;
                blocking_reqs.erase(req);
              }
#ifdef AL_TRACE
              trace::record_pe_done(*req);
#endif
              delete req;
              i = pipeline[stage].erase(i);
              break;
            default:
              throw_al_exception("Unknown PEAction");
              break;
            }
          }
        }
        // Check whether we can advance paused states.
        for (auto i = pipeline[stage].begin(); i != pipeline[stage].end();) {
          AlState* req = *i;
          if (req->paused_for_advance) {
            // Move to the next stage.
            req->paused_for_advance = false;
            pipeline[stage+1].push_back(req);
            i = pipeline[stage].erase(i);
          } else {
            break;  // Nothing at the head to advance.
          }
        }
      }
    }
  }
}

}  // namespace internal
}  // namespace Al
