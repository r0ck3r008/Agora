#pragma once

#include <string>

#include "config.h"
#include "utils_ldpc.h"

/**
 * @brief Building blocks for generating end-to-end or unit test workloads for
 * Agora
 */
class DataGenerator {
 public:
  // The profile of the input information bits
  enum class Profile {
    kRandom,  // The input information bytes are chosen at random

    // The input informatioon bytes are {1, 2, 3, 1, 2, 3, ...} for UE 0,
    // {4, 5, 6, 4, 5, 6, ...} for UE 1, and so on
    kK123
  };

  DataGenerator(Config* cfg, uint64_t seed = 0,
                Profile profile = Profile::kRandom)
      : cfg_(cfg), profile_(profile) {
    if (seed != 0) {
      fast_rand_.seed_ = seed;
    }
  }

  /**
   * @brief Generate one information bit sequence and the corresponding
   * encoded bit sequence for one code block for the active LDPC configuration
   *
   * @param information The generated input bit sequence
   * @param encoded_codeword The generated encoded codeword bit sequence
   * @param ue_id ID of the UE that this codeblock belongs to
   */
  void GenCodeblock(std::vector<int8_t>& information,
                    std::vector<int8_t>& encoded_codeword, size_t ue_id) {
    const LDPCconfig& lc = cfg_->LdpcConfig();
    std::vector<int8_t> parity;
    parity.resize(
        LdpcEncodingParityBufSize(lc.BaseGraph(), lc.ExpansionFactor()));

    information.resize(
        LdpcEncodingInputBufSize(lc.BaseGraph(), lc.ExpansionFactor()));
    encoded_codeword.resize(
        LdpcEncodingEncodedBufSize(lc.BaseGraph(), lc.ExpansionFactor()));

    for (size_t i = 0; i < lc.NumInputBytes(); i++) {
      if (profile_ == Profile::kRandom) {
        information.at(i) = static_cast<int8_t>(fast_rand_.NextU32());
      } else if (profile_ == Profile::kK123) {
        information.at(i) = 1 + (ue_id * 3) + (i % 3);
      }
    }

    LdpcEncodeHelper(cfg_->LdpcConfig().BaseGraph(),
                     cfg_->LdpcConfig().ExpansionFactor(),
                     cfg_->LdpcConfig().NumRows(), &encoded_codeword.at(0),
                     &parity.at(0), &information.at(0));

    information.resize(lc.NumInputBytes());
    encoded_codeword.resize(lc.NumEncodedBytes());
  }

  /**
   * @brief Return the output of modulating the encoded codeword
   * @param encoded_codeword The encoded LDPC codeword bit sequence
   * @return An array of complex floats with OFDM_DATA_NUM elements
   */
  std::vector<complex_float> GetModulation(
      const std::vector<int8_t>& encoded_codeword) {
    std::vector<complex_float> modulated_codeword(cfg_->OfdmDataNum());
    std::vector<uint8_t> mod_input(cfg_->OfdmDataNum());

    AdaptBitsForMod(reinterpret_cast<const uint8_t*>(&encoded_codeword[0]),
                    &mod_input[0], cfg_->LdpcConfig().NumEncodedBytes(),
                    cfg_->ModOrderBits());

    for (size_t i = 0; i < cfg_->OfdmDataNum(); i++) {
      modulated_codeword[i] = ModSingleUint8(mod_input[i], cfg_->ModTable());
    }
    return modulated_codeword;
  }

  std::vector<complex_float> GetModulation(const int8_t* encoded_codeword,
                                           size_t num_bits) {
    std::vector<complex_float> modulated_codeword(cfg_->OfdmDataNum());
    std::vector<uint8_t> mod_input(cfg_->OfdmDataNum());

    AdaptBitsForMod(reinterpret_cast<const uint8_t*>(&encoded_codeword[0]),
                    &mod_input[0], BitsToBytes(num_bits), cfg_->ModOrderBits());

    for (size_t i = 0; i < cfg_->OfdmDataNum(); i++) {
      modulated_codeword[i] = ModSingleUint8(mod_input[i], cfg_->ModTable());
    }
    return modulated_codeword;
  }

  /**
   * @param modulated_codeword The modulated codeword with OFDM_DATA_NUM
   * elements
   * @brief An array with OFDM_CA_NUM elements with the OFDM_DATA_NUM
   * modulated elements binned at the center
   */
  std::vector<complex_float> BinForIfft(
      const std::vector<complex_float> modulated_codeword) const {
    std::vector<complex_float> pre_ifft_symbol(cfg_->OfdmCaNum());  // Zeroed
    std::memcpy(&pre_ifft_symbol[cfg_->OfdmDataStart()], &modulated_codeword[0],
                cfg_->OfdmDataNum() * sizeof(complex_float));

    return pre_ifft_symbol;
  }

  /// Return the time-domain pilot symbol with OFDM_CA_NUM complex floats
  std::vector<complex_float> GetCommonPilotTimeDomain() const {
    const std::vector<std::complex<float>> zc_seq = Utils::DoubleToCfloat(
        CommsLib::GetSequence(cfg_->OfdmDataNum(), CommsLib::kLteZadoffChu));

    const std::vector<std::complex<float>> zc_common_pilot =
        CommsLib::SeqCyclicShift(zc_seq, M_PI / 4.0);  // Used in LTE SRS

    std::vector<complex_float> ret(cfg_->OfdmCaNum());  // Zeroed
    for (size_t i = 0; i < cfg_->OfdmDataNum(); i++) {
      ret[i + cfg_->OfdmDataStart()] = {zc_common_pilot[i].real(),
                                        zc_common_pilot[i].imag()};
    }

    return ret;
  }

 private:
  FastRand fast_rand_;     // A fast random number generator
  Config* cfg_;            // The global Agora config
  const Profile profile_;  // The pattern of the input byte sequence
};