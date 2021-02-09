#ifndef PHY_STATS
#define PHY_STATS

#include <armadillo>

#include "config.h"
#include "memory_manage.h"
#include "symbols.h"

using namespace arma;
class PhyStats {
 public:
  PhyStats(Config* cfg);
  ~PhyStats();
  void PrintPhyStats();
  void UpdateBitErrors(size_t /*ue_id*/, size_t /*offset*/, uint8_t /*tx_byte*/,
                       uint8_t /*rx_byte*/);
  void UpdateDecodedBits(size_t /*ue_id*/, size_t /*offset*/,
                         size_t /*new_bits_num*/);
  void UpdateBlockErrors(size_t /*ue_id*/, size_t /*offset*/,
                         size_t /*block_error_count*/);
  void IncrementDecodedBlocks(size_t /*ue_id*/, size_t /*offset*/);
  void UpdateUncodedBitErrors(size_t /*ue_id*/, size_t /*offset*/,
                              size_t /*mod_bit_size*/, uint8_t /*tx_byte*/,
                              uint8_t /*rx_byte*/);
  void UpdateUncodedBits(size_t /*ue_id*/, size_t /*offset*/,
                         size_t /*new_bits_num*/);
  void UpdateEvmStats(size_t /*frame_id*/, size_t /*sc_id*/, cx_fmat /*eq*/);
  void PrintEvmStats(size_t /*frame_id*/);
  void UpdatePilotSnr(size_t /*frame_id*/, size_t /*ue_id*/,
                      complex_float* /*fft_data*/);
  float GetEvmSnr(size_t frame_id, size_t ue_id);
  void PrintSnrStats(size_t /*frame_id*/);

 private:
  Config* config_;
  Table<size_t> decoded_bits_count_;
  Table<size_t> bit_error_count_;
  Table<size_t> decoded_blocks_count_;
  Table<size_t> block_error_count_;
  Table<size_t> uncoded_bits_count_;
  Table<size_t> uncoded_bit_error_count_;
  Table<float> evm_buffer_;
  Table<float> pilot_snr_;

  cx_fmat ul_gt_mat_;
};

#endif