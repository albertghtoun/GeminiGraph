/*
Copyright (c) 2015-2016 Xiaowei Zhu, Tsinghua University

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.
*/

#ifndef GRAPH_HPP
#define GRAPH_HPP

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <malloc.h>
#include <sys/mman.h>
#include <numa.h>
#include <omp.h>

#include <string>
#include <vector>
#include <thread>
#include <mutex>

#include "core/atomic.hpp"
#include "core/bitmap.hpp"
#include "core/constants.hpp"
#include "core/filesystem.hpp"
#include "core/mpi.hpp"
#include "core/time.hpp"
#include "core/type.hpp"

#include "../../fmgf_core/fm.h"


enum ThreadStatus {
  WORKING,
  STEALING,
  WORKING_REMOTE,
  STEALING_REMOTE
};

enum MessageTag {
  ShuffleGraph,
  PassMessage,
  GatherVertexArray
};

struct ThreadState {
  VertexId curr;
  VertexId end;
  ThreadStatus status;
};

struct MessageBuffer {
  size_t capacity;
  int count; // the actual size (i.e. bytes) should be sizeof(element) * count
  int owned_count;  // the count of sender's locally owned vertices as MsgUnit in the buffer.
  int delegated_start[8]; // the starting index in the send_buffer that represents the delegated messages for far memory machines of partition i. 
                          // for now, the max number of far memory machines supported is 8 - 1 = 7.
  char * data;
  MessageBuffer () {
    capacity = 0;
    count = 0;
    owned_count = 0;
    memset(delegated_start, 0, sizeof(int)*8);
    data = NULL;
  }
  void init (int socket_id) {
    capacity = 4096;
    count = 0;
    owned_count = 0;
    memset(delegated_start, 0, sizeof(int)*8);
    data = (char*)numa_alloc_onnode(capacity, socket_id);
  }
  void resize(size_t new_capacity) {
    if (new_capacity > capacity) {
      char * new_data = (char*)numa_realloc(data, capacity, new_capacity);
      assert(new_data!=NULL);
      data = new_data;
      capacity = new_capacity;
    }
  }
};

template <typename MsgData>
struct MsgUnit {
  VertexId vertex;
  MsgData msg_data;
} __attribute__((packed));

template <typename EdgeData = Empty>
class Graph {
public:
  int partition_id;
  int partitions;

  size_t alpha;

  int threads;
  int sockets;
  int threads_per_socket;

  size_t edge_data_size;
  size_t unit_size;
  size_t edge_unit_size;

  bool symmetric;
  VertexId vertices;
  EdgeId edges;
  VertexId * out_degree; // VertexId [vertices]; numa-aware
  VertexId * in_degree; // VertexId [vertices]; numa-aware

  VertexId * partition_offset; // VertexId [partitions+1]
  VertexId * local_partition_offset; // VertexId [sockets+1]

  VertexId owned_vertices;
  EdgeId * outgoing_edges; // EdgeId [sockets]
  EdgeId * incoming_edges; // EdgeId [sockets]

  Bitmap ** incoming_adj_bitmap;
  EdgeId ** incoming_adj_index; // EdgeId [sockets] [vertices+1]; numa-aware
  AdjUnit<EdgeData> ** incoming_adj_list; // AdjUnit<EdgeData> [sockets] [vertices+1]; numa-aware
  MPI_Win*** incoming_adj_bitmap_data_win;
  MPI_Win*** incoming_adj_index_data_win;
  MPI_Win*** incoming_adj_list_data_win;

  Bitmap ** outgoing_adj_bitmap;
  EdgeId ** outgoing_adj_index; // EdgeId [sockets] [vertices+1]; numa-aware
  AdjUnit<EdgeData> ** outgoing_adj_list; // AdjUnit<EdgeData> [sockets] [vertices+1]; numa-aware
  MPI_Win*** outgoing_adj_bitmap_data_win; 
  MPI_Win*** outgoing_adj_index_data_win;
  MPI_Win*** outgoing_adj_list_data_win;

  VertexId * compressed_incoming_adj_vertices;
  CompressedAdjIndexUnit ** compressed_incoming_adj_index; // CompressedAdjIndexUnit [sockets] [...+1]; numa-aware
  VertexId * compressed_outgoing_adj_vertices;
  CompressedAdjIndexUnit ** compressed_outgoing_adj_index; // CompressedAdjIndexUnit [sockets] [...+1]; numa-aware

  ThreadState ** thread_state; // ThreadState* [threads]; numa-aware
  ThreadState ** tuned_chunks_dense; // ThreadState [partitions][threads];
  ThreadState ** tuned_chunks_sparse; // ThreadState [partitions][threads];

  size_t local_send_buffer_limit;
  MessageBuffer ** local_send_buffer; // MessageBuffer* [threads]; numa-aware

  int current_send_part_id;
  MessageBuffer *** send_buffer; // MessageBuffer* [partitions] [sockets]; numa-aware
  MessageBuffer *** recv_buffer; // MessageBuffer* [partitions] [sockets]; numa-aware

  #if ENABLE_BITMAP_CACHE == 1
  FM::bitmap_cache_item **** outgoing_adj_bitmap_cache; // outgoing bitmap cache.
  FM::bitmap_cache_pool_t* outgoing_adj_bitmap_cache_pool;
  FM::bitmap_cache_item **** incoming_adj_bitmap_cache; // incoming bitmap cache.
  FM::bitmap_cache_pool_t* incoming_adj_bitmap_cache_pool;
  #endif
  #if ENABLE_INDEX_CACHE == 1
  FM::index_cache_item **** outgoing_adj_index_cache;  // outgoing index cache.
  FM::index_cache_pool_t* outgoing_adj_index_cache_pool;
  FM::index_cache_item **** incoming_adj_index_cache; // incoming index cache.
  FM::index_cache_pool_t* incoming_adj_index_cache_pool;
  #endif
  #if ENABLE_EDGE_CACHE == 1
  FM::edge_cache_set<EdgeData> **** outgoing_edge_cache;     // outgoing edge cache.
  FM::edge_cache_pool_t<EdgeData>* outgoing_edge_cache_pool;  // outgoing edge cache pool.
  FM::edge_cache_set<EdgeData> **** incoming_edge_cache;     // incoming edge cache.
  FM::edge_cache_pool_t<EdgeData>* incoming_edge_cache_pool;  // incoming edge cache pool.
  #endif

  Graph() {
    threads = numa_num_configured_cpus();
    sockets = 1; // numa_num_configured_nodes();
    threads_per_socket = threads / sockets;

    init();
  }

  ~Graph() {
    MPI_Barrier(MPI_COMM_WORLD);
    for (int s_i = 0; s_i < sockets; s_i++) {
      for (int t_i = 0; t_i < threads; t_i++) {
        MPI_Win_free(outgoing_adj_bitmap_data_win[s_i][t_i]);
        MPI_Win_free(outgoing_adj_index_data_win[s_i][t_i]);
        MPI_Win_free(outgoing_adj_list_data_win[s_i][t_i]);
        MPI_Win_free(incoming_adj_bitmap_data_win[s_i][t_i]);
        MPI_Win_free(incoming_adj_index_data_win[s_i][t_i]);
        MPI_Win_free(incoming_adj_list_data_win[s_i][t_i]);
      }
    }
  }

  inline int get_socket_id(int thread_id) {
    return thread_id / threads_per_socket;
  }

  inline int get_socket_offset(int thread_id) {
    return thread_id % threads_per_socket;
  }

  void init() {
    edge_data_size = std::is_same<EdgeData, Empty>::value ? 0 : sizeof(EdgeData);
    unit_size = sizeof(VertexId) + edge_data_size;
    edge_unit_size = sizeof(VertexId) + unit_size;

    assert( numa_available() != -1 );
    assert( sizeof(unsigned long) == 8 ); // assume unsigned long is 64-bit

    char nodestring[sockets*2+1];
    nodestring[0] = '0';
    for (int s_i=1;s_i<sockets;s_i++) {
      nodestring[s_i*2-1] = ',';
      nodestring[s_i*2] = '0'+s_i;
    }
    struct bitmask * nodemask = numa_parse_nodestring(nodestring);
    numa_set_interleave_mask(nodemask);

    omp_set_dynamic(0);
    omp_set_num_threads(threads);
    thread_state = new ThreadState * [threads];
    local_send_buffer_limit = 16;
    local_send_buffer = new MessageBuffer * [threads];
    for (int t_i=0;t_i<threads;t_i++) {
      thread_state[t_i] = (ThreadState*)numa_alloc_onnode( sizeof(ThreadState), get_socket_id(t_i));
      local_send_buffer[t_i] = (MessageBuffer*)numa_alloc_onnode( sizeof(MessageBuffer), get_socket_id(t_i));
      local_send_buffer[t_i]->init(get_socket_id(t_i));
    }
    #pragma omp parallel for
    for (int t_i=0;t_i<threads;t_i++) {
      int s_i = get_socket_id(t_i);
      assert(numa_run_on_node(s_i)==0);
      #ifdef PRINT_DEBUG_MESSAGES
      // fprintf(stderr,"thread-%d bound to socket-%d\n", t_i, s_i);
      #endif
    }
    #ifdef PRINT_DEBUG_MESSAGES
    // fprintf(stderr,"threads=%d*%d\n", sockets, threads_per_socket);
    // fprintf(stderr,"interleave on %s\n", nodestring);
    #endif

    MPI_Comm_rank(MPI_COMM_WORLD, &partition_id);
    MPI_Comm_size(MPI_COMM_WORLD, &partitions);
    assert(FM::n_compute_partitions <= partitions);
    int color = partition_id < FM::n_compute_partitions ? 0 : 1;
    MPI_Comm_split(MPI_COMM_WORLD, color, partition_id, &FM::compute_comm_world);

    send_buffer = new MessageBuffer ** [partitions];
    recv_buffer = new MessageBuffer ** [partitions];
    #if ENABLE_BITMAP_CACHE == 1
    outgoing_adj_bitmap_cache_pool = new FM::bitmap_cache_pool_t;
    incoming_adj_bitmap_cache_pool = new FM::bitmap_cache_pool_t;
    outgoing_adj_bitmap_cache = new FM::bitmap_cache_item *** [partitions];
    incoming_adj_bitmap_cache = new FM::bitmap_cache_item *** [partitions];
    #endif
    #if ENABLE_INDEX_CACHE == 1
    outgoing_adj_index_cache_pool = new FM::index_cache_pool_t;
    incoming_adj_index_cache_pool = new FM::index_cache_pool_t;
    outgoing_adj_index_cache = new FM::index_cache_item *** [partitions];
    incoming_adj_index_cache = new FM::index_cache_item *** [partitions];
    #endif
    #if ENABLE_EDGE_CACHE == 1
    outgoing_edge_cache_pool = new FM::edge_cache_pool_t<EdgeData>;
    incoming_edge_cache_pool = new FM::edge_cache_pool_t<EdgeData>;
    outgoing_edge_cache = new FM::edge_cache_set<EdgeData> *** [partitions];
    incoming_edge_cache = new FM::edge_cache_set<EdgeData> *** [partitions];
    #endif
    for (int i=0;i<partitions;i++) {
      send_buffer[i] = new MessageBuffer * [sockets];
      recv_buffer[i] = new MessageBuffer * [sockets];
      #if ENABLE_BITMAP_CACHE == 1
      outgoing_adj_bitmap_cache[i] = new FM::bitmap_cache_item ** [sockets];
      incoming_adj_bitmap_cache[i] = new FM::bitmap_cache_item ** [sockets];
      #endif
      #if ENABLE_INDEX_CACHE == 1
      outgoing_adj_index_cache[i] = new FM::index_cache_item ** [sockets];
      incoming_adj_index_cache[i] = new FM::index_cache_item ** [sockets];
      #endif
      #if ENABLE_EDGE_CACHE == 1
      outgoing_edge_cache[i] = new FM::edge_cache_set<EdgeData> ** [sockets];
      incoming_edge_cache[i] = new FM::edge_cache_set<EdgeData> ** [sockets];
      #endif
      for (int s_i=0;s_i<sockets;s_i++) {
        send_buffer[i][s_i] = (MessageBuffer*)numa_alloc_onnode( sizeof(MessageBuffer), s_i);
        send_buffer[i][s_i]->init(s_i);
        recv_buffer[i][s_i] = (MessageBuffer*)numa_alloc_onnode( sizeof(MessageBuffer), s_i);
        recv_buffer[i][s_i]->init(s_i);
      }
    }

    alpha = 8 * (partitions - 1);

    #if ENABLE_BITMAP_CACHE == 1
    FM::init_bitmap_stats();
    #endif
    #if ENABLE_INDEX_CACHE == 1
    FM::init_index_stats();
    #endif
    #if ENABLE_EDGE_CACHE == 1
    FM::init_edge_stats();
    #endif

    MPI_Barrier(MPI_COMM_WORLD);
  }

  // fill a vertex array with a specific value
  template<typename T>
  void fill_vertex_array(T * array, T value) {
    // fill out the vertex array in my partition.
    #pragma omp parallel for
    for (VertexId v_i=partition_offset[partition_id];v_i<partition_offset[partition_id+1];v_i++) {
      array[v_i] = value;
    }

    // fill out the deligated portion of the vertex array.
    #pragma omp parallel for
    for (VertexId v_i=partition_offset[FM::n_compute_partitions];v_i<partition_offset[partitions];v_i++) {
      array[v_i] = value;
    }
  }

  // allocate a numa-aware vertex array
  template<typename T>
  T * alloc_vertex_array() {
    char * array = (char *)mmap(NULL, sizeof(T) * vertices, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(array!=NULL);
    for (int s_i=0;s_i<sockets;s_i++) {
      numa_tonode_memory(array + sizeof(T) * local_partition_offset[s_i], sizeof(T) * (local_partition_offset[s_i+1] - local_partition_offset[s_i]), s_i);
    }
    return (T*)array;
  }

  // deallocate a vertex array
  template<typename T>
  void dealloc_vertex_array(T * array) {
    numa_free(array, sizeof(T) * vertices);
  }

  // allocate a numa-oblivious vertex array
  template<typename T>
  T * alloc_interleaved_vertex_array() {
    T * array = (T *)numa_alloc_interleaved( sizeof(T) * vertices );
    assert(array!=NULL);
    return array;
  }

  // dump a vertex array to path
  template<typename T>
  void dump_vertex_array(T * array, std::string path) {
    long file_length = sizeof(T) * vertices;
    if (!file_exists(path) || file_size(path) != file_length) {
      if (partition_id==0) {
        FILE * fout = fopen(path.c_str(), "wb");
        char * buffer = new char [PAGESIZE];
        for (long offset=0;offset<file_length;) {
          if (file_length - offset >= PAGESIZE) {
            fwrite(buffer, 1, PAGESIZE, fout);
            offset += PAGESIZE;
          } else {
            fwrite(buffer, 1, file_length - offset, fout);
            offset += file_length - offset;
          }
        }
        fclose(fout);
      }
      MPI_Barrier(MPI_COMM_WORLD);
    }
    int fd = open(path.c_str(), O_RDWR);
    assert(fd!=-1);
    long offset = sizeof(T) * partition_offset[partition_id];
    long end_offset = sizeof(T) * partition_offset[partition_id+1];
    void * data = (void *)array;
    assert(lseek(fd, offset, SEEK_SET)!=-1);
    while (offset < end_offset) {
      long bytes = write(fd, data + offset, end_offset - offset);
      assert(bytes!=-1);
      offset += bytes;
    }
    assert(close(fd)==0);
  }

  // restore a vertex array from path
  template<typename T>
  void restore_vertex_array(T * array, std::string path) {
    long file_length = sizeof(T) * vertices;
    if (!file_exists(path) || file_size(path) != file_length) {
      assert(false);
    }
    int fd = open(path.c_str(), O_RDWR);
    assert(fd!=-1);
    long offset = sizeof(T) * partition_offset[partition_id];
    long end_offset = sizeof(T) * partition_offset[partition_id+1];
    void * data = (void *)array;
    assert(lseek(fd, offset, SEEK_SET)!=-1);
    while (offset < end_offset) {
      long bytes = read(fd, data + offset, end_offset - offset);
      assert(bytes!=-1);
      offset += bytes;
    }
    assert(close(fd)==0);
  }

  // gather a vertex array
  template<typename T>
  void gather_vertex_array(T * array, int root) {
    assert(root < FM::n_compute_partitions);
    if (partition_id!=root) {
      MPI_Send(array + partition_offset[partition_id], sizeof(T) * owned_vertices, MPI_CHAR, root, GatherVertexArray, FM::compute_comm_world);
    } else {
      for (int i=0;i<FM::n_compute_partitions;i++) {
        if (i==partition_id) continue;
        MPI_Status recv_status;
        MPI_Recv(array + partition_offset[i], sizeof(T) * (partition_offset[i + 1] - partition_offset[i]), MPI_CHAR, i, GatherVertexArray, FM::compute_comm_world, &recv_status);
        int length;
        MPI_Get_count(&recv_status, MPI_CHAR, &length);
        assert(length == sizeof(T) * (partition_offset[i + 1] - partition_offset[i]));
      }
    }

    if (FM::n_compute_partitions < partitions) {
      if (partition_id!=root) {
        for (uint i = FM::n_compute_partitions; i < partitions; ++i) {
          if (i % FM::n_compute_partitions == partition_id) {
            MPI_Send(array + partition_offset[i], sizeof(T) * (partition_offset[i + 1] - partition_offset[i]), MPI_CHAR, root, GatherVertexArray, FM::compute_comm_world);
          }
        }
      } else {
        for (uint i = FM::n_compute_partitions; i < partitions; ++i) {
          uint delegated_partition = i % FM::n_compute_partitions;
          if (delegated_partition==partition_id) continue;
          MPI_Status recv_status;
          MPI_Recv(array + partition_offset[i], sizeof(T) * (partition_offset[i + 1] - partition_offset[i]), MPI_CHAR, delegated_partition, GatherVertexArray, FM::compute_comm_world, &recv_status);
          int length;
          MPI_Get_count(&recv_status, MPI_CHAR, &length);
          assert(length == sizeof(T) * (partition_offset[i + 1] - partition_offset[i]));          
        }
      }
    }
  }

  std::vector<uint> get_delegated_partitions(unsigned part_id) {
    std::vector<uint> delegated_farmem_partitions;
    for (int i = FM::n_compute_partitions; i < partitions; ++i) {
      if (i % FM::n_compute_partitions == part_id) {
        delegated_farmem_partitions.push_back(i);
      }
    }
    return delegated_farmem_partitions;
  }

  // allocate a vertex subset
  VertexSubset * alloc_vertex_subset() {
    return new VertexSubset(vertices);
  }

  int get_partition_id(VertexId v_i){
    for (int i=0;i<partitions;i++) {
      if (v_i >= partition_offset[i] && v_i < partition_offset[i+1]) {
        return i;
      }
    }

    assert(false);
  }

  int get_local_partition_id(VertexId v_i){
    for (int s_i=0;s_i<sockets;s_i++) {
      if (v_i >= local_partition_offset[s_i] && v_i < local_partition_offset[s_i+1]) {
        return s_i;
      }
    }
    assert(false);
  }

  // load a directed graph and make it undirected
  void load_undirected_from_directed(std::string path, VertexId vertices) {
    double prep_time = 0;
    prep_time -= MPI_Wtime();

    symmetric = true;

    MPI_Datatype vid_t = get_mpi_data_type<VertexId>();

    this->vertices = vertices;
    long total_bytes = file_size(path.c_str());
    this->edges = total_bytes / edge_unit_size;
    #ifdef PRINT_DEBUG_MESSAGES
    if (partition_id==0) {
      fprintf(stderr,"|V| = %u, |E| = %lu\n", vertices, edges);
    }
    #endif

    EdgeId read_edges = edges / partitions;
    if (partition_id==partitions-1) {
      read_edges += edges % partitions;
    }
    long bytes_to_read = edge_unit_size * read_edges;
    long read_offset = edge_unit_size * (edges / partitions * partition_id);
    long read_bytes;
    int fin = open(path.c_str(), O_RDONLY);
    EdgeUnit<EdgeData> * read_edge_buffer = new EdgeUnit<EdgeData> [CHUNKSIZE];

    out_degree = alloc_interleaved_vertex_array<VertexId>();
    for (VertexId v_i=0;v_i<vertices;v_i++) {
      out_degree[v_i] = 0;
    }
    assert(lseek(fin, read_offset, SEEK_SET)==read_offset);
    read_bytes = 0;
    while (read_bytes < bytes_to_read) {
      long curr_read_bytes;
      if (bytes_to_read - read_bytes > edge_unit_size * CHUNKSIZE) {
        curr_read_bytes = read(fin, read_edge_buffer, edge_unit_size * CHUNKSIZE);
      } else {
        curr_read_bytes = read(fin, read_edge_buffer, bytes_to_read - read_bytes);
      }
      assert(curr_read_bytes>=0);
      read_bytes += curr_read_bytes;
      EdgeId curr_read_edges = curr_read_bytes / edge_unit_size;
      // #pragma omp parallel for
      for (EdgeId e_i=0;e_i<curr_read_edges;e_i++) {
        VertexId src = read_edge_buffer[e_i].src;
        VertexId dst = read_edge_buffer[e_i].dst;
        __sync_fetch_and_add(&out_degree[src], 1);
        __sync_fetch_and_add(&out_degree[dst], 1);
      }
    }
    MPI_Allreduce(MPI_IN_PLACE, out_degree, vertices, vid_t, MPI_SUM, MPI_COMM_WORLD);

    // locality-aware chunking
    partition_offset = new VertexId [partitions + 1];
    partition_offset[0] = 0;
    EdgeId remained_amount = edges * 2 + EdgeId(vertices) * alpha;
    for (int i=0;i<partitions;i++) {
      VertexId remained_partitions = partitions - i;
      EdgeId expected_chunk_size = remained_amount / remained_partitions;
      if (remained_partitions==1) {
        partition_offset[i+1] = vertices;
      } else {
        EdgeId got_edges = 0;
        for (VertexId v_i=partition_offset[i];v_i<vertices;v_i++) {
          got_edges += out_degree[v_i] + alpha;
          if (got_edges > expected_chunk_size) {
            partition_offset[i+1] = v_i;
            break;
          }
        }
        partition_offset[i+1] = (partition_offset[i+1]) / PAGESIZE * PAGESIZE; // aligned with pages
      }
      for (VertexId v_i=partition_offset[i];v_i<partition_offset[i+1];v_i++) {
        remained_amount -= out_degree[v_i] + alpha;
      }
    }
    assert(partition_offset[partitions]==vertices);
    owned_vertices = partition_offset[partition_id+1] - partition_offset[partition_id];
    // check consistency of partition boundaries
    VertexId * global_partition_offset = new VertexId [partitions + 1];
    MPI_Allreduce(partition_offset, global_partition_offset, partitions + 1, vid_t, MPI_MAX, MPI_COMM_WORLD);
    for (int i=0;i<=partitions;i++) {
      assert(partition_offset[i] == global_partition_offset[i]);
    }
    MPI_Allreduce(partition_offset, global_partition_offset, partitions + 1, vid_t, MPI_MIN, MPI_COMM_WORLD);
    for (int i=0;i<=partitions;i++) {
      assert(partition_offset[i] == global_partition_offset[i]);
    }
    #ifdef PRINT_DEBUG_MESSAGES
    if (partition_id==0) {
      for (int i=0;i<partitions;i++) {
        EdgeId part_out_edges = 0;
        for (VertexId v_i=partition_offset[i];v_i<partition_offset[i+1];v_i++) {
          part_out_edges += out_degree[v_i];
        }
        fprintf(stderr,"|V'_%d| = %u |E_%d| = %lu\n", i, partition_offset[i+1] - partition_offset[i], i, part_out_edges);
      }
    }
    MPI_Barrier(MPI_COMM_WORLD);
    #endif
    delete [] global_partition_offset;
    {
      // NUMA-aware sub-chunking
      local_partition_offset = new VertexId [sockets + 1];
      EdgeId part_out_edges = 0;
      for (VertexId v_i=partition_offset[partition_id];v_i<partition_offset[partition_id+1];v_i++) {
        part_out_edges += out_degree[v_i];
      }
      local_partition_offset[0] = partition_offset[partition_id];
      EdgeId remained_amount = part_out_edges + EdgeId(owned_vertices) * alpha;
      for (int s_i=0;s_i<sockets;s_i++) {
        VertexId remained_partitions = sockets - s_i;
        EdgeId expected_chunk_size = remained_amount / remained_partitions;
        if (remained_partitions==1) {
          local_partition_offset[s_i+1] = partition_offset[partition_id+1];
        } else {
          EdgeId got_edges = 0;
          for (VertexId v_i=local_partition_offset[s_i];v_i<partition_offset[partition_id+1];v_i++) {
            got_edges += out_degree[v_i] + alpha;
            if (got_edges > expected_chunk_size) {
              local_partition_offset[s_i+1] = v_i;
              break;
            }
          }
          local_partition_offset[s_i+1] = (local_partition_offset[s_i+1]) / PAGESIZE * PAGESIZE; // aligned with pages
        }
        EdgeId sub_part_out_edges = 0;
        for (VertexId v_i=local_partition_offset[s_i];v_i<local_partition_offset[s_i+1];v_i++) {
          remained_amount -= out_degree[v_i] + alpha;
          sub_part_out_edges += out_degree[v_i];
        }
        #ifdef PRINT_DEBUG_MESSAGES
        fprintf(stderr,"|V'_%d_%d| = %u |E_%d| = %lu\n", partition_id, s_i, local_partition_offset[s_i+1] - local_partition_offset[s_i], partition_id, sub_part_out_edges);
        #endif
      }
    }

    in_degree = out_degree;

    int * buffered_edges = new int [partitions];
    std::vector<char> * send_buffer = new std::vector<char> [partitions];
    for (int i=0;i<partitions;i++) {
      send_buffer[i].resize(edge_unit_size * CHUNKSIZE);
    }
    EdgeUnit<EdgeData> * recv_buffer = new EdgeUnit<EdgeData> [CHUNKSIZE];

    // constructing symmetric edges
    EdgeId recv_outgoing_edges = 0;
    outgoing_edges = new EdgeId [sockets];
    outgoing_adj_index = new EdgeId* [sockets];
    outgoing_adj_list = new AdjUnit<EdgeData>* [sockets];
    outgoing_adj_bitmap = new Bitmap * [sockets];
    outgoing_adj_index_data_win = new MPI_Win** [sockets];
    outgoing_adj_bitmap_data_win = new MPI_Win** [sockets];
    outgoing_adj_list_data_win = new MPI_Win** [sockets];

    for (int s_i=0;s_i<sockets;s_i++) {
      outgoing_adj_bitmap[s_i] = new Bitmap (vertices);
      outgoing_adj_bitmap[s_i]->clear();
      outgoing_adj_index[s_i] = (EdgeId*)numa_alloc_onnode(sizeof(EdgeId) * (vertices+1), s_i);
    }
    if (partition_id >= FM::n_compute_partitions) {
      for (int s_i=0; s_i<sockets; s_i++) {
        outgoing_adj_bitmap_data_win[s_i] = new MPI_Win* [threads];
        outgoing_adj_index_data_win[s_i] = new MPI_Win* [threads];
        for (int t_i=0; t_i<threads; t_i++) {
          outgoing_adj_bitmap_data_win[s_i][t_i] = new MPI_Win;
          MPI_Win_create(outgoing_adj_bitmap[s_i]->data, (WORD_OFFSET(vertices)+1)*sizeof(unsigned long), sizeof(unsigned long), MPI_INFO_NULL, MPI_COMM_WORLD, outgoing_adj_bitmap_data_win[s_i][t_i]);
          outgoing_adj_index_data_win[s_i][t_i] = new MPI_Win;
          MPI_Win_create(outgoing_adj_index[s_i], (vertices+1)*sizeof(EdgeId), sizeof(EdgeId), MPI_INFO_NULL, MPI_COMM_WORLD, outgoing_adj_index_data_win[s_i][t_i]);
        }
      }
    } else {
      for (int s_i=0; s_i<sockets; s_i++) {
        outgoing_adj_bitmap_data_win[s_i] = new MPI_Win* [threads];
        outgoing_adj_index_data_win[s_i] = new MPI_Win* [threads];
        for (int t_i=0; t_i<threads; t_i++) {
          outgoing_adj_bitmap_data_win[s_i][t_i] = new MPI_Win;
          MPI_Win_create(NULL, 0, 1, MPI_INFO_NULL, MPI_COMM_WORLD, outgoing_adj_bitmap_data_win[s_i][t_i]);
          outgoing_adj_index_data_win[s_i][t_i] = new MPI_Win;
          MPI_Win_create(NULL, 0, 1, MPI_INFO_NULL, MPI_COMM_WORLD, outgoing_adj_index_data_win[s_i][t_i]);        
        }
      }
    }
    {
      std::thread recv_thread_dst([&](){
        int finished_count = 0;
        MPI_Status recv_status;
        while (finished_count < partitions) {
          MPI_Probe(MPI_ANY_SOURCE, ShuffleGraph, MPI_COMM_WORLD, &recv_status);
          int i = recv_status.MPI_SOURCE;
          assert(recv_status.MPI_TAG == ShuffleGraph && i >=0 && i < partitions);
          int recv_bytes;
          MPI_Get_count(&recv_status, MPI_CHAR, &recv_bytes);
          if (recv_bytes==1) {
            finished_count += 1;
            char c;
            MPI_Recv(&c, 1, MPI_CHAR, i, ShuffleGraph, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
            continue;
          }
          assert(recv_bytes % edge_unit_size == 0);
          int recv_edges = recv_bytes / edge_unit_size;
          MPI_Recv(recv_buffer, edge_unit_size * recv_edges, MPI_CHAR, i, ShuffleGraph, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
          // #pragma omp parallel for
          for (EdgeId e_i=0;e_i<recv_edges;e_i++) {
            VertexId src = recv_buffer[e_i].src;
            VertexId dst = recv_buffer[e_i].dst;
            assert(dst >= partition_offset[partition_id] && dst < partition_offset[partition_id+1]);
            int dst_socket = get_local_partition_id(dst);
            if (!outgoing_adj_bitmap[dst_socket]->get_bit(src)) {
              outgoing_adj_bitmap[dst_socket]->set_bit(src);
              outgoing_adj_index[dst_socket][src] = 0;
            }
            __sync_fetch_and_add(&outgoing_adj_index[dst_socket][src], 1);
          }
          recv_outgoing_edges += recv_edges;
        }
      });
      for (int i=0;i<partitions;i++) {
        buffered_edges[i] = 0;
      }
      assert(lseek(fin, read_offset, SEEK_SET)==read_offset);
      read_bytes = 0;
      while (read_bytes < bytes_to_read) {
        long curr_read_bytes;
        if (bytes_to_read - read_bytes > edge_unit_size * CHUNKSIZE) {
          curr_read_bytes = read(fin, read_edge_buffer, edge_unit_size * CHUNKSIZE);
        } else {
          curr_read_bytes = read(fin, read_edge_buffer, bytes_to_read - read_bytes);
        }
        assert(curr_read_bytes>=0);
        read_bytes += curr_read_bytes;
        EdgeId curr_read_edges = curr_read_bytes / edge_unit_size;
        for (EdgeId e_i=0;e_i<curr_read_edges;e_i++) {
          VertexId dst = read_edge_buffer[e_i].dst;
          int i = get_partition_id(dst);
          memcpy(send_buffer[i].data() + edge_unit_size * buffered_edges[i], &read_edge_buffer[e_i], edge_unit_size);
          buffered_edges[i] += 1;
          if (buffered_edges[i] == CHUNKSIZE) {
            MPI_Send(send_buffer[i].data(), edge_unit_size * buffered_edges[i], MPI_CHAR, i, ShuffleGraph, MPI_COMM_WORLD);
            buffered_edges[i] = 0;
          }
        }
        for (EdgeId e_i=0;e_i<curr_read_edges;e_i++) {
          // std::swap(read_edge_buffer[e_i].src, read_edge_buffer[e_i].dst);
          VertexId tmp = read_edge_buffer[e_i].src;
          read_edge_buffer[e_i].src = read_edge_buffer[e_i].dst;
          read_edge_buffer[e_i].dst = tmp;
        }
        for (EdgeId e_i=0;e_i<curr_read_edges;e_i++) {
          VertexId dst = read_edge_buffer[e_i].dst;
          int i = get_partition_id(dst);
          memcpy(send_buffer[i].data() + edge_unit_size * buffered_edges[i], &read_edge_buffer[e_i], edge_unit_size);
          buffered_edges[i] += 1;
          if (buffered_edges[i] == CHUNKSIZE) {
            MPI_Send(send_buffer[i].data(), edge_unit_size * buffered_edges[i], MPI_CHAR, i, ShuffleGraph, MPI_COMM_WORLD);
            buffered_edges[i] = 0;
          }
        }
      }
      for (int i=0;i<partitions;i++) {
        if (buffered_edges[i]==0) continue;
        MPI_Send(send_buffer[i].data(), edge_unit_size * buffered_edges[i], MPI_CHAR, i, ShuffleGraph, MPI_COMM_WORLD);
        buffered_edges[i] = 0;
      }
      for (int i=0;i<partitions;i++) {
        char c = 0;
        MPI_Send(&c, 1, MPI_CHAR, i, ShuffleGraph, MPI_COMM_WORLD);
      }
      recv_thread_dst.join();
      #ifdef PRINT_DEBUG_MESSAGES
      fprintf(stderr,"machine(%d) got %lu symmetric edges\n", partition_id, recv_outgoing_edges);
      #endif
    }
    compressed_outgoing_adj_vertices = new VertexId [sockets];
    compressed_outgoing_adj_index = new CompressedAdjIndexUnit * [sockets];
    for (int s_i=0;s_i<sockets;s_i++) {
      outgoing_edges[s_i] = 0;
      compressed_outgoing_adj_vertices[s_i] = 0;
      for (VertexId v_i=0;v_i<vertices;v_i++) {
        if (outgoing_adj_bitmap[s_i]->get_bit(v_i)) {
          outgoing_edges[s_i] += outgoing_adj_index[s_i][v_i];
          compressed_outgoing_adj_vertices[s_i] += 1;
        }
      }
      compressed_outgoing_adj_index[s_i] = (CompressedAdjIndexUnit*)numa_alloc_onnode( sizeof(CompressedAdjIndexUnit) * (compressed_outgoing_adj_vertices[s_i] + 1) , s_i );
      compressed_outgoing_adj_index[s_i][0].index = 0;
      EdgeId last_e_i = 0;
      compressed_outgoing_adj_vertices[s_i] = 0;
      for (VertexId v_i=0;v_i<vertices;v_i++) {
        if (outgoing_adj_bitmap[s_i]->get_bit(v_i)) {
          outgoing_adj_index[s_i][v_i] = last_e_i + outgoing_adj_index[s_i][v_i];
          last_e_i = outgoing_adj_index[s_i][v_i];
          compressed_outgoing_adj_index[s_i][compressed_outgoing_adj_vertices[s_i]].vertex = v_i;
          compressed_outgoing_adj_vertices[s_i] += 1;
          compressed_outgoing_adj_index[s_i][compressed_outgoing_adj_vertices[s_i]].index = last_e_i;
        }
      }
      for (VertexId p_v_i=0;p_v_i<compressed_outgoing_adj_vertices[s_i];p_v_i++) {
        VertexId v_i = compressed_outgoing_adj_index[s_i][p_v_i].vertex;
        outgoing_adj_index[s_i][v_i] = compressed_outgoing_adj_index[s_i][p_v_i].index;
        outgoing_adj_index[s_i][v_i+1] = compressed_outgoing_adj_index[s_i][p_v_i+1].index;
      }
      #ifdef PRINT_DEBUG_MESSAGES
      fprintf(stderr,"part(%d) E_%d has %lu symmetric edges\n", partition_id, s_i, outgoing_edges[s_i]);
      #endif
      outgoing_adj_list[s_i] = (AdjUnit<EdgeData>*)numa_alloc_onnode(unit_size * outgoing_edges[s_i], s_i);
      outgoing_adj_list_data_win[s_i] = new MPI_Win* [threads];
      for (int t_i=0; t_i<threads; t_i++) {
        outgoing_adj_list_data_win[s_i][t_i] = new MPI_Win;
        if (partition_id >= FM::n_compute_partitions) {
          MPI_Win_create(outgoing_adj_list[s_i], outgoing_edges[s_i]*unit_size, unit_size, MPI_INFO_NULL, MPI_COMM_WORLD, outgoing_adj_list_data_win[s_i][t_i]);
        } else {
          MPI_Win_create(NULL, 0, 1, MPI_INFO_NULL, MPI_COMM_WORLD, outgoing_adj_list_data_win[s_i][t_i]);                
        }
      }
    }
    {
      std::thread recv_thread_dst([&](){
        int finished_count = 0;
        MPI_Status recv_status;
        while (finished_count < partitions) {
          MPI_Probe(MPI_ANY_SOURCE, ShuffleGraph, MPI_COMM_WORLD, &recv_status);
          int i = recv_status.MPI_SOURCE;
          assert(recv_status.MPI_TAG == ShuffleGraph && i >=0 && i < partitions);
          int recv_bytes;
          MPI_Get_count(&recv_status, MPI_CHAR, &recv_bytes);
          if (recv_bytes==1) {
            finished_count += 1;
            char c;
            MPI_Recv(&c, 1, MPI_CHAR, i, ShuffleGraph, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
            continue;
          }
          assert(recv_bytes % edge_unit_size == 0);
          int recv_edges = recv_bytes / edge_unit_size;
          MPI_Recv(recv_buffer, edge_unit_size * recv_edges, MPI_CHAR, i, ShuffleGraph, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
          #pragma omp parallel for
          for (EdgeId e_i=0;e_i<recv_edges;e_i++) {
            VertexId src = recv_buffer[e_i].src;
            VertexId dst = recv_buffer[e_i].dst;
            assert(dst >= partition_offset[partition_id] && dst < partition_offset[partition_id+1]);
            int dst_socket = get_local_partition_id(dst);
            EdgeId pos = __sync_fetch_and_add(&outgoing_adj_index[dst_socket][src], 1);
            outgoing_adj_list[dst_socket][pos].neighbour = dst;
            if (!std::is_same<EdgeData, Empty>::value) {
              outgoing_adj_list[dst_socket][pos].edge_data = recv_buffer[e_i].edge_data;
            }
          }
        }
      });
      for (int i=0;i<partitions;i++) {
        buffered_edges[i] = 0;
      }
      assert(lseek(fin, read_offset, SEEK_SET)==read_offset);
      read_bytes = 0;
      while (read_bytes < bytes_to_read) {
        long curr_read_bytes;
        if (bytes_to_read - read_bytes > edge_unit_size * CHUNKSIZE) {
          curr_read_bytes = read(fin, read_edge_buffer, edge_unit_size * CHUNKSIZE);
        } else {
          curr_read_bytes = read(fin, read_edge_buffer, bytes_to_read - read_bytes);
        }
        assert(curr_read_bytes>=0);
        read_bytes += curr_read_bytes;
        EdgeId curr_read_edges = curr_read_bytes / edge_unit_size;
        for (EdgeId e_i=0;e_i<curr_read_edges;e_i++) {
          VertexId dst = read_edge_buffer[e_i].dst;
          int i = get_partition_id(dst);
          memcpy(send_buffer[i].data() + edge_unit_size * buffered_edges[i], &read_edge_buffer[e_i], edge_unit_size);
          buffered_edges[i] += 1;
          if (buffered_edges[i] == CHUNKSIZE) {
            MPI_Send(send_buffer[i].data(), edge_unit_size * buffered_edges[i], MPI_CHAR, i, ShuffleGraph, MPI_COMM_WORLD);
            buffered_edges[i] = 0;
          }
        }
        for (EdgeId e_i=0;e_i<curr_read_edges;e_i++) {
          // std::swap(read_edge_buffer[e_i].src, read_edge_buffer[e_i].dst);
          VertexId tmp = read_edge_buffer[e_i].src;
          read_edge_buffer[e_i].src = read_edge_buffer[e_i].dst;
          read_edge_buffer[e_i].dst = tmp;
        }
        for (EdgeId e_i=0;e_i<curr_read_edges;e_i++) {
          VertexId dst = read_edge_buffer[e_i].dst;
          int i = get_partition_id(dst);
          memcpy(send_buffer[i].data() + edge_unit_size * buffered_edges[i], &read_edge_buffer[e_i], edge_unit_size);
          buffered_edges[i] += 1;
          if (buffered_edges[i] == CHUNKSIZE) {
            MPI_Send(send_buffer[i].data(), edge_unit_size * buffered_edges[i], MPI_CHAR, i, ShuffleGraph, MPI_COMM_WORLD);
            buffered_edges[i] = 0;
          }
        }
      }
      for (int i=0;i<partitions;i++) {
        if (buffered_edges[i]==0) continue;
        MPI_Send(send_buffer[i].data(), edge_unit_size * buffered_edges[i], MPI_CHAR, i, ShuffleGraph, MPI_COMM_WORLD);
        buffered_edges[i] = 0;
      }
      for (int i=0;i<partitions;i++) {
        char c = 0;
        MPI_Send(&c, 1, MPI_CHAR, i, ShuffleGraph, MPI_COMM_WORLD);
      }
      recv_thread_dst.join();
    }
    for (int s_i=0;s_i<sockets;s_i++) {
      for (VertexId p_v_i=0;p_v_i<compressed_outgoing_adj_vertices[s_i];p_v_i++) {
        VertexId v_i = compressed_outgoing_adj_index[s_i][p_v_i].vertex;
        outgoing_adj_index[s_i][v_i] = compressed_outgoing_adj_index[s_i][p_v_i].index;
        outgoing_adj_index[s_i][v_i+1] = compressed_outgoing_adj_index[s_i][p_v_i+1].index;
      }
    }
    MPI_Barrier(MPI_COMM_WORLD);

    incoming_edges = outgoing_edges;
    incoming_adj_index = outgoing_adj_index;
    incoming_adj_list = outgoing_adj_list;
    incoming_adj_bitmap = outgoing_adj_bitmap;
    compressed_incoming_adj_vertices = compressed_outgoing_adj_vertices;
    compressed_incoming_adj_index = compressed_outgoing_adj_index;
    MPI_Barrier(MPI_COMM_WORLD);

    delete [] buffered_edges;
    delete [] send_buffer;
    delete [] read_edge_buffer;
    delete [] recv_buffer;
    close(fin);

    tune_chunks();
    tuned_chunks_sparse = tuned_chunks_dense;

    prep_time += MPI_Wtime();

    #ifdef PRINT_DEBUG_MESSAGES
    if (partition_id==0) {
      fprintf(stderr,"preprocessing cost: %.2lf (s)\n", prep_time);
    }
    #endif
  }

  // transpose the graph
  void transpose() {
    std::swap(out_degree, in_degree);
    std::swap(outgoing_edges, incoming_edges);

    std::swap(outgoing_adj_index, incoming_adj_index);
    std::swap(outgoing_adj_index_data_win, incoming_adj_index_data_win);
#if ENABLE_INDEX_CACHE == 1
    std::swap(outgoing_adj_index_cache, incoming_adj_index_cache);
    std::swap(outgoing_adj_index_cache_pool, incoming_adj_index_cache_pool);
    std::swap(FM::outgoing_adj_index_cache_hit, FM::incoming_adj_index_cache_hit);
    std::swap(FM::outgoing_adj_index_cache_miss, FM::incoming_adj_index_cache_miss);
#endif

    std::swap(outgoing_adj_bitmap, incoming_adj_bitmap);
    std::swap(outgoing_adj_bitmap_data_win, incoming_adj_bitmap_data_win);
#if ENABLE_BITMAP_CACHE == 1
    std::swap(outgoing_adj_bitmap_cache, incoming_adj_bitmap_cache);
    std::swap(outgoing_adj_bitmap_cache_pool, incoming_adj_bitmap_cache_pool);
    std::swap(FM::outgoing_adj_bitmap_cache_hit, FM::incoming_adj_bitmap_cache_hit);
    std::swap(FM::outgoing_adj_bitmap_cache_miss, FM::incoming_adj_bitmap_cache_miss);
#endif

    std::swap(outgoing_adj_list, incoming_adj_list);
    std::swap(outgoing_adj_list_data_win, incoming_adj_list_data_win);
#if ENABLE_EDGE_CACHE == 1
    std::swap(outgoing_edge_cache, incoming_edge_cache);
    std::swap(outgoing_edge_cache_pool, incoming_edge_cache_pool);
    std::swap(FM::outgoing_edge_cache_hit, FM::incoming_edge_cache_hit);
    std::swap(FM::outgoing_edge_cache_miss, FM::incoming_edge_cache_miss);
    std::swap(FM::outgoing_edge_cache_pool_count, FM::incoming_edge_cache_pool_count);
#endif

    std::swap(tuned_chunks_dense, tuned_chunks_sparse);
    std::swap(compressed_outgoing_adj_vertices, compressed_incoming_adj_vertices);
    std::swap(compressed_outgoing_adj_index, compressed_incoming_adj_index);
  }

  // load a directed graph from path
  void load_directed(std::string path, VertexId vertices) {
    double prep_time = 0;
    prep_time -= MPI_Wtime();
    #ifdef PRINT_DEBUG_MESSAGES
    if (partition_id==0) {
      fprintf(stderr, "load directed graph...");
    }
    #endif
    symmetric = false;

    MPI_Datatype vid_t = get_mpi_data_type<VertexId>();

    this->vertices = vertices;
    long total_bytes = file_size(path.c_str());
    this->edges = total_bytes / edge_unit_size;
    #ifdef PRINT_DEBUG_MESSAGES
    if (partition_id==0) {
      fprintf(stderr,"|V| = %u, |E| = %lu\n", vertices, edges);
    }
    #endif
    MPI_Barrier(MPI_COMM_WORLD);
    EdgeId read_edges = edges / partitions;
    if (partition_id==partitions-1) {
      read_edges += edges % partitions;
    }
    long bytes_to_read = edge_unit_size * read_edges;
    long read_offset = edge_unit_size * (edges / partitions * partition_id);
    long read_bytes;
    int fin = open(path.c_str(), O_RDONLY);
    EdgeUnit<EdgeData> * read_edge_buffer = new EdgeUnit<EdgeData> [CHUNKSIZE];

    out_degree = alloc_interleaved_vertex_array<VertexId>();
    for (VertexId v_i=0;v_i<vertices;v_i++) {
      out_degree[v_i] = 0;
    }
    in_degree = alloc_interleaved_vertex_array<VertexId>();
    for (VertexId v_i=0;v_i<vertices;v_i++) {
      in_degree[v_i] = 0;
    }
    assert(lseek(fin, read_offset, SEEK_SET)==read_offset);
    read_bytes = 0;
    while (read_bytes < bytes_to_read) {
      long curr_read_bytes;
      if (bytes_to_read - read_bytes > edge_unit_size * CHUNKSIZE) {
        curr_read_bytes = read(fin, read_edge_buffer, edge_unit_size * CHUNKSIZE);
      } else {
        curr_read_bytes = read(fin, read_edge_buffer, bytes_to_read - read_bytes);
      }
      assert(curr_read_bytes>=0);
      read_bytes += curr_read_bytes;
      EdgeId curr_read_edges = curr_read_bytes / edge_unit_size;
      #pragma omp parallel for
      for (EdgeId e_i=0;e_i<curr_read_edges;e_i++) {
        VertexId src = read_edge_buffer[e_i].src;
        VertexId dst = read_edge_buffer[e_i].dst;
        __sync_fetch_and_add(&out_degree[src], 1);
        __sync_fetch_and_add(&in_degree[dst], 1);
      }
    }
    MPI_Allreduce(MPI_IN_PLACE, out_degree, vertices, vid_t, MPI_SUM, MPI_COMM_WORLD);
    MPI_Allreduce(MPI_IN_PLACE, in_degree, vertices, vid_t, MPI_SUM, MPI_COMM_WORLD);

    // locality-aware chunking
    partition_offset = new VertexId [partitions + 1];
    partition_offset[0] = 0;
    EdgeId remained_amount = edges + EdgeId(vertices) * alpha;
    for (int i=0;i<partitions;i++) {
      VertexId remained_partitions = partitions - i;
      EdgeId expected_chunk_size = remained_amount / remained_partitions;
      if (remained_partitions==1) {
        partition_offset[i+1] = vertices;
      } else {
        EdgeId got_edges = 0;
        for (VertexId v_i=partition_offset[i];v_i<vertices;v_i++) {
          got_edges += out_degree[v_i] + alpha;
          if (got_edges > expected_chunk_size) {
            partition_offset[i+1] = v_i;
            break;
          }
        }
        partition_offset[i+1] = (partition_offset[i+1]) / PAGESIZE * PAGESIZE; // aligned with pages
      }
      for (VertexId v_i=partition_offset[i];v_i<partition_offset[i+1];v_i++) {
        remained_amount -= out_degree[v_i] + alpha;
      }
    }
    assert(partition_offset[partitions]==vertices);
    owned_vertices = partition_offset[partition_id+1] - partition_offset[partition_id];
    // check consistency of partition boundaries
    VertexId * global_partition_offset = new VertexId [partitions + 1];
    MPI_Allreduce(partition_offset, global_partition_offset, partitions + 1, vid_t, MPI_MAX, MPI_COMM_WORLD);
    for (int i=0;i<=partitions;i++) {
      assert(partition_offset[i] == global_partition_offset[i]);
    }
    MPI_Allreduce(partition_offset, global_partition_offset, partitions + 1, vid_t, MPI_MIN, MPI_COMM_WORLD);
    for (int i=0;i<=partitions;i++) {
      assert(partition_offset[i] == global_partition_offset[i]);
    }
    #ifdef PRINT_DEBUG_MESSAGES
    if (partition_id==0) {
      for (int i=0;i<partitions;i++) {
        EdgeId part_out_edges = 0;
        for (VertexId v_i=partition_offset[i];v_i<partition_offset[i+1];v_i++) {
          part_out_edges += out_degree[v_i];
        }
        fprintf(stderr,"|V'_%d| = %u |E^dense_%d| = %lu\n", i, partition_offset[i+1] - partition_offset[i], i, part_out_edges);
      }
    }
    #endif
    delete [] global_partition_offset;
    {
      // NUMA-aware sub-chunking
      local_partition_offset = new VertexId [sockets + 1];
      EdgeId part_out_edges = 0;
      for (VertexId v_i=partition_offset[partition_id];v_i<partition_offset[partition_id+1];v_i++) {
        part_out_edges += out_degree[v_i];
      }
      local_partition_offset[0] = partition_offset[partition_id];
      EdgeId remained_amount = part_out_edges + EdgeId(owned_vertices) * alpha;
      for (int s_i=0;s_i<sockets;s_i++) {
        VertexId remained_partitions = sockets - s_i;
        EdgeId expected_chunk_size = remained_amount / remained_partitions;
        if (remained_partitions==1) {
          local_partition_offset[s_i+1] = partition_offset[partition_id+1];
        } else {
          EdgeId got_edges = 0;
          for (VertexId v_i=local_partition_offset[s_i];v_i<partition_offset[partition_id+1];v_i++) {
            got_edges += out_degree[v_i] + alpha;
            if (got_edges > expected_chunk_size) {
              local_partition_offset[s_i+1] = v_i;
              break;
            }
          }
          local_partition_offset[s_i+1] = (local_partition_offset[s_i+1]) / PAGESIZE * PAGESIZE; // aligned with pages
        }
        EdgeId sub_part_out_edges = 0;
        for (VertexId v_i=local_partition_offset[s_i];v_i<local_partition_offset[s_i+1];v_i++) {
          remained_amount -= out_degree[v_i] + alpha;
          sub_part_out_edges += out_degree[v_i];
        }
        #ifdef PRINT_DEBUG_MESSAGES
        fprintf(stderr,"|V'_%d_%d| = %u |E^dense_%d_%d| = %lu\n", partition_id, s_i, local_partition_offset[s_i+1] - local_partition_offset[s_i], partition_id, s_i, sub_part_out_edges);
        #endif
      }
    }

    int * buffered_edges = new int [partitions];
    std::vector<char> * send_buffer = new std::vector<char> [partitions];
    for (int i=0;i<partitions;i++) {
      send_buffer[i].resize(edge_unit_size * CHUNKSIZE);
    }
    EdgeUnit<EdgeData> * recv_buffer = new EdgeUnit<EdgeData> [CHUNKSIZE];

    EdgeId recv_outgoing_edges = 0;
    outgoing_edges = new EdgeId [sockets];
    outgoing_adj_index = new EdgeId* [sockets];
    outgoing_adj_list = new AdjUnit<EdgeData>* [sockets];
    outgoing_adj_bitmap = new Bitmap * [sockets];
    outgoing_adj_index_data_win = new MPI_Win** [sockets];
    outgoing_adj_bitmap_data_win = new MPI_Win** [sockets];
    outgoing_adj_list_data_win = new MPI_Win** [sockets];

    for (int s_i=0;s_i<sockets;s_i++) {
      outgoing_adj_bitmap[s_i] = new Bitmap (vertices);
      outgoing_adj_bitmap[s_i]->clear();
      outgoing_adj_index[s_i] = (EdgeId*)numa_alloc_onnode(sizeof(EdgeId) * (vertices+1), s_i);
    }
    if (partition_id >= FM::n_compute_partitions) {
      for (int s_i=0; s_i<sockets; s_i++) {
        outgoing_adj_bitmap_data_win[s_i] = new MPI_Win* [threads];
        outgoing_adj_index_data_win[s_i] = new MPI_Win* [threads];
        for (int t_i=0; t_i<threads; t_i++) {
          outgoing_adj_bitmap_data_win[s_i][t_i] = new MPI_Win;
          MPI_Win_create(outgoing_adj_bitmap[s_i]->data, (WORD_OFFSET(vertices)+1)*sizeof(unsigned long), sizeof(unsigned long), MPI_INFO_NULL, MPI_COMM_WORLD, outgoing_adj_bitmap_data_win[s_i][t_i]);
          outgoing_adj_index_data_win[s_i][t_i] = new MPI_Win;
          MPI_Win_create(outgoing_adj_index[s_i], (vertices+1)*sizeof(EdgeId), sizeof(EdgeId), MPI_INFO_NULL, MPI_COMM_WORLD, outgoing_adj_index_data_win[s_i][t_i]);
        }
      }
    } else {
      for (int s_i=0; s_i<sockets; s_i++) {
        outgoing_adj_bitmap_data_win[s_i] = new MPI_Win* [threads];
        outgoing_adj_index_data_win[s_i] = new MPI_Win* [threads];
        for (int t_i=0; t_i<threads; t_i++) {
          outgoing_adj_bitmap_data_win[s_i][t_i] = new MPI_Win;
          MPI_Win_create(NULL, 0, 1, MPI_INFO_NULL, MPI_COMM_WORLD, outgoing_adj_bitmap_data_win[s_i][t_i]);
          outgoing_adj_index_data_win[s_i][t_i] = new MPI_Win;
          MPI_Win_create(NULL, 0, 1, MPI_INFO_NULL, MPI_COMM_WORLD, outgoing_adj_index_data_win[s_i][t_i]);        
        }
      }
    }
    {
      std::thread recv_thread_dst([&](){
        int finished_count = 0;
        MPI_Status recv_status;
        while (finished_count < partitions) {
          MPI_Probe(MPI_ANY_SOURCE, ShuffleGraph, MPI_COMM_WORLD, &recv_status);
          int i = recv_status.MPI_SOURCE;
          assert(recv_status.MPI_TAG == ShuffleGraph && i >=0 && i < partitions);
          int recv_bytes;
          MPI_Get_count(&recv_status, MPI_CHAR, &recv_bytes);
          if (recv_bytes==1) {
            finished_count += 1;
            char c;
            MPI_Recv(&c, 1, MPI_CHAR, i, ShuffleGraph, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
            continue;
          }
          assert(recv_bytes % edge_unit_size == 0);
          int recv_edges = recv_bytes / edge_unit_size;
          MPI_Recv(recv_buffer, edge_unit_size * recv_edges, MPI_CHAR, i, ShuffleGraph, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
          // #pragma omp parallel for
          for (EdgeId e_i=0;e_i<recv_edges;e_i++) {
            VertexId src = recv_buffer[e_i].src;
            VertexId dst = recv_buffer[e_i].dst;
            assert(dst >= partition_offset[partition_id] && dst < partition_offset[partition_id+1]);
            int dst_socket = get_local_partition_id(dst);
            if (!outgoing_adj_bitmap[dst_socket]->get_bit(src)) {
              outgoing_adj_bitmap[dst_socket]->set_bit(src);
              outgoing_adj_index[dst_socket][src] = 0;
            }
            __sync_fetch_and_add(&outgoing_adj_index[dst_socket][src], 1);
          }
          recv_outgoing_edges += recv_edges;
        }
      });
      for (int i=0;i<partitions;i++) {
        buffered_edges[i] = 0;
      }
      assert(lseek(fin, read_offset, SEEK_SET)==read_offset);
      read_bytes = 0;
      while (read_bytes < bytes_to_read) {
        long curr_read_bytes;
        if (bytes_to_read - read_bytes > edge_unit_size * CHUNKSIZE) {
          curr_read_bytes = read(fin, read_edge_buffer, edge_unit_size * CHUNKSIZE);
        } else {
          curr_read_bytes = read(fin, read_edge_buffer, bytes_to_read - read_bytes);
        }
        assert(curr_read_bytes>=0);
        read_bytes += curr_read_bytes;
        EdgeId curr_read_edges = curr_read_bytes / edge_unit_size;
        for (EdgeId e_i=0;e_i<curr_read_edges;e_i++) {
          VertexId dst = read_edge_buffer[e_i].dst;
          int i = get_partition_id(dst);
          memcpy(send_buffer[i].data() + edge_unit_size * buffered_edges[i], &read_edge_buffer[e_i], edge_unit_size);
          buffered_edges[i] += 1;
          if (buffered_edges[i] == CHUNKSIZE) {
            MPI_Send(send_buffer[i].data(), edge_unit_size * buffered_edges[i], MPI_CHAR, i, ShuffleGraph, MPI_COMM_WORLD);
            buffered_edges[i] = 0;
          }
        }
      }
      for (int i=0;i<partitions;i++) {
        if (buffered_edges[i]==0) continue;
        MPI_Send(send_buffer[i].data(), edge_unit_size * buffered_edges[i], MPI_CHAR, i, ShuffleGraph, MPI_COMM_WORLD);
        buffered_edges[i] = 0;
      }
      for (int i=0;i<partitions;i++) {
        char c = 0;
        MPI_Send(&c, 1, MPI_CHAR, i, ShuffleGraph, MPI_COMM_WORLD);
      }
      recv_thread_dst.join();
      #ifdef PRINT_DEBUG_MESSAGES
      fprintf(stderr,"machine(%d) got %lu sparse mode edges\n", partition_id, recv_outgoing_edges);
      #endif
    }
    compressed_outgoing_adj_vertices = new VertexId [sockets];
    compressed_outgoing_adj_index = new CompressedAdjIndexUnit * [sockets];
    for (int s_i=0;s_i<sockets;s_i++) {
      outgoing_edges[s_i] = 0;
      compressed_outgoing_adj_vertices[s_i] = 0;
      for (VertexId v_i=0;v_i<vertices;v_i++) {
        if (outgoing_adj_bitmap[s_i]->get_bit(v_i)) {
          outgoing_edges[s_i] += outgoing_adj_index[s_i][v_i];
          compressed_outgoing_adj_vertices[s_i] += 1;
        }
      }
      compressed_outgoing_adj_index[s_i] = (CompressedAdjIndexUnit*)numa_alloc_onnode( sizeof(CompressedAdjIndexUnit) * (compressed_outgoing_adj_vertices[s_i] + 1) , s_i );
      compressed_outgoing_adj_index[s_i][0].index = 0;
      EdgeId last_e_i = 0;
      compressed_outgoing_adj_vertices[s_i] = 0;
      for (VertexId v_i=0;v_i<vertices;v_i++) {
        if (outgoing_adj_bitmap[s_i]->get_bit(v_i)) {
          outgoing_adj_index[s_i][v_i] = last_e_i + outgoing_adj_index[s_i][v_i];
          last_e_i = outgoing_adj_index[s_i][v_i];
          compressed_outgoing_adj_index[s_i][compressed_outgoing_adj_vertices[s_i]].vertex = v_i;
          compressed_outgoing_adj_vertices[s_i] += 1;
          compressed_outgoing_adj_index[s_i][compressed_outgoing_adj_vertices[s_i]].index = last_e_i;
        }
      }
      for (VertexId p_v_i=0;p_v_i<compressed_outgoing_adj_vertices[s_i];p_v_i++) {
        VertexId v_i = compressed_outgoing_adj_index[s_i][p_v_i].vertex;
        outgoing_adj_index[s_i][v_i] = compressed_outgoing_adj_index[s_i][p_v_i].index;
        outgoing_adj_index[s_i][v_i+1] = compressed_outgoing_adj_index[s_i][p_v_i+1].index;
      }
      #ifdef PRINT_DEBUG_MESSAGES
      fprintf(stderr,"part(%d) E_%d has %lu sparse mode edges\n", partition_id, s_i, outgoing_edges[s_i]);
      #endif
      outgoing_adj_list[s_i] = (AdjUnit<EdgeData>*)numa_alloc_onnode(unit_size * outgoing_edges[s_i], s_i);
      outgoing_adj_list_data_win[s_i] = new MPI_Win* [threads];
      for (int t_i=0; t_i<threads; t_i++) {
        outgoing_adj_list_data_win[s_i][t_i] = new MPI_Win;
        if (partition_id >= FM::n_compute_partitions) {
          MPI_Win_create(outgoing_adj_list[s_i], outgoing_edges[s_i]*unit_size, unit_size, MPI_INFO_NULL, MPI_COMM_WORLD, outgoing_adj_list_data_win[s_i][t_i]);
        } else {
          MPI_Win_create(NULL, 0, 1, MPI_INFO_NULL, MPI_COMM_WORLD, outgoing_adj_list_data_win[s_i][t_i]);                
        }
      }
    }
    {
      std::thread recv_thread_dst([&](){
        int finished_count = 0;
        MPI_Status recv_status;
        while (finished_count < partitions) {
          MPI_Probe(MPI_ANY_SOURCE, ShuffleGraph, MPI_COMM_WORLD, &recv_status);
          int i = recv_status.MPI_SOURCE;
          assert(recv_status.MPI_TAG == ShuffleGraph && i >=0 && i < partitions);
          int recv_bytes;
          MPI_Get_count(&recv_status, MPI_CHAR, &recv_bytes);
          if (recv_bytes==1) {
            finished_count += 1;
            char c;
            MPI_Recv(&c, 1, MPI_CHAR, i, ShuffleGraph, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
            continue;
          }
          assert(recv_bytes % edge_unit_size == 0);
          int recv_edges = recv_bytes / edge_unit_size;
          MPI_Recv(recv_buffer, edge_unit_size * recv_edges, MPI_CHAR, i, ShuffleGraph, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
          #pragma omp parallel for
          for (EdgeId e_i=0;e_i<recv_edges;e_i++) {
            VertexId src = recv_buffer[e_i].src;
            VertexId dst = recv_buffer[e_i].dst;
            assert(dst >= partition_offset[partition_id] && dst < partition_offset[partition_id+1]);
            int dst_socket = get_local_partition_id(dst);
            EdgeId pos = __sync_fetch_and_add(&outgoing_adj_index[dst_socket][src], 1);
            outgoing_adj_list[dst_socket][pos].neighbour = dst;
            if (!std::is_same<EdgeData, Empty>::value) {
              outgoing_adj_list[dst_socket][pos].edge_data = recv_buffer[e_i].edge_data;
            }
          }
        }
      });
      for (int i=0;i<partitions;i++) {
        buffered_edges[i] = 0;
      }
      assert(lseek(fin, read_offset, SEEK_SET)==read_offset);
      read_bytes = 0;
      while (read_bytes < bytes_to_read) {
        long curr_read_bytes;
        if (bytes_to_read - read_bytes > edge_unit_size * CHUNKSIZE) {
          curr_read_bytes = read(fin, read_edge_buffer, edge_unit_size * CHUNKSIZE);
        } else {
          curr_read_bytes = read(fin, read_edge_buffer, bytes_to_read - read_bytes);
        }
        assert(curr_read_bytes>=0);
        read_bytes += curr_read_bytes;
        EdgeId curr_read_edges = curr_read_bytes / edge_unit_size;
        for (EdgeId e_i=0;e_i<curr_read_edges;e_i++) {
          VertexId dst = read_edge_buffer[e_i].dst;
          int i = get_partition_id(dst);
          memcpy(send_buffer[i].data() + edge_unit_size * buffered_edges[i], &read_edge_buffer[e_i], edge_unit_size);
          buffered_edges[i] += 1;
          if (buffered_edges[i] == CHUNKSIZE) {
            MPI_Send(send_buffer[i].data(), edge_unit_size * buffered_edges[i], MPI_CHAR, i, ShuffleGraph, MPI_COMM_WORLD);
            buffered_edges[i] = 0;
          }
        }
      }
      for (int i=0;i<partitions;i++) {
        if (buffered_edges[i]==0) continue;
        MPI_Send(send_buffer[i].data(), edge_unit_size * buffered_edges[i], MPI_CHAR, i, ShuffleGraph, MPI_COMM_WORLD);
        buffered_edges[i] = 0;
      }
      for (int i=0;i<partitions;i++) {
        char c = 0;
        MPI_Send(&c, 1, MPI_CHAR, i, ShuffleGraph, MPI_COMM_WORLD);
      }
      recv_thread_dst.join();
    }
    for (int s_i=0;s_i<sockets;s_i++) {
      for (VertexId p_v_i=0;p_v_i<compressed_outgoing_adj_vertices[s_i];p_v_i++) {
        VertexId v_i = compressed_outgoing_adj_index[s_i][p_v_i].vertex;
        outgoing_adj_index[s_i][v_i] = compressed_outgoing_adj_index[s_i][p_v_i].index;
        outgoing_adj_index[s_i][v_i+1] = compressed_outgoing_adj_index[s_i][p_v_i+1].index;
      }
    }
    MPI_Barrier(MPI_COMM_WORLD);

    // preparing for dense mode graph data
    EdgeId recv_incoming_edges = 0;
    incoming_edges = new EdgeId [sockets];
    incoming_adj_index = new EdgeId* [sockets];
    incoming_adj_list = new AdjUnit<EdgeData>* [sockets];
    incoming_adj_bitmap = new Bitmap * [sockets];
    incoming_adj_index_data_win = new MPI_Win** [sockets];
    incoming_adj_bitmap_data_win = new MPI_Win** [sockets];
    incoming_adj_list_data_win = new MPI_Win** [sockets];
    for (int s_i=0;s_i<sockets;s_i++) {
      incoming_adj_bitmap[s_i] = new Bitmap (vertices);
      incoming_adj_bitmap[s_i]->clear();
      incoming_adj_index[s_i] = (EdgeId*)numa_alloc_onnode(sizeof(EdgeId) * (vertices+1), s_i);
    }
    if (partition_id >= FM::n_compute_partitions) {
      for (int s_i=0; s_i<sockets; s_i++) {
        incoming_adj_bitmap_data_win[s_i] = new MPI_Win* [threads];
        incoming_adj_index_data_win[s_i] = new MPI_Win* [threads];
        for (int t_i=0; t_i<threads; t_i++) {
          incoming_adj_bitmap_data_win[s_i][t_i] = new MPI_Win;
          MPI_Win_create(incoming_adj_bitmap[s_i]->data, (WORD_OFFSET(vertices)+1)*sizeof(unsigned long), sizeof(unsigned long), MPI_INFO_NULL, MPI_COMM_WORLD, incoming_adj_bitmap_data_win[s_i][t_i]);
          incoming_adj_index_data_win[s_i][t_i] = new MPI_Win;
          MPI_Win_create(incoming_adj_index[s_i], (vertices+1)*sizeof(EdgeId), sizeof(EdgeId), MPI_INFO_NULL, MPI_COMM_WORLD, incoming_adj_index_data_win[s_i][t_i]);
        }
      }
    } else {
      for (int s_i=0; s_i<sockets; s_i++) {
        incoming_adj_bitmap_data_win[s_i] = new MPI_Win* [threads];
        incoming_adj_index_data_win[s_i] = new MPI_Win* [threads];
        for (int t_i=0; t_i<threads; t_i++) {
          incoming_adj_bitmap_data_win[s_i][t_i] = new MPI_Win;
          MPI_Win_create(NULL, 0, 1, MPI_INFO_NULL, MPI_COMM_WORLD, incoming_adj_bitmap_data_win[s_i][t_i]);
          incoming_adj_index_data_win[s_i][t_i] = new MPI_Win;
          MPI_Win_create(NULL, 0, 1, MPI_INFO_NULL, MPI_COMM_WORLD, incoming_adj_index_data_win[s_i][t_i]);
        }
      }
    }
    {
      std::thread recv_thread_src([&](){
        int finished_count = 0;
        MPI_Status recv_status;
        while (finished_count < partitions) {
          MPI_Probe(MPI_ANY_SOURCE, ShuffleGraph, MPI_COMM_WORLD, &recv_status);
          int i = recv_status.MPI_SOURCE;
          assert(recv_status.MPI_TAG == ShuffleGraph && i >=0 && i < partitions);
          int recv_bytes;
          MPI_Get_count(&recv_status, MPI_CHAR, &recv_bytes);
          if (recv_bytes==1) {
            finished_count += 1;
            char c;
            MPI_Recv(&c, 1, MPI_CHAR, i, ShuffleGraph, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
            continue;
          }
          assert(recv_bytes % edge_unit_size == 0);
          int recv_edges = recv_bytes / edge_unit_size;
          MPI_Recv(recv_buffer, edge_unit_size * recv_edges, MPI_CHAR, i, ShuffleGraph, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
          // #pragma omp parallel for
          for (EdgeId e_i=0;e_i<recv_edges;e_i++) {
            VertexId src = recv_buffer[e_i].src;
            VertexId dst = recv_buffer[e_i].dst;
            assert(src >= partition_offset[partition_id] && src < partition_offset[partition_id+1]);
            int src_socket = get_local_partition_id(src);
            if (!incoming_adj_bitmap[src_socket]->get_bit(dst)) {
              incoming_adj_bitmap[src_socket]->set_bit(dst);
              incoming_adj_index[src_socket][dst] = 0;
            }
            __sync_fetch_and_add(&incoming_adj_index[src_socket][dst], 1);
          }
          recv_incoming_edges += recv_edges;
        }
      });
      for (int i=0;i<partitions;i++) {
        buffered_edges[i] = 0;
      }
      assert(lseek(fin, read_offset, SEEK_SET)==read_offset);
      read_bytes = 0;
      while (read_bytes < bytes_to_read) {
        long curr_read_bytes;
        if (bytes_to_read - read_bytes > edge_unit_size * CHUNKSIZE) {
          curr_read_bytes = read(fin, read_edge_buffer, edge_unit_size * CHUNKSIZE);
        } else {
          curr_read_bytes = read(fin, read_edge_buffer, bytes_to_read - read_bytes);
        }
        assert(curr_read_bytes>=0);
        read_bytes += curr_read_bytes;
        EdgeId curr_read_edges = curr_read_bytes / edge_unit_size;
        for (EdgeId e_i=0;e_i<curr_read_edges;e_i++) {
          VertexId src = read_edge_buffer[e_i].src;
          int i = get_partition_id(src);
          memcpy(send_buffer[i].data() + edge_unit_size * buffered_edges[i], &read_edge_buffer[e_i], edge_unit_size);
          buffered_edges[i] += 1;
          if (buffered_edges[i] == CHUNKSIZE) {
            MPI_Send(send_buffer[i].data(), edge_unit_size * buffered_edges[i], MPI_CHAR, i, ShuffleGraph, MPI_COMM_WORLD);
            buffered_edges[i] = 0;
          }
        }
      }
      for (int i=0;i<partitions;i++) {
        if (buffered_edges[i]==0) continue;
        MPI_Send(send_buffer[i].data(), edge_unit_size * buffered_edges[i], MPI_CHAR, i, ShuffleGraph, MPI_COMM_WORLD);
        buffered_edges[i] = 0;
      }
      for (int i=0;i<partitions;i++) {
        char c = 0;
        MPI_Send(&c, 1, MPI_CHAR, i, ShuffleGraph, MPI_COMM_WORLD);
      }
      recv_thread_src.join();
      #ifdef PRINT_DEBUG_MESSAGES
      fprintf(stderr,"machine(%d) got %lu dense mode edges\n", partition_id, recv_incoming_edges);
      #endif
    }
    compressed_incoming_adj_vertices = new VertexId [sockets];
    compressed_incoming_adj_index = new CompressedAdjIndexUnit * [sockets];
    for (int s_i=0;s_i<sockets;s_i++) {
      incoming_edges[s_i] = 0;
      compressed_incoming_adj_vertices[s_i] = 0;
      for (VertexId v_i=0;v_i<vertices;v_i++) {
        if (incoming_adj_bitmap[s_i]->get_bit(v_i)) {
          incoming_edges[s_i] += incoming_adj_index[s_i][v_i];
          compressed_incoming_adj_vertices[s_i] += 1;
        }
      }
      compressed_incoming_adj_index[s_i] = (CompressedAdjIndexUnit*)numa_alloc_onnode( sizeof(CompressedAdjIndexUnit) * (compressed_incoming_adj_vertices[s_i] + 1) , s_i );
      compressed_incoming_adj_index[s_i][0].index = 0;
      EdgeId last_e_i = 0;
      compressed_incoming_adj_vertices[s_i] = 0;
      for (VertexId v_i=0;v_i<vertices;v_i++) {
        if (incoming_adj_bitmap[s_i]->get_bit(v_i)) {
          incoming_adj_index[s_i][v_i] = last_e_i + incoming_adj_index[s_i][v_i];
          last_e_i = incoming_adj_index[s_i][v_i];
          compressed_incoming_adj_index[s_i][compressed_incoming_adj_vertices[s_i]].vertex = v_i;
          compressed_incoming_adj_vertices[s_i] += 1;
          compressed_incoming_adj_index[s_i][compressed_incoming_adj_vertices[s_i]].index = last_e_i;
        }
      }
      for (VertexId p_v_i=0;p_v_i<compressed_incoming_adj_vertices[s_i];p_v_i++) {
        VertexId v_i = compressed_incoming_adj_index[s_i][p_v_i].vertex;
        incoming_adj_index[s_i][v_i] = compressed_incoming_adj_index[s_i][p_v_i].index;
        incoming_adj_index[s_i][v_i+1] = compressed_incoming_adj_index[s_i][p_v_i+1].index;
      }
      #ifdef PRINT_DEBUG_MESSAGES
      fprintf(stderr,"part(%d) E_%d has %lu dense mode edges\n", partition_id, s_i, incoming_edges[s_i]);
      #endif
      incoming_adj_list[s_i] = (AdjUnit<EdgeData>*)numa_alloc_onnode(unit_size * incoming_edges[s_i], s_i);
      incoming_adj_list_data_win[s_i] = new MPI_Win* [threads];
      for (int t_i=0; t_i<threads; t_i++) {
        incoming_adj_list_data_win[s_i][t_i] = new MPI_Win;
        if (partition_id >= FM::n_compute_partitions) {
          MPI_Win_create(incoming_adj_list[s_i], incoming_edges[s_i]*unit_size, unit_size, MPI_INFO_NULL, MPI_COMM_WORLD, incoming_adj_list_data_win[s_i][t_i]);
        } else {
          MPI_Win_create(NULL, 0, 1, MPI_INFO_NULL, MPI_COMM_WORLD, incoming_adj_list_data_win[s_i][t_i]);
        }
      }
    }
    {
      std::thread recv_thread_src([&](){
        int finished_count = 0;
        MPI_Status recv_status;
        while (finished_count < partitions) {
          MPI_Probe(MPI_ANY_SOURCE, ShuffleGraph, MPI_COMM_WORLD, &recv_status);
          int i = recv_status.MPI_SOURCE;
          assert(recv_status.MPI_TAG == ShuffleGraph && i >=0 && i < partitions);
          int recv_bytes;
          MPI_Get_count(&recv_status, MPI_CHAR, &recv_bytes);
          if (recv_bytes==1) {
            finished_count += 1;
            char c;
            MPI_Recv(&c, 1, MPI_CHAR, i, ShuffleGraph, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
            continue;
          }
          assert(recv_bytes % edge_unit_size == 0);
          int recv_edges = recv_bytes / edge_unit_size;
          MPI_Recv(recv_buffer, edge_unit_size * recv_edges, MPI_CHAR, i, ShuffleGraph, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
          #pragma omp parallel for
          for (EdgeId e_i=0;e_i<recv_edges;e_i++) {
            VertexId src = recv_buffer[e_i].src;
            VertexId dst = recv_buffer[e_i].dst;
            assert(src >= partition_offset[partition_id] && src < partition_offset[partition_id+1]);
            int src_socket = get_local_partition_id(src);
            EdgeId pos = __sync_fetch_and_add(&incoming_adj_index[src_socket][dst], 1);
            incoming_adj_list[src_socket][pos].neighbour = src;
            if (!std::is_same<EdgeData, Empty>::value) {
              incoming_adj_list[src_socket][pos].edge_data = recv_buffer[e_i].edge_data;
            }
          }
        }
      });
      for (int i=0;i<partitions;i++) {
        buffered_edges[i] = 0;
      }
      assert(lseek(fin, read_offset, SEEK_SET)==read_offset);
      read_bytes = 0;
      while (read_bytes < bytes_to_read) {
        long curr_read_bytes;
        if (bytes_to_read - read_bytes > edge_unit_size * CHUNKSIZE) {
          curr_read_bytes = read(fin, read_edge_buffer, edge_unit_size * CHUNKSIZE);
        } else {
          curr_read_bytes = read(fin, read_edge_buffer, bytes_to_read - read_bytes);
        }
        assert(curr_read_bytes>=0);
        read_bytes += curr_read_bytes;
        EdgeId curr_read_edges = curr_read_bytes / edge_unit_size;
        for (EdgeId e_i=0;e_i<curr_read_edges;e_i++) {
          VertexId src = read_edge_buffer[e_i].src;
          int i = get_partition_id(src);
          memcpy(send_buffer[i].data() + edge_unit_size * buffered_edges[i], &read_edge_buffer[e_i], edge_unit_size);
          buffered_edges[i] += 1;
          if (buffered_edges[i] == CHUNKSIZE) {
            MPI_Send(send_buffer[i].data(), edge_unit_size * buffered_edges[i], MPI_CHAR, i, ShuffleGraph, MPI_COMM_WORLD);
            buffered_edges[i] = 0;
          }
        }
      }
      for (int i=0;i<partitions;i++) {
        if (buffered_edges[i]==0) continue;
        MPI_Send(send_buffer[i].data(), edge_unit_size * buffered_edges[i], MPI_CHAR, i, ShuffleGraph, MPI_COMM_WORLD);
        buffered_edges[i] = 0;
      }
      for (int i=0;i<partitions;i++) {
        char c = 0;
        MPI_Send(&c, 1, MPI_CHAR, i, ShuffleGraph, MPI_COMM_WORLD);
      }
      recv_thread_src.join();
    }
    for (int s_i=0;s_i<sockets;s_i++) {
      for (VertexId p_v_i=0;p_v_i<compressed_incoming_adj_vertices[s_i];p_v_i++) {
        VertexId v_i = compressed_incoming_adj_index[s_i][p_v_i].vertex;
        incoming_adj_index[s_i][v_i] = compressed_incoming_adj_index[s_i][p_v_i].index;
        incoming_adj_index[s_i][v_i+1] = compressed_incoming_adj_index[s_i][p_v_i+1].index;
      }
    }
    MPI_Barrier(MPI_COMM_WORLD);

    // init optimization structures
    #if ENABLE_BITMAP_CACHE == 1
    for (int i=0;i<partitions;i++) {
      for (int s_i=0;s_i<sockets;s_i++) {
        outgoing_adj_bitmap_cache[i][s_i] = (FM::bitmap_cache_item**)numa_alloc_onnode(sizeof(FM::bitmap_cache_item*) * vertices * sockets, s_i);
        memset(outgoing_adj_bitmap_cache[i][s_i], 0, sizeof(FM::bitmap_cache_item*) * vertices * sockets);
      }
    }
    outgoing_adj_bitmap_cache_pool->cache_pool = (FM::bitmap_cache_item*)malloc(sizeof(FM::bitmap_cache_item) * vertices);

    for (int i=0;i<partitions;i++) {
      for (int s_i=0;s_i<sockets;s_i++) {
        incoming_adj_bitmap_cache[i][s_i] = (FM::bitmap_cache_item**)numa_alloc_onnode(sizeof(FM::bitmap_cache_item*) * vertices * sockets, s_i);
        memset(incoming_adj_bitmap_cache[i][s_i], 0, sizeof(FM::bitmap_cache_item*) * vertices * sockets);
      }
    }
    incoming_adj_bitmap_cache_pool->cache_pool = (FM::bitmap_cache_item*)malloc(sizeof(FM::bitmap_cache_item) * vertices);
    #endif

    #if ENABLE_INDEX_CACHE == 1
    for (int i=0;i<partitions;i++) {
      for (int s_i=0;s_i<sockets;s_i++) {
        outgoing_adj_index_cache[i][s_i] = (FM::index_cache_item**)numa_alloc_onnode(sizeof(FM::index_cache_item*) * vertices * sockets, s_i);
        memset(outgoing_adj_index_cache[i][s_i], 0, sizeof(FM::index_cache_item*) * vertices * sockets);
      }
    }
    outgoing_adj_index_cache_pool->cache_pool = (FM::index_cache_item*)malloc(sizeof(FM::index_cache_item) * vertices);

    for (int i=0;i<partitions;i++) {
      for (int s_i=0;s_i<sockets;s_i++) {
        incoming_adj_index_cache[i][s_i] = (FM::index_cache_item**)numa_alloc_onnode(sizeof(FM::index_cache_item*) * vertices * sockets, s_i);
        memset(incoming_adj_index_cache[i][s_i], 0, sizeof(FM::index_cache_item*) * vertices * sockets);
      }
    }
    incoming_adj_index_cache_pool->cache_pool = (FM::index_cache_item*)malloc(sizeof(FM::index_cache_item) * vertices);
    #endif

    #if ENABLE_EDGE_CACHE == 1
    for (int i=0;i<partitions;i++) {
      for (int s_i=0;s_i<sockets;s_i++) {
        outgoing_edge_cache[i][s_i] = (FM::edge_cache_set<EdgeData>**)numa_alloc_onnode(sizeof(FM::edge_cache_set<EdgeData>*) * vertices * sockets, s_i);
        memset(outgoing_edge_cache[i][s_i], 0, sizeof(FM::edge_cache_set<EdgeData>*) * vertices * sockets);
      }
    }
    outgoing_edge_cache_pool->head.next = NULL;
    *FM::outgoing_edge_cache_pool_count = 0;

    for (int i=0;i<partitions;i++) {
      for (int s_i=0;s_i<sockets;s_i++) {
        incoming_edge_cache[i][s_i] = (FM::edge_cache_set<EdgeData>**)numa_alloc_onnode(sizeof(FM::edge_cache_set<EdgeData>*) * vertices * sockets, s_i);
        memset(incoming_edge_cache[i][s_i], 0, sizeof(FM::edge_cache_set<EdgeData>*) * vertices * sockets);
      }
    }
    incoming_edge_cache_pool->head.next = NULL;
    *FM::incoming_edge_cache_pool_count = 0;
    #endif

    delete [] buffered_edges;
    delete [] send_buffer;
    delete [] read_edge_buffer;
    delete [] recv_buffer;
    close(fin);

    transpose();
    tune_chunks();
    transpose();
    tune_chunks();

    prep_time += MPI_Wtime();

    #ifdef PRINT_DEBUG_MESSAGES
    if (partition_id==0) {
      fprintf(stderr,"preprocessing cost: %.2lf (s)\n", prep_time);
    }
    #endif
  }

  void tune_chunks() {
    tuned_chunks_dense = new ThreadState * [partitions];
    int current_send_part_id = partition_id;
    for (int step=0;step<partitions;step++) {
      current_send_part_id = (current_send_part_id + 1) % partitions;
      int i = current_send_part_id;
      tuned_chunks_dense[i] = new ThreadState [threads];
      EdgeId remained_edges;
      int remained_partitions;
      VertexId last_p_v_i;
      VertexId end_p_v_i;
      for (int t_i=0;t_i<threads;t_i++) {
        tuned_chunks_dense[i][t_i].status = WORKING;
        int s_i = get_socket_id(t_i);
        int s_j = get_socket_offset(t_i);
        if (s_j==0) {
          VertexId p_v_i = 0;
          while (p_v_i<compressed_incoming_adj_vertices[s_i]) {
            VertexId v_i = compressed_incoming_adj_index[s_i][p_v_i].vertex;
            if (v_i >= partition_offset[i]) {
              break;
            }
            p_v_i++;
          }
          last_p_v_i = p_v_i;
          while (p_v_i<compressed_incoming_adj_vertices[s_i]) {
            VertexId v_i = compressed_incoming_adj_index[s_i][p_v_i].vertex;
            if (v_i >= partition_offset[i+1]) {
              break;
            }
            p_v_i++;
          }
          end_p_v_i = p_v_i;
          remained_edges = 0;
          for (VertexId p_v_i=last_p_v_i;p_v_i<end_p_v_i;p_v_i++) {
            remained_edges += compressed_incoming_adj_index[s_i][p_v_i+1].index - compressed_incoming_adj_index[s_i][p_v_i].index;
            remained_edges += alpha;
          }
        }
        tuned_chunks_dense[i][t_i].curr = last_p_v_i;
        tuned_chunks_dense[i][t_i].end = last_p_v_i;
        remained_partitions = threads_per_socket - s_j;
        EdgeId expected_chunk_size = remained_edges / remained_partitions;
        if (remained_partitions==1) {
          tuned_chunks_dense[i][t_i].end = end_p_v_i;
        } else {
          EdgeId got_edges = 0;
          for (VertexId p_v_i=last_p_v_i;p_v_i<end_p_v_i;p_v_i++) {
            got_edges += compressed_incoming_adj_index[s_i][p_v_i+1].index - compressed_incoming_adj_index[s_i][p_v_i].index + alpha;
            if (got_edges >= expected_chunk_size) {
              tuned_chunks_dense[i][t_i].end = p_v_i;
              last_p_v_i = tuned_chunks_dense[i][t_i].end;
              break;
            }
          }
          got_edges = 0;
          for (VertexId p_v_i=tuned_chunks_dense[i][t_i].curr;p_v_i<tuned_chunks_dense[i][t_i].end;p_v_i++) {
            got_edges += compressed_incoming_adj_index[s_i][p_v_i+1].index - compressed_incoming_adj_index[s_i][p_v_i].index + alpha;
          }
          remained_edges -= got_edges;
        }
      }
    }
  }

  // process vertices
  template<typename R>
  R process_vertices(std::function<R(VertexId)> process, Bitmap * active) {
    double stream_time = 0;
    stream_time -= MPI_Wtime();

    R reducer = 0;
    size_t basic_chunk = 64;
    for (int t_i=0;t_i<threads;t_i++) {
      int s_i = get_socket_id(t_i);
      int s_j = get_socket_offset(t_i);
      VertexId partition_size = local_partition_offset[s_i+1] - local_partition_offset[s_i];
      thread_state[t_i]->curr = local_partition_offset[s_i] + partition_size / threads_per_socket  / basic_chunk * basic_chunk * s_j;
      thread_state[t_i]->end = local_partition_offset[s_i] + partition_size / threads_per_socket / basic_chunk * basic_chunk * (s_j+1);
      if (s_j == threads_per_socket - 1) {
        thread_state[t_i]->end = local_partition_offset[s_i+1];
      }
      thread_state[t_i]->status = WORKING;
    }

    // before actually processing vertices on my partiton,
    // do not need to initiate a MPI_send to request the vertex partition ranges of far memory partitions.

    #pragma omp parallel reduction(+:reducer)
    {
      R local_reducer = 0;
      int thread_id = omp_get_thread_num();
      while (true) {
        VertexId v_i = __sync_fetch_and_add(&thread_state[thread_id]->curr, basic_chunk);
        if (v_i >= thread_state[thread_id]->end) break;
        unsigned long word = active->data[WORD_OFFSET(v_i)];
        while (word != 0) {
          if (word & 1) {
            local_reducer += process(v_i);
          }
          v_i++;
          word = word >> 1;
        }
      }
      thread_state[thread_id]->status = STEALING;
      for (int t_offset=1;t_offset<threads;t_offset++) {
        int t_i = (thread_id + t_offset) % threads;
        while (thread_state[t_i]->status < STEALING) {
          VertexId v_i = __sync_fetch_and_add(&thread_state[t_i]->curr, basic_chunk);
          if (v_i >= thread_state[t_i]->end) continue;
          unsigned long word = active->data[WORD_OFFSET(v_i)];
          while (word != 0) {
            if (word & 1) {
              local_reducer += process(v_i);
            }
            v_i++;
            word = word >> 1;
          }
        }
      }
      reducer += local_reducer;
    }

    #ifdef PRINT_DEBUG_MESSAGES
    fprintf(stderr, "%d: reducer = %d\n", partition_id, reducer);
    #endif
    // Equally split the workload for processing vertices mapped onto far memory partitions
    // into each thread of each computing partitions.
    // for (int t_i=0;t_i<threads;t_i++) {
    //   VertexId partition_size = partition_offset[partitions] - partition_offset[FM::n_compute_partitions];
    //   thread_state[t_i]->curr = partition_offset[FM::n_compute_partitions] + partition_size / FM::n_compute_partitions / threads / basic_chunk * basic_chunk * (partition_id*threads + t_i);
    //   thread_state[t_i]->end =  partition_offset[FM::n_compute_partitions] + partition_size / FM::n_compute_partitions / threads / basic_chunk * basic_chunk * (partition_id*threads + t_i + 1);
    //   if (partition_id == FM::n_compute_partitions - 1 && t_i == threads - 1) {
    //     thread_state[t_i]->end = partition_offset[partitions];
    //   }
    //   thread_state[t_i]->status = WORKING_REMOTE;
    // }

    // #pragma omp parallel reduction(+:reducer)
    // {
    //   R local_reducer = 0;
    //   int thread_id = omp_get_thread_num();
    //   while (true) {
    //     VertexId v_i = __sync_fetch_and_add(&thread_state[thread_id]->curr, basic_chunk);
    //     if (v_i >= thread_state[thread_id]->end) break;
    //     unsigned long word = active->data[WORD_OFFSET(v_i)];
    //     while (word != 0) {
    //       if (word & 1) {
    //         local_reducer += process(v_i);
    //       }
    //       v_i++;
    //       word = word >> 1;
    //     }
    //   }
    //   thread_state[thread_id]->status = STEALING_REMOTE;
    //   for (int t_offset=1;t_offset<threads;t_offset++) {
    //     int t_i = (thread_id + t_offset) % threads;
    //     while (thread_state[t_i]->status < STEALING_REMOTE) {
    //       VertexId v_i = __sync_fetch_and_add(&thread_state[t_i]->curr, basic_chunk);
    //       if (v_i >= thread_state[t_i]->end) continue;
    //       unsigned long word = active->data[WORD_OFFSET(v_i)];
    //       while (word != 0) {
    //         if (word & 1) {
    //           local_reducer += process(v_i);
    //         }
    //         v_i++;
    //         word = word >> 1;
    //       }
    //     }
    //   }
    //   reducer += local_reducer;
    // }


    std::vector<uint> delegated_farmem_partitions;
    for (int i = FM::n_compute_partitions; i < partitions; ++i) {
      if (i % FM::n_compute_partitions == partition_id) {
        delegated_farmem_partitions.push_back(i);
      }
    }

    for (uint fp : delegated_farmem_partitions) {
      VertexId partition_size = partition_offset[fp+1] - partition_offset[fp];
      // for (uint b_i = partition_offset[fp]; b_i < partition_offset[fp+1]; b_i += basic_chunk) {
      //     VertexId v_i = b_i;
      //     unsigned long word = active->data[WORD_OFFSET(v_i)];
      //     while (word != 0) {
      //       if (word & 1) {
      //         write_add(&reducer, process(v_i));
      //       }
      //       v_i++;
      //       word = word >> 1;
      //     }
      // }

      for (int t_i=0;t_i<threads;t_i++) {
        thread_state[t_i]->curr = partition_offset[fp] + partition_size / threads / basic_chunk * basic_chunk * (t_i);
        thread_state[t_i]->end =  partition_offset[fp] + partition_size / threads / basic_chunk * basic_chunk * (t_i + 1);
        if (t_i == threads - 1) {
          thread_state[t_i]->end = partition_offset[fp+1];
        }
        thread_state[t_i]->status = WORKING;
      }

      #pragma omp parallel reduction(+:reducer)
      {
        R local_reducer = 0;
        int thread_id = omp_get_thread_num();
        while (true) {
          VertexId v_i = __sync_fetch_and_add(&thread_state[thread_id]->curr, basic_chunk);
          if (v_i >= thread_state[thread_id]->end) break;
          unsigned long word = active->data[WORD_OFFSET(v_i)];
          while (word != 0) {
            if (word & 1) {
              local_reducer += process(v_i);
            }
            v_i++;
            word = word >> 1;
          }
        }
        thread_state[thread_id]->status = STEALING;
        for (int t_offset=1;t_offset<threads;t_offset++) {
          int t_i = (thread_id + t_offset) % threads;
          while (thread_state[t_i]->status != STEALING) {
            VertexId v_i = __sync_fetch_and_add(&thread_state[t_i]->curr, basic_chunk);
            if (v_i >= thread_state[t_i]->end) continue;
            unsigned long word = active->data[WORD_OFFSET(v_i)];
            while (word != 0) {
              if (word & 1) {
                local_reducer += process(v_i);
              }
              v_i++;
              word = word >> 1;
            }
          }
        }
        reducer += local_reducer;
      }
    }

    #ifdef PRINT_DEBUG_MESSAGES
    fprintf(stderr, "%d: reducer = %d\n", partition_id, reducer);
    #endif
    R global_reducer;
    MPI_Datatype dt = get_mpi_data_type<R>();
    MPI_Allreduce(&reducer, &global_reducer, 1, dt, MPI_SUM, FM::compute_comm_world);
    stream_time += MPI_Wtime();
    #ifdef PRINT_DEBUG_MESSAGES
    if (partition_id==0) {
      fprintf(stderr,"process_vertices took %lf (s)\n", stream_time);
    }
    #endif
    return global_reducer;
  }

  template<typename M>
  void flush_local_send_buffer(int t_i) {
    int s_i = get_socket_id(t_i);
    int pos = __sync_fetch_and_add(&send_buffer[current_send_part_id][s_i]->count, local_send_buffer[t_i]->count);
    memcpy(send_buffer[current_send_part_id][s_i]->data + sizeof(MsgUnit<M>) * pos, local_send_buffer[t_i]->data, sizeof(MsgUnit<M>) * local_send_buffer[t_i]->count);
    local_send_buffer[t_i]->count = 0;
  }

  // emit a message to a vertex's master (dense) / mirror (sparse)
  template<typename M>
  void emit(VertexId vtx, M msg) {
    int t_i = omp_get_thread_num();
    MsgUnit<M> * buffer = (MsgUnit<M>*)local_send_buffer[t_i]->data;
    buffer[local_send_buffer[t_i]->count].vertex = vtx;
    buffer[local_send_buffer[t_i]->count].msg_data = msg;
    local_send_buffer[t_i]->count += 1;
    if (local_send_buffer[t_i]->count==local_send_buffer_limit) {
      flush_local_send_buffer<M>(t_i);
    }
  }

  // process edges
  template<typename R, typename M>
  R process_edges(std::function<void(VertexId)> sparse_signal, std::function<R(VertexId, M, VertexAdjList<EdgeData>)> sparse_slot, std::function<void(VertexId, VertexAdjList<EdgeData>)> dense_signal, std::function<R(VertexId, M)> dense_slot, Bitmap * active, Bitmap * dense_selective = nullptr) {
    double stream_time = 0;
    stream_time -= MPI_Wtime();

    for (int t_i=0;t_i<threads;t_i++) {
      local_send_buffer[t_i]->resize( sizeof(MsgUnit<M>) * local_send_buffer_limit );
      local_send_buffer[t_i]->count = 0;
    }
    R reducer = 0;
    EdgeId active_edges = process_vertices<EdgeId>(
      [&](VertexId vtx){
        return (EdgeId)out_degree[vtx];
      },
      active
    );
    size_t basic_chunk = 64;
    // bool sparse = (active_edges < edges / 20);
    bool sparse = true;
    if (sparse) {
      for (int i=0;i<partitions;i++) {
        for (int s_i=0;s_i<sockets;s_i++) {
          recv_buffer[i][s_i]->resize( sizeof(MsgUnit<M>) * ((partition_offset[i+1] - partition_offset[i])) * sockets );
          send_buffer[i][s_i]->resize( sizeof(MsgUnit<M>) * (vertices) * sockets );
          send_buffer[i][s_i]->count = 0;
          recv_buffer[i][s_i]->count = 0;
          send_buffer[i][s_i]->owned_count = 0;
          recv_buffer[i][s_i]->owned_count = 0;
          memset(send_buffer[i][s_i]->delegated_start, 0, sizeof(int)*8);
          memset(recv_buffer[i][s_i]->delegated_start, 0, sizeof(int)*8);
        }
      }
    } else {
      for (int i=0;i<partitions;i++) {
        for (int s_i=0;s_i<sockets;s_i++) {
          recv_buffer[i][s_i]->resize( sizeof(MsgUnit<M>) * owned_vertices * sockets );
          send_buffer[i][s_i]->resize( sizeof(MsgUnit<M>) * (partition_offset[i+1] - partition_offset[i]) * sockets );
          send_buffer[i][s_i]->count = 0;
          recv_buffer[i][s_i]->count = 0;
          send_buffer[i][s_i]->owned_count = 0;
          recv_buffer[i][s_i]->owned_count = 0;
          memset(send_buffer[i][s_i]->delegated_start, 0, sizeof(int)*8);
          memset(recv_buffer[i][s_i]->delegated_start, 0, sizeof(int)*8);
        }
      }
    }
    if (sparse) {
      #ifdef PRINT_DEBUG_MESSAGES
      if (partition_id==0) {
        fprintf(stderr,"sparse mode\n");
      }
      #endif
      int * recv_queue = new int [FM::n_compute_partitions];
      int recv_queue_size = 0;
      std::mutex recv_queue_mutex;

      current_send_part_id = partition_id;
      #pragma omp parallel for
      for (VertexId begin_v_i=partition_offset[partition_id];begin_v_i<partition_offset[partition_id+1];begin_v_i+=basic_chunk) {
        VertexId v_i = begin_v_i;
        unsigned long word = active->data[WORD_OFFSET(v_i)];
        while (word != 0) {
          if (word & 1) {
            sparse_signal(v_i);
          }
          v_i++;
          word = word >> 1;
        }
      }
      #pragma omp parallel for
      for (int t_i=0;t_i<threads;t_i++) {
        flush_local_send_buffer<M>(t_i);
      }
      #ifdef PRINT_DEBUG_MESSAGES
      fprintf(stderr, "%d done sparse signal.\n", partition_id);
      #endif

      for(int s_i=0; s_i<sockets; ++s_i) {
        send_buffer[current_send_part_id][s_i]->owned_count = send_buffer[current_send_part_id][s_i]->count;
        #ifdef PRINT_DEBUG_MESSAGES
        fprintf(stderr, "partition %d socket %d owns %d vertices.\n", current_send_part_id, s_i, 
                        send_buffer[current_send_part_id][s_i]->owned_count);
        #endif
      }

      #ifdef PRINT_DEBUG_MESSAGES
      if (partition_id == 0) {
        for (int i = 0; i <= partitions; ++i) {
          fprintf(stderr, "partition %d offset: %d\n", i, partition_offset[i]);
        }
      }
      #endif

      // for (int i = FM::n_compute_partitions; i < partitions; ++i) {
      //   uint remote_signal_size = partition_offset[i+1] - partition_offset[i];
      //   uint start = partition_offset[i]+remote_signal_size/FM::n_compute_partitions/basic_chunk*basic_chunk*partition_id;
      //   uint end = partition_offset[i]+remote_signal_size/FM::n_compute_partitions/basic_chunk*basic_chunk*(partition_id+1);
      //   if (i == partitions-1) {
      //     end = partition_offset[partitions];
      //   }
      //   #ifdef PRINT_DEBUG_MESSAGES
      //   printf("%d trying to process delegated vertices [%d, %d) from partition %d.\n", partition_id,
      //     start,
      //     end,
      //     i);
      //   #endif
      //   #pragma omp parallel for
      //   for (VertexId begin_v_i=start; begin_v_i<end; begin_v_i+=basic_chunk) {
      //     VertexId v_i = begin_v_i;
      //     // fprintf(stderr,"signal chunk starting at vertex %d.\n", v_i);
      //     unsigned long word = active->data[WORD_OFFSET(v_i)];
      //     while (word != 0) {
      //       if (word & 1) {
      //         printf("delegate signaling %d\n", v_i);
      //         sparse_signal(v_i);
      //       }
      //       v_i++;
      //       word = word >> 1;
      //     }
      //   }
      // }

      // uint remote_signal_size = partition_offset[partitions] - partition_offset[FM::n_compute_partitions];
      // #pragma omp parallel for
      // for (VertexId begin_v_i=partition_offset[FM::n_compute_partitions]+remote_signal_size/FM::n_compute_partitions/basic_chunk*basic_chunk*partition_id;
      //      begin_v_i<partition_offset[FM::n_compute_partitions]+remote_signal_size/FM::n_compute_partitions/basic_chunk*basic_chunk*(partition_id+1);
      //      begin_v_i+=basic_chunk) {
      //   VertexId v_i = begin_v_i;
      //   // fprintf(stderr,"signal chunk starting at vertex %d.\n", v_i);
      //   unsigned long word = active->data[WORD_OFFSET(v_i)];
      //   while (word != 0) {
      //     if (word & 1) {
      //       sparse_signal(v_i);
      //     }
      //     v_i++;
      //     word = word >> 1;
      //   }
      // }

      std::vector<uint> delegated_farmem_partitions;
      for (int i = FM::n_compute_partitions; i < partitions; ++i) {
        if (i % FM::n_compute_partitions == partition_id) {
          delegated_farmem_partitions.push_back(i);
        }
      }

      for (int i = FM::n_compute_partitions; i < partitions; ++i) {
        if (i % FM::n_compute_partitions == partition_id) {

          for(int s_i=0; s_i<sockets; ++s_i) {
            send_buffer[current_send_part_id][s_i]->delegated_start[i] = send_buffer[current_send_part_id][s_i]->count;
            #ifdef PRINT_DEBUG_MESSAGES
            fprintf(stderr, "%d send_buffer delegated_start_%d = %d\n", partition_id, i, send_buffer[current_send_part_id][s_i]->delegated_start[i]);
            #endif
          }
          // emit delegated vertices on behalf on partition i to recv_buffer, 
          // instead of actually receiving from it.
          #pragma omp parallel for
          for (VertexId begin_v_i=partition_offset[i];begin_v_i<partition_offset[i+1];begin_v_i+=basic_chunk) {
            VertexId v_i = begin_v_i;
            unsigned long word = active->data[WORD_OFFSET(v_i)];
            while (word != 0) {
              if (word & 1) {
                sparse_signal(v_i);
              }
              v_i++;
              word = word >> 1;
            }
          }

          #pragma omp parallel for
          for (int t_i=0;t_i<threads;t_i++) {
            flush_local_send_buffer<M>(t_i);
          }
        }
      }

      for(int s_i=0; s_i<sockets; ++s_i) {
        send_buffer[current_send_part_id][s_i]->delegated_start[partitions] = send_buffer[current_send_part_id][s_i]->count;
        #ifdef PRINT_DEBUG_MESSAGES
        fprintf(stderr, "%d send_buffer delegated_start_%d = %d\n", partition_id, partitions, send_buffer[current_send_part_id][s_i]->delegated_start[partitions]);
        #endif
      }
      // #pragma omp parallel for
      // for (int t_i=0;t_i<threads;t_i++) {
      //   flush_local_send_buffer<M>(t_i);
      // }

      #ifdef PRINT_DEBUG_MESSAGES
      fprintf(stderr, "%d done delegated sparse signal.\n", partition_id);
      #endif

      // bool gc_thread_should_end = false;
      // // starting a garbage collecting thread.
      // std::thread gc([&](){
      //   fprintf(stderr, "%d: started gc thread.\n", partition_id);
      //   while (true) {
      //     // std::this_thread::yield();
      //     sleep(10);
      //     if (gc_thread_should_end)
      //       break;
      //     if (!gc_thread_should_gc)
      //     uint64_t edges_cnt = 0;
      //     FM::edge_cache_set<EdgeData>* cur = outgoing_edge_cache_pool->head.next;
      //     while (cur != NULL) {
      //       if (edges_cnt + cur->edges.size() >= FM::edge_cache_pool_t<EdgeData>::EDGE_CACHE_ENTRIES) {
      //         break;
      //       }
      //       edges_cnt += cur->edges.size();
      //       cur = cur->next;
      //     }
      //     fprintf(stderr, "here2\n");
      //     if (cur != NULL) {
      //       FM::edge_cache_set<EdgeData>* next_val = cur->next;
      //       bool success = cas(&cur->next, next_val, (FM::edge_cache_set<EdgeData>*)NULL);
      //       assert(success);
      //       for (FM::edge_cache_set<EdgeData>* ptr = next_val; ptr != NULL; ptr = ptr->next) {
      //         FM::edge_cache_set<EdgeData>** addr = ptr->this_stored_address;
      //         *addr = NULL;
      //         ptr->edges.clear();
      //       }
      //     }
      //   }
      // });

      recv_queue[recv_queue_size] = partition_id;
      recv_queue_mutex.lock();
      recv_queue_size += 1;
      recv_queue_mutex.unlock();
      std::thread send_thread([&](){
        for (int step=1;step<partitions;step++) {
          int i = (partition_id - step + partitions) % partitions;
          if (i < FM::n_compute_partitions) {
            for (int s_i=0;s_i<sockets;s_i++) {
              MPI_Send(send_buffer[partition_id][s_i]->data, sizeof(MsgUnit<M>) * send_buffer[partition_id][s_i]->owned_count, MPI_CHAR, i, PassMessage, FM::compute_comm_world);
            }
          } else {
            if (i % FM::n_compute_partitions != partition_id) {
              for (int j = partition_id + FM::n_compute_partitions; j < partitions; j += FM::n_compute_partitions) {
                for (int s_i=0;s_i<sockets;s_i++) {
                  int size = send_buffer[partition_id][s_i]->delegated_start[j + FM::n_compute_partitions >= partitions ? partitions : j + FM::n_compute_partitions] - send_buffer[partition_id][s_i]->delegated_start[j];
                  MPI_Send(send_buffer[partition_id][s_i]->data + sizeof(MsgUnit<M>) * send_buffer[partition_id][s_i]->delegated_start[j], 
                    sizeof(MsgUnit<M>) * size, MPI_CHAR, i % FM::n_compute_partitions, 
                    PassMessage, FM::compute_comm_world);
                }
              }
            } else {
              // the receipient i is delegated by me, so instead of actually MPI_Send
              // let the recv_thread memcpy directly.
            }
          }
        }
      });
      std::thread recv_thread([&](){
        for (int step=1;step<partitions;step++) {
          int i = (partition_id + step) % partitions;
          if (i < FM::n_compute_partitions) {
            for (int s_i=0;s_i<sockets;s_i++) {
              MPI_Status recv_status;
              MPI_Probe(i, PassMessage, FM::compute_comm_world, &recv_status);
              MPI_Get_count(&recv_status, MPI_CHAR, &recv_buffer[i][s_i]->count);
              MPI_Recv(recv_buffer[i][s_i]->data, recv_buffer[i][s_i]->count, MPI_CHAR, i, PassMessage, FM::compute_comm_world, MPI_STATUS_IGNORE);
              recv_buffer[i][s_i]->count /= sizeof(MsgUnit<M>);
              recv_buffer[i][s_i]->owned_count = recv_buffer[i][s_i]->count;
            }
          } else {
            if (i % FM::n_compute_partitions != partition_id) {
              for (int j = i % FM::n_compute_partitions + FM::n_compute_partitions; j < partitions; j+=FM::n_compute_partitions) {
                // copy from partition i's delegated message in my own send_buffer to recv_buffer, 
                // instead of actually receiving from partition i.
                for (int s_i=0;s_i<sockets;s_i++) {
                  MPI_Status recv_status;
                  MPI_Probe(i % FM::n_compute_partitions, PassMessage, FM::compute_comm_world, &recv_status);
                  MPI_Get_count(&recv_status, MPI_CHAR, &recv_buffer[j][s_i]->count);
                  MPI_Recv(recv_buffer[j][s_i]->data, recv_buffer[j][s_i]->count, MPI_CHAR, i % FM::n_compute_partitions, PassMessage, FM::compute_comm_world, MPI_STATUS_IGNORE);
                  recv_buffer[j][s_i]->count /= sizeof(MsgUnit<M>);
                  recv_buffer[j][s_i]->owned_count = recv_buffer[j][s_i]->count;
                }
              }
            } else {
              // the sender i is delegated by me, so instead of actually MPI_Recv,
              // copy the corresponding delegated portion of i in my own send_buffer to the my corresponding recv_buffer, 
              for (int s_i=0;s_i<sockets;s_i++) {
                recv_buffer[i][s_i]->count = send_buffer[partition_id][s_i]->delegated_start[i + FM::n_compute_partitions >= partitions ? partitions : i + FM::n_compute_partitions] - send_buffer[partition_id][s_i]->delegated_start[i];
                recv_buffer[i][s_i]->owned_count = recv_buffer[i][s_i]->count;
                memcpy(recv_buffer[i][s_i]->data, send_buffer[partition_id][s_i]->data + sizeof(MsgUnit<M>)*send_buffer[partition_id][s_i]->delegated_start[i], 
                       sizeof(MsgUnit<M>)*recv_buffer[i][s_i]->count);
              }
            }
          }
          recv_queue[recv_queue_size] = i;
          recv_queue_mutex.lock();
          recv_queue_size += 1;
          recv_queue_mutex.unlock();
        }
      });

      for (int step=0;step<partitions;step++) {
        while (true) {
          recv_queue_mutex.lock();
          bool condition = (recv_queue_size<=step);
          recv_queue_mutex.unlock();
          if (!condition) break;
          __asm volatile ("pause" ::: "memory");
        }
        int i = recv_queue[step];
        MessageBuffer ** used_buffer;
        if (i==partition_id) {
          used_buffer = send_buffer[i];
        } else {
          used_buffer = recv_buffer[i];
        }

        R reducer2 = 0;
        // process recv_buffer from partition i,
        // irrespective of i being a computing node or a far memory node.
        for (int s_i=0;s_i<sockets;s_i++) {
          MsgUnit<M> * buffer = (MsgUnit<M> *)used_buffer[s_i]->data;
          size_t buffer_size = used_buffer[s_i]->owned_count;
          #ifdef PRINT_DEBUG_MESSAGES
          fprintf(stderr, "%d local sparse slot buffer_size = %d\n", partition_id, buffer_size);
          #endif
          for (int t_i=0;t_i<threads;t_i++) {
            // int s_i = get_socket_id(t_i);
            int s_j = get_socket_offset(t_i);
            VertexId partition_size = buffer_size;
            thread_state[t_i]->curr = partition_size / threads_per_socket  / basic_chunk * basic_chunk * s_j;
            thread_state[t_i]->end = partition_size / threads_per_socket / basic_chunk * basic_chunk * (s_j+1);
            if (s_j == threads_per_socket - 1) {
              thread_state[t_i]->end = buffer_size;
            }
            thread_state[t_i]->status = WORKING;
          }
          #pragma omp parallel reduction(+:reducer,reducer2)
          {
            R local_reducer = 0;
            int thread_id = omp_get_thread_num();
            int s_i = get_socket_id(thread_id);
            while (true) {
              VertexId b_i = __sync_fetch_and_add(&thread_state[thread_id]->curr, basic_chunk);
              if (b_i >= thread_state[thread_id]->end) break;
              VertexId begin_b_i = b_i;
              VertexId end_b_i = b_i + basic_chunk;
              if (end_b_i>thread_state[thread_id]->end) {
                end_b_i = thread_state[thread_id]->end;
              }
              for (b_i=begin_b_i;b_i<end_b_i;b_i++) {
                VertexId v_i = buffer[b_i].vertex;
                M msg_data = buffer[b_i].msg_data;
                if (outgoing_adj_bitmap[s_i]->get_bit(v_i)) {
                  // printf("local sparse_slot %d\n", v_i);
                  local_reducer += sparse_slot(v_i, msg_data, VertexAdjList<EdgeData>(outgoing_adj_list[s_i] + outgoing_adj_index[s_i][v_i], outgoing_adj_list[s_i] + outgoing_adj_index[s_i][v_i+1]));
                }
              }
            }
            thread_state[thread_id]->status = STEALING;
            for (int t_offset=1;t_offset<threads;t_offset++) {
              int t_i = (thread_id + t_offset) % threads;
              if (thread_state[t_i]->status==STEALING) continue;
              while (true) {
                VertexId b_i = __sync_fetch_and_add(&thread_state[t_i]->curr, basic_chunk);
                if (b_i >= thread_state[t_i]->end) break;
                VertexId begin_b_i = b_i;
                VertexId end_b_i = b_i + basic_chunk;
                if (end_b_i>thread_state[t_i]->end) {
                  end_b_i = thread_state[t_i]->end;
                }
                int s_i = get_socket_id(t_i);
                for (b_i=begin_b_i;b_i<end_b_i;b_i++) {
                  VertexId v_i = buffer[b_i].vertex;
                  M msg_data = buffer[b_i].msg_data;
                  if (outgoing_adj_bitmap[s_i]->get_bit(v_i)) {
                    // printf("local sparse_slot %d\n", v_i);
                    local_reducer += sparse_slot(v_i, msg_data, VertexAdjList<EdgeData>(outgoing_adj_list[s_i] + outgoing_adj_index[s_i][v_i], outgoing_adj_list[s_i] + outgoing_adj_index[s_i][v_i+1]));
                  }
                }
              }
            }
            reducer += local_reducer;
            reducer2 += local_reducer;
          }
        }
        #ifdef PRINT_DEBUG_MESSAGES
        fprintf(stderr, "%d done local sparse slot at step %d. reducer2 = %d\n", partition_id, step, reducer2);
        #endif
      }

      for (int step=0;step<partitions;step++) {
        for (uint fp : delegated_farmem_partitions) {
          int i = (fp + step) % partitions;
          #ifdef PRINT_DEBUG_MESSAGES
          fprintf(stderr, "%d serving as %d to sparse slot %d at step %d\n", partition_id, fp, i, step);
          #endif
          MessageBuffer ** used_buffer = nullptr;
          if (i==partition_id) {
            used_buffer = send_buffer[i];
          } else {
            used_buffer = recv_buffer[i];
          }

          R reducer2 = 0;
          // handling received vertices delegated to partition i
          for (int s_i=0;s_i<sockets;s_i++) {
            // MsgUnit<M> * buffer = ((MsgUnit<M> *)used_buffer[s_i]->data) + used_buffer[s_i]->owned_count;
            // size_t buffer_size = used_buffer[s_i]->count - used_buffer[s_i]->owned_count;
            MsgUnit<M> * buffer = (MsgUnit<M> *)used_buffer[s_i]->data;
            size_t buffer_size = used_buffer[s_i]->owned_count;
            #ifdef PRINT_DEBUG_MESSAGES
            fprintf(stderr, "%d remote sparse slot buffer_size = %d\n", partition_id, buffer_size);
            #endif
            for (int t_i=0;t_i<threads;t_i++) {
              // int s_i = get_socket_id(t_i);
              int s_j = get_socket_offset(t_i);
              VertexId partition_size = buffer_size;
              thread_state[t_i]->curr = partition_size / threads_per_socket  / basic_chunk * basic_chunk * s_j;
              thread_state[t_i]->end = partition_size / threads_per_socket / basic_chunk * basic_chunk * (s_j+1);
              if (s_j == threads_per_socket - 1) {
                thread_state[t_i]->end = buffer_size;
              }
              thread_state[t_i]->status = WORKING;
            }

            FM::edge_cache_set<EdgeData>* gc_edgeset_start = NULL;
            std::mutex insert_after_head;
            #pragma omp parallel reduction(+:reducer,reducer2)
            {
              R local_reducer = 0;
              int thread_id = omp_get_thread_num();
              int s_i = get_socket_id(thread_id);
              while (true) {
                VertexId b_i = __sync_fetch_and_add(&thread_state[thread_id]->curr, basic_chunk);
                if (b_i >= thread_state[thread_id]->end) break;
                VertexId begin_b_i = b_i;
                VertexId end_b_i = b_i + basic_chunk;
                if (end_b_i>thread_state[thread_id]->end) {
                  end_b_i = thread_state[thread_id]->end;
                }
                for (b_i=begin_b_i;b_i<end_b_i;b_i++) {
                  VertexId v_i = buffer[b_i].vertex;
                  M msg_data = buffer[b_i].msg_data;
                  uint remote_node = fp;
                  unsigned long word;
                  #if ENABLE_BITMAP_CACHE == 1
                    auto cached_item_ptr = outgoing_adj_bitmap_cache[remote_node][s_i][v_i];
                    if (cached_item_ptr != NULL) {
                      word = cached_item_ptr->word;
                      (*FM::outgoing_adj_bitmap_cache_hit)++;
                    } else {
                      MPI_Win_lock(MPI_LOCK_SHARED, remote_node, 0, *outgoing_adj_bitmap_data_win[s_i][thread_id]);
                      MPI_Get(&word, 1, MPI_UNSIGNED_LONG, remote_node, WORD_OFFSET(v_i), 1, MPI_UNSIGNED_LONG, *outgoing_adj_bitmap_data_win[s_i][thread_id]);
                      MPI_Win_unlock(remote_node, *outgoing_adj_bitmap_data_win[s_i][thread_id]);
                      uint64_t cache_index = __sync_fetch_and_add(&outgoing_adj_bitmap_cache_pool->cache_pool_count, 1);
                      outgoing_adj_bitmap_cache_pool->cache_pool[cache_index] = FM::bitmap_cache_item(word);
                      outgoing_adj_bitmap_cache[remote_node][s_i][v_i] = &outgoing_adj_bitmap_cache_pool->cache_pool[cache_index];
                      (*FM::outgoing_adj_bitmap_cache_miss)++;
                    }
                  #else
                  MPI_Win_lock(MPI_LOCK_SHARED, remote_node, 0, *outgoing_adj_bitmap_data_win[s_i][thread_id]);
                  MPI_Get(&word, 1, MPI_UNSIGNED_LONG, remote_node, WORD_OFFSET(v_i), 1, MPI_UNSIGNED_LONG, *outgoing_adj_bitmap_data_win[s_i][thread_id]);
                  MPI_Win_unlock(remote_node, *outgoing_adj_bitmap_data_win[s_i][thread_id]);
                  #endif
                  if (word & (1ul<<BIT_OFFSET(v_i))) {
                    // retrieve index and index+1
                    EdgeId indices[2];
                    #if ENABLE_INDEX_CACHE == 1
                    auto cached_item_ptr = outgoing_adj_index_cache[remote_node][s_i][v_i];
                    if (cached_item_ptr != NULL) {
                      indices[0] = cached_item_ptr->first;
                      indices[1] = cached_item_ptr->second;
                      (*FM::outgoing_adj_index_cache_hit)++;
                    } else {
                      MPI_Win_lock(MPI_LOCK_SHARED, remote_node, 0, *outgoing_adj_index_data_win[s_i][thread_id]);
                      MPI_Get(&indices, 2, MPI_UNSIGNED_LONG, remote_node, v_i, 2, MPI_UNSIGNED_LONG, *outgoing_adj_index_data_win[s_i][thread_id]);
                      MPI_Win_unlock(remote_node, *outgoing_adj_index_data_win[s_i][thread_id]);
                      uint64_t cache_index = __sync_fetch_and_add(&outgoing_adj_index_cache_pool->cache_pool_count, 1);
                      outgoing_adj_index_cache_pool->cache_pool[cache_index] = FM::index_cache_item(indices[0], indices[1]);
                      outgoing_adj_index_cache[remote_node][s_i][v_i] = &outgoing_adj_index_cache_pool->cache_pool[cache_index];
                      (*FM::outgoing_adj_index_cache_miss)++;
                    }
                    #else
                    MPI_Win_lock(MPI_LOCK_SHARED, remote_node, 0, *outgoing_adj_index_data_win[s_i][thread_id]);
                    MPI_Get(&indices, 2, MPI_UNSIGNED_LONG, remote_node, v_i, 2, MPI_UNSIGNED_LONG, *outgoing_adj_index_data_win[s_i][thread_id]);
                    MPI_Win_unlock(remote_node, *outgoing_adj_index_data_win[s_i][thread_id]);
                    #endif

                    // retrieve corresponding list values between [index, index+1)
                    EdgeId n_adj_edges = indices[1]-indices[0];
                    std::vector<AdjUnit<EdgeData>> locally_cached_adj(n_adj_edges+1);
                    #if ENABLE_EDGE_CACHE == 1
                    auto cached_edgeset_ptr = outgoing_edge_cache[remote_node][s_i][v_i];
                    if (cached_edgeset_ptr != NULL) {
                      bool success = cas(&cached_edgeset_ptr->status, 0, 1);
                      if (!success) {
                        // I am already been garbage collected. construct the cached_edgeset from the network.
                        assert(cached_edgeset_ptr->status == 2);
                        MPI_Win_lock(MPI_LOCK_SHARED, remote_node, 0, *outgoing_adj_list_data_win[s_i][thread_id]);
                        MPI_Get(&locally_cached_adj[0], n_adj_edges*unit_size, MPI_CHAR,
                                remote_node, indices[0], n_adj_edges*unit_size, MPI_CHAR, *outgoing_adj_list_data_win[s_i][thread_id]);
                        MPI_Win_unlock(remote_node, *outgoing_adj_list_data_win[s_i][thread_id]);
                        auto set = new FM::edge_cache_set<EdgeData>(v_i);
                        for (int idx = 0; idx < n_adj_edges; ++idx) {
                          set->edges.emplace_back(locally_cached_adj[idx]);
                        }
                        outgoing_edge_cache[remote_node][s_i][v_i] = set;
                        {
                          const std::lock_guard<std::mutex> lock(insert_after_head);
                          set->edges_cnt_after_me_in_history = outgoing_edge_cache_pool->head.all_edges_cnt_in_history;
                          set->next = outgoing_edge_cache_pool->head.next;
                          outgoing_edge_cache_pool->head.all_edges_cnt_in_history += set->edges.size();
                          outgoing_edge_cache_pool->head.next = set;
                        }
                        (*FM::outgoing_edge_cache_pool_count) += n_adj_edges;
                        (*FM::outgoing_edge_cache_miss)++;
                      } else {
                        // I haven't been garbage collected and have successfully set my status to ACCESS.
                        assert(cached_edgeset_ptr->edges.size() == n_adj_edges);
                        for(int idx = 0; idx < cached_edgeset_ptr->edges.size(); ++idx) {
                          locally_cached_adj[idx].neighbour = cached_edgeset_ptr->edges[idx].edge.neighbour;
                          locally_cached_adj[idx].edge_data = cached_edgeset_ptr->edges[idx].edge.edge_data;
                        }
                        cached_edgeset_ptr->status = 0;

                        uint64_t diff = outgoing_edge_cache_pool->head.all_edges_cnt_in_history - cached_edgeset_ptr->edges_cnt_after_me_in_history;
                        if (diff >= FM::edge_cache_pool_t<EdgeData>::EDGE_CACHE_ENTRIES) {
                          // I need to do edge recycling.
                          bool success = cas(&gc_edgeset_start, (FM::edge_cache_set<EdgeData>*)NULL, cached_edgeset_ptr->next);
                          if (success) {
                            // I am the only one recycler. I need to do both age refreshing and edge recycling for list node after me.
                            auto refreshed_set = new FM::edge_cache_set<EdgeData>(v_i);
                            for (int idx = 0; idx < cached_edgeset_ptr->edges.size(); ++idx) {
                              refreshed_set->edges.emplace_back(locally_cached_adj[idx]);
                            }
                            outgoing_edge_cache[remote_node][s_i][v_i] = refreshed_set;
                            {
                              const std::lock_guard<std::mutex> lock(insert_after_head);
                              refreshed_set->edges_cnt_after_me_in_history = outgoing_edge_cache_pool->head.all_edges_cnt_in_history;
                              refreshed_set->next = outgoing_edge_cache_pool->head.next;
                              outgoing_edge_cache_pool->head.all_edges_cnt_in_history += cached_edgeset_ptr->edges.size();
                              outgoing_edge_cache_pool->head.next = refreshed_set;
                            }

                            // do the recycling
                            for (FM::edge_cache_set<EdgeData>* ptr = gc_edgeset_start; ptr != NULL && ptr->status != 2; ptr = ptr->next) {
                              while (true) {
                                bool success = cas(&ptr->status, 0, 2);
                                if (success) break;
                              }
                              (*FM::outgoing_edge_cache_pool_count) -= ptr->edges.size();
                              ptr->edges.clear();
                            }
                          } else {
                            // some other thread is recycling, I will skip this recycling step this time.
                            // only do age refreshing with a recycling thread running in mind.
                            auto refreshed_set = new FM::edge_cache_set<EdgeData>(v_i);
                            for (int idx = 0; idx < n_adj_edges; ++idx) {
                              refreshed_set->edges.emplace_back(locally_cached_adj[idx]);
                            }
                            outgoing_edge_cache[remote_node][s_i][v_i] = refreshed_set;
                            {
                              const std::lock_guard<std::mutex> lock(insert_after_head);
                              refreshed_set->edges_cnt_after_me_in_history = outgoing_edge_cache_pool->head.all_edges_cnt_in_history;
                              refreshed_set->next = outgoing_edge_cache_pool->head.next;
                              outgoing_edge_cache_pool->head.all_edges_cnt_in_history += cached_edgeset_ptr->edges.size();
                              outgoing_edge_cache_pool->head.next = refreshed_set;
                            }
                          }
                        } else {
                          // I can continue staying in the edge cache only need to do age refreshing
                          // with a recycling thread running in mind.
                          auto refreshed_set = new FM::edge_cache_set<EdgeData>(v_i);
                          for (int idx = 0; idx < n_adj_edges; ++idx) {
                            refreshed_set->edges.emplace_back(locally_cached_adj[idx]);
                          }
                          outgoing_edge_cache[remote_node][s_i][v_i] = refreshed_set;
                          {
                            const std::lock_guard<std::mutex> lock(insert_after_head);
                            refreshed_set->edges_cnt_after_me_in_history = outgoing_edge_cache_pool->head.all_edges_cnt_in_history;
                            refreshed_set->next = outgoing_edge_cache_pool->head.next;
                            outgoing_edge_cache_pool->head.all_edges_cnt_in_history += cached_edgeset_ptr->edges.size();
                            outgoing_edge_cache_pool->head.next = refreshed_set;
                          }
                        }
                      }
                      (*FM::outgoing_edge_cache_hit)++;
                    } else {
                      MPI_Win_lock(MPI_LOCK_SHARED, remote_node, 0, *outgoing_adj_list_data_win[s_i][thread_id]);
                      MPI_Get(&locally_cached_adj[0], n_adj_edges*unit_size, MPI_CHAR,
                              remote_node, indices[0], n_adj_edges*unit_size, MPI_CHAR, *outgoing_adj_list_data_win[s_i][thread_id]);
                      MPI_Win_unlock(remote_node, *outgoing_adj_list_data_win[s_i][thread_id]);
                      auto set = new FM::edge_cache_set<EdgeData>(v_i);
                      for (int idx = 0; idx < n_adj_edges; ++idx) {
                        set->edges.emplace_back(locally_cached_adj[idx]);
                      }
                      outgoing_edge_cache[remote_node][s_i][v_i] = set;
                      {
                        const std::lock_guard<std::mutex> lock(insert_after_head);
                        set->edges_cnt_after_me_in_history = outgoing_edge_cache_pool->head.all_edges_cnt_in_history;
                        set->next = outgoing_edge_cache_pool->head.next;
                        outgoing_edge_cache_pool->head.all_edges_cnt_in_history += set->edges.size();
                        outgoing_edge_cache_pool->head.next = set;
                      }
                      (*FM::outgoing_edge_cache_pool_count) += n_adj_edges;
                      (*FM::outgoing_edge_cache_miss)++;
                    }
                    #else
                    MPI_Win_lock(MPI_LOCK_SHARED, remote_node, 0, *outgoing_adj_list_data_win[s_i][thread_id]);
                    MPI_Get(&locally_cached_adj[0], n_adj_edges*unit_size, MPI_CHAR, 
                            remote_node, indices[0], n_adj_edges*unit_size, MPI_CHAR, *outgoing_adj_list_data_win[s_i][thread_id]);
                    MPI_Win_unlock(remote_node, *outgoing_adj_list_data_win[s_i][thread_id]);
                    #endif
                    // printf("remote sparse_slot %d\n", v_i);
                    local_reducer += sparse_slot(v_i, msg_data, VertexAdjList<EdgeData>(&locally_cached_adj[0], &locally_cached_adj[n_adj_edges]));
                  }
                }
              }
              thread_state[thread_id]->status = STEALING;
              for (int t_offset=1;t_offset<threads;t_offset++) {
                int t_i = (thread_id + t_offset) % threads;
                if (thread_state[t_i]->status==STEALING) continue;
                while (true) {
                  VertexId b_i = __sync_fetch_and_add(&thread_state[t_i]->curr, basic_chunk);
                  if (b_i >= thread_state[t_i]->end) break;
                  VertexId begin_b_i = b_i;
                  VertexId end_b_i = b_i + basic_chunk;
                  if (end_b_i>thread_state[t_i]->end) {
                    end_b_i = thread_state[t_i]->end;
                  }
                  int s_i = get_socket_id(t_i);
                  for (b_i=begin_b_i;b_i<end_b_i;b_i++) {
                    VertexId v_i = buffer[b_i].vertex;
                    M msg_data = buffer[b_i].msg_data;
                    uint remote_node = fp;
                    unsigned long word;
                    #if ENABLE_BITMAP_CACHE == 1
                    auto cached_item_ptr = outgoing_adj_bitmap_cache[remote_node][s_i][v_i];
                    if (cached_item_ptr != NULL) {
                      word = cached_item_ptr->word;
                      (*FM::outgoing_adj_bitmap_cache_hit)++;
                    } else {
                      MPI_Win_lock(MPI_LOCK_SHARED, remote_node, 0, *outgoing_adj_bitmap_data_win[s_i][thread_id]);
                      MPI_Get(&word, 1, MPI_UNSIGNED_LONG, remote_node, WORD_OFFSET(v_i), 1, MPI_UNSIGNED_LONG, *outgoing_adj_bitmap_data_win[s_i][thread_id]);
                      MPI_Win_unlock(remote_node, *outgoing_adj_bitmap_data_win[s_i][thread_id]);
                      uint64_t cache_index = __sync_fetch_and_add(&outgoing_adj_bitmap_cache_pool->cache_pool_count, 1);
                      outgoing_adj_bitmap_cache_pool->cache_pool[cache_index] = FM::bitmap_cache_item(word);
                      outgoing_adj_bitmap_cache[remote_node][s_i][v_i] = &outgoing_adj_bitmap_cache_pool->cache_pool[cache_index];
                      (*FM::outgoing_adj_bitmap_cache_miss)++;
                    }
                    #else
                    MPI_Win_lock(MPI_LOCK_SHARED, remote_node, 0, *outgoing_adj_bitmap_data_win[s_i][thread_id]);
                    MPI_Get(&word, 1, MPI_UNSIGNED_LONG, remote_node, WORD_OFFSET(v_i), 1, MPI_UNSIGNED_LONG, *outgoing_adj_bitmap_data_win[s_i][thread_id]);
                    MPI_Win_unlock(remote_node, *outgoing_adj_bitmap_data_win[s_i][thread_id]);
                    #endif
                    if (word & (1ul<<BIT_OFFSET(v_i))) {
                      // retrieve index and index+1
                      EdgeId indices[2];
                      #if ENABLE_INDEX_CACHE == 1
                      auto cached_item_ptr = outgoing_adj_index_cache[remote_node][s_i][v_i];
                      if (cached_item_ptr != NULL) {
                        indices[0] = cached_item_ptr->first;
                        indices[1] = cached_item_ptr->second;
                        (*FM::outgoing_adj_index_cache_hit)++;
                      } else {
                        MPI_Win_lock(MPI_LOCK_SHARED, remote_node, 0, *outgoing_adj_index_data_win[s_i][thread_id]);
                        MPI_Get(&indices, 2, MPI_UNSIGNED_LONG, remote_node, v_i, 2, MPI_UNSIGNED_LONG, *outgoing_adj_index_data_win[s_i][thread_id]);
                        MPI_Win_unlock(remote_node, *outgoing_adj_index_data_win[s_i][thread_id]);
                        uint64_t cache_index = __sync_fetch_and_add(&outgoing_adj_index_cache_pool->cache_pool_count, 1);
                        outgoing_adj_index_cache_pool->cache_pool[cache_index] = FM::index_cache_item(indices[0], indices[1]);
                        outgoing_adj_index_cache[remote_node][s_i][v_i] = &outgoing_adj_index_cache_pool->cache_pool[cache_index];
                        (*FM::outgoing_adj_index_cache_miss)++;
                      }
                      #else
                      MPI_Win_lock(MPI_LOCK_SHARED, remote_node, 0, *outgoing_adj_index_data_win[s_i][thread_id]);
                      MPI_Get(&indices, 2, MPI_UNSIGNED_LONG, remote_node, v_i, 2, MPI_UNSIGNED_LONG, *outgoing_adj_index_data_win[s_i][thread_id]);
                      MPI_Win_unlock(remote_node, *outgoing_adj_index_data_win[s_i][thread_id]);
                      #endif

                      // retrieve corresponding list values between [index, index+1)
                      EdgeId n_adj_edges = indices[1]-indices[0];
                      std::vector<AdjUnit<EdgeData>> locally_cached_adj(n_adj_edges+1);
                      #if ENABLE_EDGE_CACHE == 1
                      auto cached_edgeset_ptr = outgoing_edge_cache[remote_node][s_i][v_i];
                      if (cached_edgeset_ptr != NULL) {
                        bool success = cas(&cached_edgeset_ptr->status, 0, 1);
                        if (!success) {
                          // I am already been garbage collected. construct the cached_edgeset from the network.
                          assert(cached_edgeset_ptr->status == 2);
                          MPI_Win_lock(MPI_LOCK_SHARED, remote_node, 0, *outgoing_adj_list_data_win[s_i][thread_id]);
                          MPI_Get(&locally_cached_adj[0], n_adj_edges*unit_size, MPI_CHAR,
                                  remote_node, indices[0], n_adj_edges*unit_size, MPI_CHAR, *outgoing_adj_list_data_win[s_i][thread_id]);
                          MPI_Win_unlock(remote_node, *outgoing_adj_list_data_win[s_i][thread_id]);
                          auto set = new FM::edge_cache_set<EdgeData>(v_i);
                          for (int idx = 0; idx < n_adj_edges; ++idx) {
                            set->edges.emplace_back(locally_cached_adj[idx]);
                          }
                          outgoing_edge_cache[remote_node][s_i][v_i] = set;
                          {
                            const std::lock_guard<std::mutex> lock(insert_after_head);
                            set->edges_cnt_after_me_in_history = outgoing_edge_cache_pool->head.all_edges_cnt_in_history;
                            set->next = outgoing_edge_cache_pool->head.next;
                            outgoing_edge_cache_pool->head.all_edges_cnt_in_history += set->edges.size();
                            outgoing_edge_cache_pool->head.next = set;
                          }
                          (*FM::outgoing_edge_cache_pool_count) += n_adj_edges;
                          (*FM::outgoing_edge_cache_miss)++;
                        } else {
                          // I haven't been garbage collected and have successfully set my status to ACCESS.
                          assert(cached_edgeset_ptr->edges.size() == n_adj_edges);
                          for(int idx = 0; idx < cached_edgeset_ptr->edges.size(); ++idx) {
                            locally_cached_adj[idx].neighbour = cached_edgeset_ptr->edges[idx].edge.neighbour;
                            locally_cached_adj[idx].edge_data = cached_edgeset_ptr->edges[idx].edge.edge_data;
                          }
                          cached_edgeset_ptr->status = 0;

                          uint64_t diff = outgoing_edge_cache_pool->head.all_edges_cnt_in_history - cached_edgeset_ptr->edges_cnt_after_me_in_history;
                          if (diff >= FM::edge_cache_pool_t<EdgeData>::EDGE_CACHE_ENTRIES) {
                            // I need to do edge recycling.
                            bool success = cas(&gc_edgeset_start, (FM::edge_cache_set<EdgeData>*)NULL, cached_edgeset_ptr->next);
                            if (success) {
                              // I am the only one recycler. I need to do both age refreshing and edge recycling for list node after me.
                              auto refreshed_set = new FM::edge_cache_set<EdgeData>(v_i);
                              for (int idx = 0; idx < cached_edgeset_ptr->edges.size(); ++idx) {
                                refreshed_set->edges.emplace_back(locally_cached_adj[idx]);
                              }
                              outgoing_edge_cache[remote_node][s_i][v_i] = refreshed_set;
                              {
                                const std::lock_guard<std::mutex> lock(insert_after_head);
                                refreshed_set->edges_cnt_after_me_in_history = outgoing_edge_cache_pool->head.all_edges_cnt_in_history;
                                refreshed_set->next = outgoing_edge_cache_pool->head.next;
                                outgoing_edge_cache_pool->head.all_edges_cnt_in_history += cached_edgeset_ptr->edges.size();
                                outgoing_edge_cache_pool->head.next = refreshed_set;
                              }

                              // do the recycling
                              for (FM::edge_cache_set<EdgeData>* ptr = gc_edgeset_start; ptr != NULL && ptr->status != 2; ptr = ptr->next) {
                                while (true) {
                                  bool success = cas(&ptr->status, 0, 2);
                                  if (success) break;
                                }
                                (*FM::outgoing_edge_cache_pool_count) -= ptr->edges.size();
                                ptr->edges.clear();
                              }
                            } else {
                              // some other thread is recycling, I will skip this recycling step this time.
                              // only do age refreshing with a recycling thread running in mind.
                              auto refreshed_set = new FM::edge_cache_set<EdgeData>(v_i);
                              for (int idx = 0; idx < n_adj_edges; ++idx) {
                                refreshed_set->edges.emplace_back(locally_cached_adj[idx]);
                              }
                              outgoing_edge_cache[remote_node][s_i][v_i] = refreshed_set;
                              {
                                const std::lock_guard<std::mutex> lock(insert_after_head);
                                refreshed_set->edges_cnt_after_me_in_history = outgoing_edge_cache_pool->head.all_edges_cnt_in_history;
                                refreshed_set->next = outgoing_edge_cache_pool->head.next;
                                outgoing_edge_cache_pool->head.all_edges_cnt_in_history += cached_edgeset_ptr->edges.size();
                                outgoing_edge_cache_pool->head.next = refreshed_set;
                              }
                            }
                          } else {
                            // I can continue staying in the edge cache only need to do age refreshing
                            // with a recycling thread running in mind.
                            auto refreshed_set = new FM::edge_cache_set<EdgeData>(v_i);
                            for (int idx = 0; idx < n_adj_edges; ++idx) {
                              refreshed_set->edges.emplace_back(locally_cached_adj[idx]);
                            }
                            outgoing_edge_cache[remote_node][s_i][v_i] = refreshed_set;
                            {
                              const std::lock_guard<std::mutex> lock(insert_after_head);
                              refreshed_set->edges_cnt_after_me_in_history = outgoing_edge_cache_pool->head.all_edges_cnt_in_history;
                              refreshed_set->next = outgoing_edge_cache_pool->head.next;
                              outgoing_edge_cache_pool->head.all_edges_cnt_in_history += cached_edgeset_ptr->edges.size();
                              outgoing_edge_cache_pool->head.next = refreshed_set;
                            }
                          }
                        }
                        (*FM::outgoing_edge_cache_hit)++;
                      } else {
                        MPI_Win_lock(MPI_LOCK_SHARED, remote_node, 0, *outgoing_adj_list_data_win[s_i][thread_id]);
                        MPI_Get(&locally_cached_adj[0], n_adj_edges*unit_size, MPI_CHAR,
                                remote_node, indices[0], n_adj_edges*unit_size, MPI_CHAR, *outgoing_adj_list_data_win[s_i][thread_id]);
                        MPI_Win_unlock(remote_node, *outgoing_adj_list_data_win[s_i][thread_id]);
                        auto set = new FM::edge_cache_set<EdgeData>(v_i);
                        for (int idx = 0; idx < n_adj_edges; ++idx) {
                          set->edges.emplace_back(locally_cached_adj[idx]);
                        }
                        outgoing_edge_cache[remote_node][s_i][v_i] = set;
                        {
                          const std::lock_guard<std::mutex> lock(insert_after_head);
                          set->edges_cnt_after_me_in_history = outgoing_edge_cache_pool->head.all_edges_cnt_in_history;
                          set->next = outgoing_edge_cache_pool->head.next;
                          outgoing_edge_cache_pool->head.all_edges_cnt_in_history += set->edges.size();
                          outgoing_edge_cache_pool->head.next = set;
                        }
                        (*FM::outgoing_edge_cache_pool_count) += n_adj_edges;
                        (*FM::outgoing_edge_cache_miss)++;
                      }
                      #else
                      MPI_Win_lock(MPI_LOCK_SHARED, remote_node, 0, *outgoing_adj_list_data_win[s_i][thread_id]);
                      MPI_Get(&locally_cached_adj[0], n_adj_edges*unit_size, MPI_CHAR, 
                              remote_node, indices[0], n_adj_edges*unit_size, MPI_CHAR, *outgoing_adj_list_data_win[s_i][thread_id]);
                      MPI_Win_unlock(remote_node, *outgoing_adj_list_data_win[s_i][thread_id]);
                      #endif
                      // printf("remote sparse_slot %d\n", v_i);
                      local_reducer += sparse_slot(v_i, msg_data, VertexAdjList<EdgeData>(&locally_cached_adj[0], &locally_cached_adj[n_adj_edges]));
                    }
                  }
                }
              }
              reducer += local_reducer;
              reducer2 += local_reducer;
            }
          }
          #ifdef PRINT_DEBUG_MESSAGES
          fprintf(stderr, "%d done delegated sparse slot at step %d. reducer2 = %d\n", partition_id, step, reducer2);
          #endif
        }
      }
      // uint farmem_partitions = partitions - FM::n_compute_partitions;
      // for (int step=0;step<farmem_partitions;step++) {
      //   for (uint fp : delegated_farmem_partitions) {
      //     int i = ((fp - FM::n_compute_partitions) + step) % farmem_partitions + FM::n_compute_partitions;
      //     #ifdef PRINT_DEBUG_MESSAGES
      //     fprintf(stderr, "%d serving as %d to sparse slot %d at step %d\n", partition_id, fp, i, step);
      //     #endif
      //     MessageBuffer ** used_buffer = recv_buffer[fp];
      //     // handling received vertices delegated to partition i
      //     for (int s_i=0;s_i<sockets;s_i++) {
      //       // MsgUnit<M> * buffer = ((MsgUnit<M> *)used_buffer[s_i]->data) + used_buffer[s_i]->owned_count;
      //       // size_t buffer_size = used_buffer[s_i]->count - used_buffer[s_i]->owned_count;
      //       MsgUnit<M> * buffer = (MsgUnit<M> *)used_buffer[s_i]->data;
      //       size_t buffer_size = used_buffer[s_i]->owned_count;
      //       #ifdef PRINT_DEBUG_MESSAGES
      //       fprintf(stderr, "%d remote sparse slot buffer_size = %d\n", partition_id, buffer_size);
      //       #endif
      //       for (int t_i=0;t_i<threads;t_i++) {
      //         // int s_i = get_socket_id(t_i);
      //         int s_j = get_socket_offset(t_i);
      //         VertexId partition_size = buffer_size;
      //         thread_state[t_i]->curr = partition_size / threads_per_socket  / basic_chunk * basic_chunk * s_j;
      //         thread_state[t_i]->end = partition_size / threads_per_socket / basic_chunk * basic_chunk * (s_j+1);
      //         if (s_j == threads_per_socket - 1) {
      //           thread_state[t_i]->end = buffer_size;
      //         }
      //         thread_state[t_i]->status = WORKING;
      //       }

      //       #pragma omp parallel reduction(+:reducer)
      //       {
      //         R local_reducer = 0;
      //         int thread_id = omp_get_thread_num();
      //         int s_i = get_socket_id(thread_id);
      //         while (true) {
      //           VertexId b_i = __sync_fetch_and_add(&thread_state[thread_id]->curr, basic_chunk);
      //           if (b_i >= thread_state[thread_id]->end) break;
      //           VertexId begin_b_i = b_i;
      //           VertexId end_b_i = b_i + basic_chunk;
      //           if (end_b_i>thread_state[thread_id]->end) {
      //             end_b_i = thread_state[thread_id]->end;
      //           }
      //           for (b_i=begin_b_i;b_i<end_b_i;b_i++) {
      //             VertexId v_i = buffer[b_i].vertex;
      //             M msg_data = buffer[b_i].msg_data;

      //             // now v_i cannot be on partition i
      //             // since partition i's owned vertices
      //             // have been processed before.
      //             uint remote_node = i;
      //             unsigned long word;
      //             MPI_Win_lock(MPI_LOCK_SHARED, remote_node, 0, *outgoing_adj_bitmap_data_win[s_i][thread_id]);
      //             MPI_Get(&word, 1, MPI_UNSIGNED_LONG, remote_node, WORD_OFFSET(v_i), 1, MPI_UNSIGNED_LONG, *outgoing_adj_bitmap_data_win[s_i][thread_id]);
      //             MPI_Win_unlock(remote_node, *outgoing_adj_bitmap_data_win[s_i][thread_id]);
      //             if (word & (1ul<<BIT_OFFSET(v_i))) {
      //               // retrieve index and index+1
      //               EdgeId indices[2];
      //               MPI_Win_lock(MPI_LOCK_SHARED, remote_node, 0, *outgoing_adj_index_data_win[s_i][thread_id]);
      //               MPI_Get(&indices, 2, MPI_UNSIGNED_LONG, remote_node, v_i, 2, MPI_UNSIGNED_LONG, *outgoing_adj_index_data_win[s_i][thread_id]);
      //               MPI_Win_unlock(remote_node, *outgoing_adj_index_data_win[s_i][thread_id]);
      //               // retrieve corresponding list values between [index, index+1)
      //               EdgeId n_adj_edges = indices[1]-indices[0];
      //               std::vector<AdjUnit<EdgeData>> locally_cached_adj(n_adj_edges+1);
      //               MPI_Win_lock(MPI_LOCK_SHARED, remote_node, 0, *outgoing_adj_list_data_win[s_i][thread_id]);
      //               MPI_Get(&locally_cached_adj[0], n_adj_edges*unit_size, MPI_CHAR, 
      //                       remote_node, indices[0], n_adj_edges*unit_size, MPI_CHAR, *outgoing_adj_list_data_win[s_i][thread_id]);
      //               MPI_Win_unlock(remote_node, *outgoing_adj_list_data_win[s_i][thread_id]);
      //               // printf("remote sparse_slot %d\n", v_i);
      //               local_reducer += sparse_slot(v_i, msg_data, VertexAdjList<EdgeData>(&locally_cached_adj[0], &locally_cached_adj[n_adj_edges]));
      //             }
      //           }
      //         }
      //         thread_state[thread_id]->status = STEALING;
      //         for (int t_offset=1;t_offset<threads;t_offset++) {
      //           int t_i = (thread_id + t_offset) % threads;
      //           if (thread_state[t_i]->status==STEALING) continue;
      //           while (true) {
      //             VertexId b_i = __sync_fetch_and_add(&thread_state[t_i]->curr, basic_chunk);
      //             if (b_i >= thread_state[t_i]->end) break;
      //             VertexId begin_b_i = b_i;
      //             VertexId end_b_i = b_i + basic_chunk;
      //             if (end_b_i>thread_state[t_i]->end) {
      //               end_b_i = thread_state[t_i]->end;
      //             }
      //             int s_i = get_socket_id(t_i);
      //             for (b_i=begin_b_i;b_i<end_b_i;b_i++) {
      //               VertexId v_i = buffer[b_i].vertex;
      //               M msg_data = buffer[b_i].msg_data;
      //               uint remote_node = i;
      //               unsigned long word;
      //               MPI_Win_lock(MPI_LOCK_SHARED, remote_node, 0, *outgoing_adj_bitmap_data_win[s_i][thread_id]);
      //               MPI_Get(&word, 1, MPI_UNSIGNED_LONG, remote_node, WORD_OFFSET(v_i), 1, MPI_UNSIGNED_LONG, *outgoing_adj_bitmap_data_win[s_i][thread_id]);
      //               MPI_Win_unlock(remote_node, *outgoing_adj_bitmap_data_win[s_i][thread_id]);
      //               if (word & (1ul<<BIT_OFFSET(v_i))) {
      //                 // retrieve index and index+1
      //                 EdgeId indices[2];
      //                 MPI_Win_lock(MPI_LOCK_SHARED, remote_node, 0, *outgoing_adj_index_data_win[s_i][thread_id]);
      //                 MPI_Get(&indices, 2, MPI_UNSIGNED_LONG, remote_node, v_i, 2, MPI_UNSIGNED_LONG, *outgoing_adj_index_data_win[s_i][thread_id]);
      //                 MPI_Win_unlock(remote_node, *outgoing_adj_index_data_win[s_i][thread_id]);
      //                 // retrieve corresponding list values between [index, index+1)
      //                 EdgeId n_adj_edges = indices[1]-indices[0];
      //                 std::vector<AdjUnit<EdgeData>> locally_cached_adj(n_adj_edges+1);
      //                 MPI_Win_lock(MPI_LOCK_SHARED, remote_node, 0, *outgoing_adj_list_data_win[s_i][thread_id]);
      //                 MPI_Get(&locally_cached_adj[0], n_adj_edges*unit_size, MPI_CHAR, 
      //                         remote_node, indices[0], n_adj_edges*unit_size, MPI_CHAR, *outgoing_adj_list_data_win[s_i][thread_id]);
      //                 MPI_Win_unlock(remote_node, *outgoing_adj_list_data_win[s_i][thread_id]);
      //                 // printf("remote sparse_slot %d\n", v_i);
      //                 local_reducer += sparse_slot(v_i, msg_data, VertexAdjList<EdgeData>(&locally_cached_adj[0], &locally_cached_adj[n_adj_edges]));
      //               }
      //             }
      //           }
      //         }
      //         reducer += local_reducer;
      //       }
      //     }
      //   }
      // }
      send_thread.join();
        #ifdef PRINT_DEBUG_MESSAGES
        fprintf(stderr, "%d sender thread joined.\n", partition_id);
        #endif
      recv_thread.join();
        #ifdef PRINT_DEBUG_MESSAGES
        fprintf(stderr, "%d receiver thread joined.\n", partition_id);
        #endif
      // gc_thread_should_end = true;
      // gc.join();
      //   #ifdef PRINT_DEBUG_MESSAGES
      //   fprintf(stderr, "%d gc thread joined.\n", partition_id);
      //   #endif
      delete [] recv_queue;
    } else {
      // dense selective bitmap
      if (dense_selective!=nullptr && partitions>1) {
        double sync_time = 0;
        sync_time -= get_time();
        std::thread send_thread([&](){
          for (int step=1;step<partitions;step++) {
            int recipient_id = (partition_id + step) % partitions;
            MPI_Send(dense_selective->data + WORD_OFFSET(partition_offset[partition_id]), (owned_vertices + 63) / 64, MPI_UNSIGNED_LONG, recipient_id, PassMessage, MPI_COMM_WORLD);
          }
        });
        std::thread recv_thread([&](){
          for (int step=1;step<partitions;step++) {
            int sender_id = (partition_id - step + partitions) % partitions;
            MPI_Recv(dense_selective->data + WORD_OFFSET(partition_offset[sender_id]), (partition_offset[sender_id + 1] - partition_offset[sender_id] + 63) / 64, MPI_UNSIGNED_LONG, sender_id, PassMessage, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
          }
        });
        send_thread.join();
        recv_thread.join();
        MPI_Barrier(MPI_COMM_WORLD);
        sync_time += get_time();
        #ifdef PRINT_DEBUG_MESSAGES
        if (partition_id==0) {
          fprintf(stderr,"sync_time = %lf\n", sync_time);
        }
        #endif
      }
      #ifdef PRINT_DEBUG_MESSAGES
      if (partition_id==0) {
        fprintf(stderr,"dense mode\n");
      }
      #endif
      int * send_queue = new int [partitions];
      int * recv_queue = new int [partitions];
      volatile int send_queue_size = 0;
      volatile int recv_queue_size = 0;
      std::mutex send_queue_mutex;
      std::mutex recv_queue_mutex;

      std::thread send_thread([&](){
        for (int step=0;step<partitions;step++) {
          if (step==partitions-1) {
            break;
          }
          while (true) {
            send_queue_mutex.lock();
            bool condition = (send_queue_size<=step);
            send_queue_mutex.unlock();
            if (!condition) break;
            __asm volatile ("pause" ::: "memory");
          }
          int i = send_queue[step];
          for (int s_i=0;s_i<sockets;s_i++) {
            MPI_Send(send_buffer[i][s_i]->data, sizeof(MsgUnit<M>) * send_buffer[i][s_i]->count, MPI_CHAR, i, PassMessage, MPI_COMM_WORLD);
          }
        }
      });
      std::thread recv_thread([&](){
        std::vector<std::thread> threads;
        for (int step=1;step<partitions;step++) {
          int i = (partition_id - step + partitions) % partitions;
          threads.emplace_back([&](int i){
            for (int s_i=0;s_i<sockets;s_i++) {
              MPI_Status recv_status;
              MPI_Probe(i, PassMessage, MPI_COMM_WORLD, &recv_status);
              MPI_Get_count(&recv_status, MPI_CHAR, &recv_buffer[i][s_i]->count);
              MPI_Recv(recv_buffer[i][s_i]->data, recv_buffer[i][s_i]->count, MPI_CHAR, i, PassMessage, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
              recv_buffer[i][s_i]->count /= sizeof(MsgUnit<M>);
            }
          }, i);
        }
        for (int step=1;step<partitions;step++) {
          int i = (partition_id - step + partitions) % partitions;
          threads[step-1].join();
          recv_queue[recv_queue_size] = i;
          recv_queue_mutex.lock();
          recv_queue_size += 1;
          recv_queue_mutex.unlock();
        }
        recv_queue[recv_queue_size] = partition_id;
        recv_queue_mutex.lock();
        recv_queue_size += 1;
        recv_queue_mutex.unlock();
      });
      current_send_part_id = partition_id;
      for (int step=0;step<partitions;step++) {
        current_send_part_id = (current_send_part_id + 1) % partitions;
        int i = current_send_part_id;
        for (int t_i=0;t_i<threads;t_i++) {
          *thread_state[t_i] = tuned_chunks_dense[i][t_i];
        }
        #pragma omp parallel
        {
          int thread_id = omp_get_thread_num();
          int s_i = get_socket_id(thread_id);
          VertexId final_p_v_i = thread_state[thread_id]->end;
          while (true) {
            VertexId begin_p_v_i = __sync_fetch_and_add(&thread_state[thread_id]->curr, basic_chunk);
            if (begin_p_v_i >= final_p_v_i) break;
            VertexId end_p_v_i = begin_p_v_i + basic_chunk;
            if (end_p_v_i > final_p_v_i) {
              end_p_v_i = final_p_v_i;
            }
            for (VertexId p_v_i = begin_p_v_i; p_v_i < end_p_v_i; p_v_i ++) {
              VertexId v_i = compressed_incoming_adj_index[s_i][p_v_i].vertex;
              dense_signal(v_i, VertexAdjList<EdgeData>(incoming_adj_list[s_i] + compressed_incoming_adj_index[s_i][p_v_i].index, incoming_adj_list[s_i] + compressed_incoming_adj_index[s_i][p_v_i+1].index));
            }
          }
          thread_state[thread_id]->status = STEALING;
          for (int t_offset=1;t_offset<threads;t_offset++) {
            int t_i = (thread_id + t_offset) % threads;
            int s_i = get_socket_id(t_i);
            while (thread_state[t_i]->status!=STEALING) {
              VertexId begin_p_v_i = __sync_fetch_and_add(&thread_state[t_i]->curr, basic_chunk);
              if (begin_p_v_i >= thread_state[t_i]->end) break;
              VertexId end_p_v_i = begin_p_v_i + basic_chunk;
              if (end_p_v_i > thread_state[t_i]->end) {
                end_p_v_i = thread_state[t_i]->end;
              }
              for (VertexId p_v_i = begin_p_v_i; p_v_i < end_p_v_i; p_v_i ++) {
                VertexId v_i = compressed_incoming_adj_index[s_i][p_v_i].vertex;
                dense_signal(v_i, VertexAdjList<EdgeData>(incoming_adj_list[s_i] + compressed_incoming_adj_index[s_i][p_v_i].index, incoming_adj_list[s_i] + compressed_incoming_adj_index[s_i][p_v_i+1].index));
              }
            }
          }
        }
        #pragma omp parallel for
        for (int t_i=0;t_i<threads;t_i++) {
          flush_local_send_buffer<M>(t_i);
        }
        if (i!=partition_id) {
          send_queue[send_queue_size] = i;
          send_queue_mutex.lock();
          send_queue_size += 1;
          send_queue_mutex.unlock();
        }
      }
      for (int step=0;step<partitions;step++) {
        while (true) {
          recv_queue_mutex.lock();
          bool condition = (recv_queue_size<=step);
          recv_queue_mutex.unlock();
          if (!condition) break;
          __asm volatile ("pause" ::: "memory");
        }
        int i = recv_queue[step];
        MessageBuffer ** used_buffer;
        if (i==partition_id) {
          used_buffer = send_buffer[i];
        } else {
          used_buffer = recv_buffer[i];
        }
        for (int t_i=0;t_i<threads;t_i++) {
          int s_i = get_socket_id(t_i);
          int s_j = get_socket_offset(t_i);
          VertexId partition_size = used_buffer[s_i]->count;
          thread_state[t_i]->curr = partition_size / threads_per_socket  / basic_chunk * basic_chunk * s_j;
          thread_state[t_i]->end = partition_size / threads_per_socket / basic_chunk * basic_chunk * (s_j+1);
          if (s_j == threads_per_socket - 1) {
            thread_state[t_i]->end = used_buffer[s_i]->count;
          }
          thread_state[t_i]->status = WORKING;
        }
        #pragma omp parallel reduction(+:reducer)
        {
          R local_reducer = 0;
          int thread_id = omp_get_thread_num();
          int s_i = get_socket_id(thread_id);
          MsgUnit<M> * buffer = (MsgUnit<M> *)used_buffer[s_i]->data;
          while (true) {
            VertexId b_i = __sync_fetch_and_add(&thread_state[thread_id]->curr, basic_chunk);
            if (b_i >= thread_state[thread_id]->end) break;
            VertexId begin_b_i = b_i;
            VertexId end_b_i = b_i + basic_chunk;
            if (end_b_i>thread_state[thread_id]->end) {
              end_b_i = thread_state[thread_id]->end;
            }
            for (b_i=begin_b_i;b_i<end_b_i;b_i++) {
              VertexId v_i = buffer[b_i].vertex;
              M msg_data = buffer[b_i].msg_data;
              local_reducer += dense_slot(v_i, msg_data);
            }
          }
          thread_state[thread_id]->status = STEALING;
          reducer += local_reducer;
        }
      }
      send_thread.join();
      recv_thread.join();
      delete [] send_queue;
      delete [] recv_queue;
    }

    R global_reducer;
    MPI_Datatype dt = get_mpi_data_type<R>();
    MPI_Allreduce(&reducer, &global_reducer, 1, dt, MPI_SUM, FM::compute_comm_world);
    // dt = get_mpi_data_type<unsigned long>();
    // MPI_Allreduce(MPI_IN_PLACE, active->data, WORD_OFFSET(vertices), dt, MPI_BOR, FM::compute_comm_world);
    stream_time += MPI_Wtime();
    #ifdef PRINT_DEBUG_MESSAGES
    if (partition_id==0) {
      fprintf(stderr,"process_edges took %lf (s)\n", stream_time);
    }
    #endif
    return global_reducer;
  }

};

#endif

