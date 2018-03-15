#ifdef ALUMINUM_HAS_NCCL
#include "allreduce_nccl_impl.hpp"

// Error checking macros
#define CUDACHECK(cmd) do {                     \
  cudaError_t e = cmd;                          \
  if (e != cudaSuccess) {                       \
    printf("CUDA failure %s:%d '%s'\n",         \
           __FILE__, __LINE__,                  \
           cudaGetErrorString(e));              \
    exit(EXIT_FAILURE);                         \
  }                                             \
} while(0)
#define NCCLCHECK(cmd) do {                     \
  ncclResult_t r = cmd;                         \
  if (r!= ncclSuccess) {                        \
    printf("NCCL failure %s:%d '%s'\n",         \
           __FILE__, __LINE__,                  \
           ncclGetErrorString(r));              \
    exit(EXIT_FAILURE);                         \
  }                                             \
} while(0)

namespace allreduces {

NCCLCommunicator::NCCLCommunicator(MPI_Comm comm_, std::vector<int> gpus)
  : MPICommunicator(comm_),
    m_gpus(gpus),
    m_num_gpus(gpus.size()) {
  MPI_Comm_dup(comm_, &mpi_comm);
  gpu_setup();
  nccl_setup();
}

NCCLCommunicator::~NCCLCommunicator() {
  nccl_destroy();
  for (size_t i=0; i<m_gpus.size(); ++i) {
    CUDACHECK(cudaSetDevice(m_gpus[i]));
    CUDACHECK(cudaStreamDestroy(m_streams[i]));
  }
}

void NCCLCommunicator::gpu_setup() {

  // Initialize list of GPUs
  if (m_gpus.empty()) {
    m_gpus.push_back(0);
    CUDACHECK(cudaGetDevice(&m_gpus.back()));
  }
  m_num_gpus = m_gpus.size();

  // Initialize streams
  for (const auto& gpu : m_gpus) {
    CUDACHECK(cudaSetDevice(gpu));
    m_streams.push_back(nullptr);
    CUDACHECK(cudaStreamCreate(&m_streams.back()));
  }

}

void NCCLCommunicator::nccl_setup() {

  if(m_num_gpus != 1){
    std::cerr << "NCCLCommunicator: rank " << rank() << ": the number of GPUs assigned to process is " << m_num_gpus << "; should be 1\n";
    MPI_Abort(mpi_comm, -3);
  }

  int num_gpus_assigned = m_num_gpus;
  int nProcs = size();
  int myid = rank();
  int total_num_comms = nProcs*num_gpus_assigned;

  ncclUniqueId ncclId;
  if (myid == 0) {
    NCCLCHECK(ncclGetUniqueId(&ncclId));
  }

  MPI_Bcast(&ncclId, sizeof(ncclId), MPI_BYTE, 0, mpi_comm);

  if (nProcs == 1) {
    int gpuArray = 0;
    NCCLCHECK(ncclCommInitAll(&m_nccl_comm, 1, &gpuArray));
  }
  else {
    NCCLCHECK(ncclCommInitRank(&m_nccl_comm, total_num_comms, ncclId, num_gpus_assigned*myid));
/*
    if(num_gpus_assigned > 1) NCCLCHECK(ncclGroupStart());
    for(int i=0; i<num_gpus_assigned; i++){
      CUDACHECK(cudaSetDevice(m_gpus[i]));
      NCCLCHECK(ncclCommInitRank(&(m_nccl_comm[i]), total_num_comms, ncclId, num_gpus_assigned*myid+i));
    }
    if(num_gpus_assigned > 1) NCCLCHECK(ncclGroupEnd());
*/
  }
} // nccl_setup

void NCCLCommunicator::nccl_destroy() {
  ncclCommDestroy(m_nccl_comm);
/*
  int num_gpus_assigned = m_num_gpus;
  synchronize();
  for(int i=0; i<num_gpus_assigned; i++){
    ncclCommDestroy(m_nccl_comm[i]);
  }
*/
}

void NCCLCommunicator::synchronize() {
  for (int i = 0; i < m_num_gpus; ++i) {
    cudaSetDevice(m_gpus[i]);
    cudaStreamSynchronize(m_streams[i]);
  }
}

cudaStream_t NCCLCommunicator::get_default_stream() {
  return m_streams[0];
}

/// It is assumed that both sendbuf and recvbuf are in device memory
/// for NCCL sendbuf and recvbuf can be identical; in-place operation will be performed
void NCCLCommunicator::Allreduce(void* sendbuf, void* recvbuf, size_t count, ncclDataType_t nccl_type,
               ncclRedOp_t nccl_redop, cudaStream_t default_stream) {

  if(count == 0) return;

  NCCLCHECK(ncclAllReduce(sendbuf, recvbuf, count, nccl_type, nccl_redop, m_nccl_comm, default_stream));

/*
  int num_gpus_assigned = m_gpus.size();

  if(num_gpus_assigned > 1) ncclGroupStart();
  for(int i = 0; i < num_gpus_assigned; ++i) {
    CUDACHECK(cudaSetDevice(m_gpus[i]));
    NCCLCHECK(ncclAllReduce(sendbuf, recvbuf, count, nccl_type, nccl_redop, m_nccl_comm[i], m_streams[i]));
  }
  if(num_gpus_assigned > 1) ncclGroupEnd();
*/

}


/// It is assumed that both sendbuf and recvbuf are in device memory
/// for NCCL sendbuf and recvbuf can be identical; in-place operation will be performed
void NCCLCommunicator::Reduce(void* sendbuf, void* recvbuf, size_t count, ncclDataType_t nccl_type,
               ncclRedOp_t nccl_redop, int root, cudaStream_t default_stream) {

  if(count == 0) return;

  NCCLCHECK(ncclReduce(sendbuf, recvbuf, count, nccl_type, nccl_redop, root, m_nccl_comm, default_stream));

/*
  int num_gpus_assigned = m_gpus.size();

  if(num_gpus_assigned > 1) ncclGroupStart();
  for(int i = 0; i < num_gpus_assigned; ++i) {
    CUDACHECK(cudaSetDevice(m_gpus[i]));
    NCCLCHECK(ncclAllReduce(sendbuf, recvbuf, count, nccl_type, nccl_redop, m_nccl_comm[i], m_streams[i]));
  }
  if(num_gpus_assigned > 1) ncclGroupEnd();
*/

}

void NCCLCommunicator::Bcast(void* sendbuf, size_t count, ncclDataType_t nccl_type, int root, cudaStream_t default_stream) {

  if(count == 0) return;
  NCCLCHECK(ncclBcast(sendbuf, count, nccl_type, root, m_nccl_comm, default_stream));

/*
  int num_gpus_assigned = m_gpus.size();

  if(num_gpus_assigned > 1) ncclGroupStart();
  for(int i = 0; i < num_gpus_assigned; ++i) {
    CUDACHECK(cudaSetDevice(m_gpus[i]));
    NCCLCHECK(ncclAllReduce(sendbuf, recvbuf, count, nccl_type, nccl_redop, m_nccl_comm[i], m_streams[i]));
  }
  if(num_gpus_assigned > 1) ncclGroupEnd();
*/

}

/**
 * It is assumed for NCCL-based Allgather that
 *   1. send and recv buffers have the same type
 *   2. the recv_count is computed implicitly by NCCL
 *   3. recv buffer should have a size of at least num_ranks*send_count
*/
void NCCLCommunicator::Allgather(void* sendbuf, void* recvbuf, size_t send_count,
                 ncclDataType_t nccl_type, cudaStream_t default_stream) {

  if(send_count == 0) return;
  NCCLCHECK(ncclAllGather(sendbuf, recvbuf, send_count, nccl_type, m_nccl_comm, default_stream));
}

/**
 * It is assumed that both sendbuf and recvbuf are in device memory
 * For NCCL sendbuf and recvbuf can be identical; in-place operation will be performed
 * NCCL-based Reduce_scatter assumes that send_count is equal to num_ranks*recv_count, which means
 * that send_buf should have a size of at least num_ranks*recv_count elements
*/
void NCCLCommunicator::Reduce_scatter(void* sendbuf, void* recvbuf, size_t recv_count, ncclDataType_t nccl_type,
                 ncclRedOp_t nccl_redop, cudaStream_t default_stream) {

  if(recv_count == 0) return;
  NCCLCHECK(ncclReduceScatter(sendbuf, recvbuf, recv_count, nccl_type, nccl_redop, m_nccl_comm, default_stream));
}

} // namespace allreduces
#endif