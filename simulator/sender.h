#ifndef SENDER
#define SENDER

#include <arpa/inet.h>
#include <emmintrin.h>
#include <immintrin.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <boost/align/aligned_allocator.hpp>
#include <chrono>
#include <iostream>
#include <numeric>
#include <thread>
#include <vector>

#include "concurrentqueue.h"
#include "config.h"
#include "datatype_conversion.inc"
#include "gettime.inc"
#include "memory_manage.h"
#include "mkl_dfti.h"
#include "net.h"
#include "symbols.h"
#include "utils.h"

#ifdef USE_DPDK
#include <netinet/ether.h>

#include "dpdk_transport.h"
#endif

class Sender {
 public:
  static constexpr size_t kDequeueBulkSize = 4;

  /**
   * @brief Create and optionally start a Sender that sends IQ packets to a
   * server with MAC address [server_mac_addr_str]
   *
   * @param config The Agora config
   *
   * @param socket_thread_num Number of worker threads sending packets
   *
   * @param core_offset The master thread runs on core [core_offset]. Worker
   * thread #i runs on core [core_offset + i]
   *
   * @param frame_duration The TTI slot duration
   *
   * @param enable_slow_start If 1, the sender initially sends frames in a
   * duration larger than the TTI
   *
   * @param server_mac_addr_str The MAC address of the server's NIC
   */
  Sender(Config* cfg, size_t socket_thread_num, size_t core_offset = 30,
         size_t frame_duration = 1000, size_t enable_slow_start = 1,
         std::string server_mac_addr_str = "ff:ff:ff:ff:ff:ff",
         bool create_thread_for_master = false);

  ~Sender();

  void StartTx();

  // in_frame_start and in_frame_end must have space for at least
  // kNumStatsFrames entries
  void StartTXfromMain(double* in_frame_start, double* in_frame_end);

 private:
  void* MasterThread(int tid);
  void* WorkerThread(int tid);

  /**
   * @brief Read time-domain 32-bit floating-point IQ samples from [filename]
   * and populate iq_data_short_ by converting to 16-bit fixed-point samples
   *
   * [filename] must contain data for one frame. For every symbol and antenna,
   * the file must provide (CP_LEN + OFDM_CA_NUM) IQ samples.
   */
  void InitIqFromFile(std::string filename);

  // Get number of CPU ticks for a symbol given a frame index
  uint64_t GetTicksForFrame(size_t frame_id) const;
  size_t GetMaxSymbolId() const;

  // Launch threads to run worker with thread IDs from tid_start to tid_end
  void CreateThreads(void* (*worker)(void*), int tid_start, int tid_end);

  void DelayForSymbol(size_t tx_frame_count, uint64_t tick_start);
  void DelayForFrame(size_t tx_frame_count, uint64_t tick_start);

  void WriteStatsToFile(size_t tx_frame_count) const;

  size_t FindNextSymbol(size_t frame, size_t start_symbol);
  void ScheduleSymbol(size_t frame, size_t symbol_id);

  // Run FFT on the data field in pkt, output to fft_inout
  // Recombine pkt header data and fft output data into payload
  void RunFft(Packet* pkt, complex_float* fft_inout,
              DFTI_DESCRIPTOR_HANDLE mkl_handle) const;

  Config* cfg_;
  const double freq_ghz_;           // RDTSC frequency in GHz
  const double ticks_per_usec_;     // RDTSC frequency in GHz
  const size_t socket_thread_num_;  // Number of worker threads sending pkts
  const size_t enable_slow_start_;  // If 1, send frames slowly at first

  // The master thread runs on core core_offset. Worker threads use cores
  // {core_offset + 1, ..., core_offset + thread_num - 1}
  const size_t core_offset_;
  const size_t frame_duration_;

  // RDTSC clock ticks between the start of transmission of two symbols in
  // the steady state
  const uint64_t ticks_all_;

  // ticks_wnd_1 and ticks_wnd_2 are the RDTSC clock ticks between the start
  // of transmission of two symbols for the first several frames
  const uint64_t ticks_wnd_1_;
  const uint64_t ticks_wnd_2_;

  moodycamel::ConcurrentQueue<size_t> send_queue_ =
      moodycamel::ConcurrentQueue<size_t>(1024);
  moodycamel::ConcurrentQueue<size_t> completion_queue_ =
      moodycamel::ConcurrentQueue<size_t>(1024);
  moodycamel::ProducerToken** task_ptok_;

  // First dimension: symbol_num_perframe * BS_ANT_NUM
  // Second dimension: (CP_LEN + OFDM_CA_NUM) * 2
  Table<unsigned short> iq_data_short_;

  // Number of packets transmitted for each symbol in a frame
  size_t* packet_count_per_symbol_[kFrameWnd];

  double* frame_start_;
  double* frame_end_;

#ifdef USE_DPDK
  struct rte_mempool* mbuf_pool;
  uint32_t bs_rru_addr;     // IPv4 address of this data sender
  uint32_t bs_server_addr;  // IPv4 address of the remote target Agora server
  // MAC addresses of this data sender
  std::vector<rte_ether_addr> sender_mac_addr;
  // MAC addresses of the remote target Agora server
  std::vector<rte_ether_addr> server_mac_addr;
#endif
};

#endif