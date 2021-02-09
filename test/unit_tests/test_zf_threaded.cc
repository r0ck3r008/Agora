#include <gtest/gtest.h>
// For some reason, gtest include order matters
#include "concurrentqueue.h"
#include "config.h"
#include "dozf.h"
#include "gettime.h"
#include "utils.h"
#include <thread>

static constexpr size_t kNumWorkers = 14;
static constexpr size_t kMaxTestNum = 100;
static constexpr size_t kMaxItrNum = (1 << 30);
static constexpr size_t kAntTestNum = 3;
static constexpr size_t kBsAntNums[kAntTestNum] = { 32, 16, 48 };
static constexpr size_t kFrameOffsets[kAntTestNum] = { 0, 20, 30 };
// A spinning barrier to synchronize the start of worker threads
std::atomic<size_t> num_workers_ready_atomic;

void MasterToWorkerDynamicMaster(Config* cfg,
    moodycamel::ConcurrentQueue<EventData>& event_queue,
    moodycamel::ConcurrentQueue<EventData>& complete_task_queue)
{
    PinToCoreWithOffset(ThreadType::kMaster, cfg->core_offset_, 0);
    // Wait for all worker threads to be ready
    while (num_workers_ready_atomic != kNumWorkers) {
        // Wait
    }

    for (size_t bs_ant_idx = 0; bs_ant_idx < kAntTestNum; bs_ant_idx++) {
        cfg->bs_ant_num_ = kBsAntNums[bs_ant_idx];
        for (size_t i = 0; i < kMaxTestNum; i++) {
            uint32_t frame_id
                = i / cfg->zf_events_per_symbol_ + kFrameOffsets[bs_ant_idx];
            size_t base_sc_id
                = (i % cfg->zf_events_per_symbol_) * cfg->zf_block_size_;
            event_queue.enqueue(EventData(
                EventType::kZF, gen_tag_t::FrmSc(frame_id, base_sc_id).tag_));
        }

        // Dequeue all events in queue to avoid overflow
        size_t num_finished_events = 0;
        while (num_finished_events < kMaxTestNum) {
            EventData event;
            int ret = static_cast<int>(complete_task_queue.try_dequeue(event));
            if (ret != 0) {
                num_finished_events++;
}
        }
    }
}

void MasterToWorkerDynamicWorker(Config* cfg, size_t worker_id,
    moodycamel::ConcurrentQueue<EventData>& event_queue,
    moodycamel::ConcurrentQueue<EventData>& complete_task_queue,
    moodycamel::ProducerToken* ptok,
    PtrGrid<kFrameWnd, kMaxUEs, complex_float>& csi_buffers,
    Table<complex_float>& calib_dl_buffer,
    Table<complex_float>& calib_ul_buffer,
    PtrGrid<kFrameWnd, kMaxDataSCs, complex_float>& ul_zf_matrices,
    PtrGrid<kFrameWnd, kMaxDataSCs, complex_float>& dl_zf_matrices,
    Stats* stats)
{
    PinToCoreWithOffset(
        ThreadType::kWorker, cfg->core_offset_ + 1, worker_id);

    // Wait for all threads (including master) to start runnung
    num_workers_ready_atomic++;
    while (num_workers_ready_atomic != kNumWorkers) {
        // Wait
    }

    auto *compute_zf = new DoZF(cfg, worker_id, csi_buffers, calib_dl_buffer,
        calib_ul_buffer, ul_zf_matrices, dl_zf_matrices, stats);

    size_t start_tsc = Rdtsc();
    size_t num_tasks = 0;
    EventData req_event;
    size_t max_frame_id_wo_offset
        = (kMaxTestNum - 1) / (cfg->ofdm_data_num_ / cfg->zf_block_size_);
    for (size_t i = 0; i < kMaxItrNum; i++) {
        if (event_queue.try_dequeue(req_event)) {
            num_tasks++;
            size_t frame_offset_id = 0;
            size_t cur_frame_id = gen_tag_t(req_event.tags_[0]).frame_id_;
            if (cur_frame_id >= kFrameOffsets[1]
                and cur_frame_id - kFrameOffsets[1] <= max_frame_id_wo_offset) {
                frame_offset_id = 1;
            } else if (cur_frame_id >= kFrameOffsets[2]
                and cur_frame_id - kFrameOffsets[2] <= max_frame_id_wo_offset) {
                frame_offset_id = 2;
            }
            ASSERT_EQ(cfg->bs_ant_num_, kBsAntNums[frame_offset_id]);
            EventData resp_event = compute_zf->launch(req_event.tags_[0]);
            TryEnqueueFallback(&complete_task_queue, ptok, resp_event);
        }
    }
    double ms = CyclesToMs(Rdtsc() - start_tsc, cfg->freq_ghz_);

    std::printf("Worker %zu: %zu tasks, time per task = %.4f ms\n", worker_id,
        num_tasks, ms / num_tasks);
}

/// Test correctness of BS_ANT_NUM values in multi-threaded zeroforcing
/// when BS_ANT_NUM varies in runtime
TEST(TestZF, VaryingConfig)
{
    static constexpr size_t kNumIters = 10000;
    auto* cfg = new Config("data/tddconfig-sim-ul.json");
    cfg->GenData();

    auto event_queue = moodycamel::ConcurrentQueue<EventData>(2 * kNumIters);
    moodycamel::ProducerToken* ptoks[kNumWorkers];
    auto complete_task_queue
        = moodycamel::ConcurrentQueue<EventData>(2 * kNumIters);
    for (size_t i = 0; i < kNumWorkers; i++) {
        ptoks[i] = new moodycamel::ProducerToken(complete_task_queue);
    }

    Table<complex_float> calib_dl_buffer;
    Table<complex_float> calib_ul_buffer;

    PtrGrid<kFrameWnd, kMaxUEs, complex_float> csi_buffers;
    csi_buffers.RandAllocCxFloat(kMaxAntennas * kMaxDataSCs);

    PtrGrid<kFrameWnd, kMaxDataSCs, complex_float> ul_zf_matrices(
        kMaxAntennas * kMaxUEs);
    PtrGrid<kFrameWnd, kMaxDataSCs, complex_float> dl_zf_matrices(
        kMaxUEs * kMaxAntennas);

    calib_dl_buffer.RandAllocCxFloat(
        kFrameWnd, kMaxDataSCs * kMaxAntennas, Agora_memory::Alignment_t::kK64Align);
    calib_ul_buffer.RandAllocCxFloat(
        kFrameWnd, kMaxDataSCs * kMaxAntennas, Agora_memory::Alignment_t::kK64Align);

    auto *stats = new Stats(cfg);

    auto master = std::thread(MasterToWorkerDynamicMaster, cfg,
        std::ref(event_queue), std::ref(complete_task_queue));
    std::thread workers[kNumWorkers];
    for (size_t i = 0; i < kNumWorkers; i++) {
        workers[i] = std::thread(MasterToWorkerDynamicWorker, cfg, i,
            std::ref(event_queue), std::ref(complete_task_queue), ptoks[i],
            std::ref(csi_buffers), std::ref(calib_dl_buffer),
            std::ref(calib_ul_buffer), std::ref(ul_zf_matrices),
            std::ref(dl_zf_matrices), stats);
    }
    master.join();
    for (auto& w : workers) {
        w.join();
}
}

int main(int argc, char** argv)
{
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
