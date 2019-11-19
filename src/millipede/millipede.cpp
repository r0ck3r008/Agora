/**
 * Author: Jian Ding
 * Email: jianding17@gmail.com
 * 
 */
#include "millipede.hpp"

// typedef cx_float COMPLEX;
bool keep_running = true;

void intHandler(int) {
    std::cout << "will exit..." << std::endl;
    keep_running = false;
}


Millipede::Millipede(Config *cfg)
{
    std::string directory = TOSTRING(PROJECT_DIRECTORY);
    printf("PROJECT_DIRECTORY: %s\n", directory.c_str());
    printf("Main thread: on core %d\n", sched_getcpu());
    // std::string env_parameter = "MKL_THREADING_LAYER=sequential";
    // char *env_parameter_char = (char *)env_parameter.c_str();
    // putenv(env_parameter_char);
    putenv("MKL_THREADING_LAYER=sequential");
    std::cout << "MKL_THREADING_LAYER =  " << getenv("MKL_THREADING_LAYER") << std::endl; 
    // openblas_set_num_threads(1);
    printf("enter constructor\n");


    this->cfg_ = cfg;
    initialize_vars_from_cfg(cfg_);

    pin_to_core_with_offset(Master, CORE_OFFSET, 0);

    initialize_queues();

    printf("initialize uplink buffers\n");
    initialize_uplink_buffers();

    if (downlink_mode)
    {
        printf("initialize downlink buffers\n");
        initialize_downlink_buffers();
    }
    stats_manager_ = new Stats(cfg_, 4, TASK_THREAD_NUM, FFT_THREAD_NUM, ZF_THREAD_NUM, DEMUL_THREAD_NUM);
    
    /* initialize TXRX threads*/
    printf("new TXRX\n");
    receiver_.reset(new PacketTXRX(cfg_, SOCKET_RX_THREAD_NUM, SOCKET_TX_THREAD_NUM, CORE_OFFSET + 1, 
                    &message_queue_, &tx_queue_, rx_ptoks_ptr, tx_ptoks_ptr));

    /* create worker threads */
#if BIGSTATION
    create_threads(Worker_FFT, 0, FFT_THREAD_NUM);
    create_threads(Worker_ZF, FFT_THREAD_NUM, FFT_THREAD_NUM + ZF_THREAD_NUM);
    create_threads(Worker_Demul, FFT_THREAD_NUM + ZF_THREAD_NUM, TASK_THREAD_NUM);
#else
    create_threads(Worker, 0, TASK_THREAD_NUM);
#endif
    // stats_manager_.reset(new Stats(cfg_, 4, TASK_THREAD_NUM, FFT_THREAD_NUM, ZF_THREAD_NUM, DEMUL_THREAD_NUM));
    
}

Millipede::~Millipede()
{
    free_uplink_buffers();
    /* downlink */
    if (downlink_mode)
        free_downlink_buffers();
}

void Millipede::stop()
{
    std::cout << "stopping threads " << std::endl;
    cfg_->running = false;
    usleep(1000);
    receiver_.reset();
}

void Millipede::start()
{
    /* start uplink receiver */
    std::vector<pthread_t> rx_threads = receiver_->startRecv(socket_buffer_, 
        socket_buffer_status_, socket_buffer_status_size_, socket_buffer_size_, stats_manager_->frame_start);
#ifdef USE_ARGOS
    if (rx_threads.size() == 0) {
	this->stop();
	return;
    }
#endif

    /* start downlink transmitter */
    std::vector<pthread_t> tx_threads;
    if (downlink_mode) {
        std::vector<pthread_t> tx_threads = receiver_->startTX(dl_socket_buffer_, 
            dl_socket_buffer_status_, dl_socket_buffer_status_size_, dl_socket_buffer_size_);

#ifdef USE_ARGOS
        std::vector<std::vector<std::complex<float>>> calib_mat = receiver_->get_calib_mat();
        for (int i = 0; i < BS_ANT_NUM; i++) {
            for (int j = 0; j < OFDM_DATA_NUM; j++) {
                float re = calib_mat[i][j].real();
                float im = calib_mat[i][j].imag();
                recip_buffer_[j][i].re = re; //re/(re*re + im*im);
                recip_buffer_[j][i].im = im; //-im/(re*re + im*im);
            }
        }
#endif
    }

    /* tokens used for enqueue */
    /* uplink */
    moodycamel::ProducerToken ptok(fft_queue_);
    moodycamel::ProducerToken ptok_zf(zf_queue_);
    moodycamel::ProducerToken ptok_demul(demul_queue_);
    moodycamel::ProducerToken ptok_decode(decode_queue_);
    /* downlink */
    moodycamel::ProducerToken ptok_encode(encode_queue_);
    moodycamel::ProducerToken ptok_ifft(ifft_queue_);
    moodycamel::ProducerToken ptok_precode(precode_queue_);
    moodycamel::ProducerToken ptok_tx(tx_queue_);
    /* tokens used for dequeue */
    moodycamel::ConsumerToken ctok(message_queue_);
    moodycamel::ConsumerToken ctok_complete(complete_task_queue_);


    buffer_frame_num = subframe_num_perframe * BS_ANT_NUM * SOCKET_BUFFER_FRAME_NUM;
    

    #ifdef USE_LDPC
    prev_frame_counter = downlink_mode ? ifft_stats_.symbol_count : decode_stats_.symbol_count;
    #else
    prev_frame_counter = downlink_mode ? ifft_stats_.symbol_count : demul_stats_.symbol_count;
    #endif
    prev_frame_counter_max = downlink_mode ?  ifft_stats_.max_symbol_count : demul_stats_.max_symbol_count;

    /* counters for printing summary */
    int demul_count = 0;
    int tx_count = 0;
    double demul_begin = get_time();
    double tx_begin = get_time();
    
    bool prev_demul_scheduled = false;

    int last_dequeue = 0;
    int ret = 0;
    Event_data events_list[dequeue_bulk_size];
    int miss_count = 0;
    int total_count = 0;
    
    while (cfg_->running && !SignalHandler::gotExitSignal()) {
        /* get a bulk of events */
        if (last_dequeue == 0) {
// #ifdef USE_ARGOS
//             ret = message_queue_.try_dequeue_bulk(ctok, events_list, dequeue_bulk_size_single);
// #else
            ret = 0;
            for (int rx_itr = 0; rx_itr < SOCKET_RX_THREAD_NUM; rx_itr ++)             
                ret += message_queue_.try_dequeue_bulk_from_producer(*(rx_ptoks_ptr[rx_itr]), events_list + ret, dequeue_bulk_size_single);
// #endif
            last_dequeue = 1;
        }
        else {   
            ret = complete_task_queue_.try_dequeue_bulk(ctok_complete, events_list, dequeue_bulk_size_single);
            last_dequeue = 0;
        }
        total_count++;
        if(total_count == 1e9) {
            //printf("message dequeue miss rate %f\n", (float)miss_count / total_count);
            total_count = 0;
            miss_count = 0;
        }
        if(ret == 0) {
            miss_count++;
            continue;
        }

        /* handle each event */
        for(int bulk_count = 0; bulk_count < ret; bulk_count++) {
            Event_data& event = events_list[bulk_count];
            switch(event.event_type) {
                case EVENT_PACKET_RECEIVED: {         
                    int offset = event.data;    
                    int socket_thread_id, offset_in_current_buffer;
                    interpreteOffset2d_setbits(offset, &socket_thread_id, &offset_in_current_buffer, 28);                
                    char *socket_buffer_ptr = socket_buffer_[socket_thread_id] + (long long) offset_in_current_buffer * packet_length;
                    struct Packet *pkt = (struct Packet *)socket_buffer_ptr;
                    
                    int frame_id = pkt->frame_id % 10000;
                    int subframe_id = pkt->symbol_id;                                    
                    int ant_id = pkt->ant_id;
                    int frame_id_in_buffer = (frame_id % TASK_BUFFER_FRAME_NUM);
                    int prev_frame_id = (frame_id - 1) % TASK_BUFFER_FRAME_NUM;
                    
                    update_rx_counters(frame_id, frame_id_in_buffer, subframe_id, ant_id); 
                    #if BIGSTATION 
                    /* in BigStation, schedule FFT whenever a packet is received */
                    schedule_fft_task(offset, frame_id, frame_id_in_buffer, subframe_id, ant_id, prev_frame_id, ptok);
                    #else
                    bool previous_frame_done = prev_frame_counter[prev_frame_id] == prev_frame_counter_max;
                    /* if this is the first frame or the previous frame is all processed, schedule FFT for this packet */
                    if ((frame_id == 0 && fft_stats_.frame_count < 100) || (fft_stats_.frame_count > 0 && previous_frame_done)) {
                        schedule_fft_task(offset, frame_id, frame_id_in_buffer, subframe_id, ant_id, prev_frame_id, ptok);
                    }
                    else {
                        /* if the previous frame is not finished, store offset in queue */
                        delay_fft_queue[frame_id_in_buffer][delay_fft_queue_cnt[frame_id_in_buffer]] = offset;
                        delay_fft_queue_cnt[frame_id_in_buffer]++;
                    }
                    #endif                                            
                }                
                break;        
                case EVENT_FFT: {
                    int offset_fft = event.data;
                    int frame_id, subframe_id;
                    interpreteOffset2d(offset_fft, &frame_id, &subframe_id);
                    fft_stats_.task_count[frame_id][subframe_id]++;
                    if (fft_stats_.task_count[frame_id][subframe_id] == fft_stats_.max_task_count) {
                        fft_stats_.task_count[frame_id][subframe_id] = 0;
                        if (cfg_->isPilot(frame_id, subframe_id)) { 
                            fft_stats_.symbol_pilot_count[frame_id]++;  
                            print_per_subframe_done(PRINT_FFT_PILOTS, fft_stats_.frame_count, frame_id, subframe_id);
                            /* if csi of all UEs is ready, schedule ZF or prediction */
                            if (fft_stats_.symbol_pilot_count[frame_id] == fft_stats_.max_symbol_pilot_count) {
                                fft_stats_.symbol_pilot_count[frame_id] = 0;
                                stats_manager_->update_fft_processed(fft_stats_.frame_count); 
                                print_per_frame_done(PRINT_FFT_PILOTS, fft_stats_.frame_count, frame_id); 
                                update_frame_count(&(fft_stats_.frame_count));  
                                schedule_zf_task(frame_id, ptok_zf);
                            }
                        }
                        else if (cfg_->isUplink(frame_id, subframe_id)) {   
                            fft_stats_.data_exist_in_symbol[frame_id][subframe_id - PILOT_NUM] = true;
                            fft_stats_.symbol_data_count[frame_id]++;
                            print_per_subframe_done(PRINT_FFT_DATA, fft_stats_.frame_count - 1, frame_id, subframe_id); 
                            if (fft_stats_.symbol_data_count[frame_id] == fft_stats_.max_symbol_data_count) {      
                                print_per_frame_done(PRINT_FFT_DATA, fft_stats_.frame_count - 1, frame_id);                                
                                prev_demul_scheduled = false;
                            }  
                            /* if precoder exist, schedule demodulation */
                            if (zf_stats_.precoder_exist_in_frame[frame_id]) {
                                int start_sche_id = subframe_id;
                                if (!prev_demul_scheduled) {
                                    start_sche_id = PILOT_NUM;
                                    prev_demul_scheduled = true;
                                }
                                int end_sche_id = PILOT_NUM + fft_stats_.symbol_data_count[frame_id];
                                if (end_sche_id < subframe_id)
                                    end_sche_id = subframe_id + 1;
                                schedule_demul_task(frame_id, start_sche_id, end_sche_id, ptok_demul);
                            }                                       
                        }
                    }
                }
                break;          
                case EVENT_ZF: {
                    int offset_zf = event.data;
                    int frame_id, sc_id;
                    interpreteOffset2d(offset_zf, &frame_id, &sc_id);
                    zf_stats_.task_count[frame_id]++;
                    // print_per_task_done(PRINT_ZF, frame_id, 0, sc_id);
                    if (zf_stats_.task_count[frame_id] == zf_stats_.max_task_count) { 
                        stats_manager_->update_zf_processed(zf_stats_.frame_count);
                        zf_stats_.task_count[frame_id] = 0;
                        zf_stats_.precoder_exist_in_frame[frame_id] = true;
                        print_per_frame_done(PRINT_ZF, zf_stats_.frame_count, frame_id); 
                        update_frame_count(&(zf_stats_.frame_count));
                        /* if all the data in a frame has arrived when ZF is done */
                        if (fft_stats_.symbol_data_count[frame_id] == fft_stats_.max_symbol_data_count) 
                            schedule_demul_task(frame_id, PILOT_NUM, subframe_num_perframe, ptok_demul);   
                        if (downlink_mode) {
                            /* if downlink data transmission is enabled, schedule downlink encode/modulation for the first data subframe */
                            #ifdef USE_LDPC
                            schedule_encode_task(frame_id, dl_data_subframe_start, ptok_encode);
                            #else
                            schedule_precode_task(frame_id, dl_data_subframe_start, ptok_precode); 
                            #endif
                        }              
                    }
                }
                break;

                case EVENT_DEMUL: {
                    int offset_demul = event.data;                   
                    int frame_id, data_subframe_id, sc_id;
                    interpreteOffset3d(offset_demul, &frame_id, &data_subframe_id, &sc_id);
                    demul_stats_.task_count[frame_id][data_subframe_id]++;
                    print_per_task_done(PRINT_DEMUL, frame_id, data_subframe_id, sc_id);
                    /* if this subframe is ready */
                    if (demul_stats_.task_count[frame_id][data_subframe_id] == demul_stats_.max_task_count) {
                        max_equaled_frame = frame_id;
                        demul_stats_.task_count[frame_id][data_subframe_id] = 0;
                        demul_stats_.symbol_count[frame_id]++;
                        #ifdef USE_LDPC
                        schedule_decode_task(frame_id, data_subframe_id, ptok_decode);
                        #endif
                        print_per_subframe_done(PRINT_DEMUL, demul_stats_.frame_count, frame_id, data_subframe_id);
                        if (demul_stats_.symbol_count[frame_id] == demul_stats_.max_symbol_count) {
                            /* schedule fft for the next frame if there are delayed fft tasks */
                        #ifndef USE_LDPC
                            schedule_delayed_fft_tasks(demul_stats_.frame_count, frame_id, data_subframe_id, ptok);
                            #if BIGSTATION
                            demul_stats_.symbol_count[frame_id] = 0;
                            #endif  
                            stats_manager_->update_stats_in_functions_uplink(demul_stats_.frame_count);
                        #else
                            demul_stats_.symbol_count[frame_id] = 0;
                        #endif
                            stats_manager_->update_demul_processed(demul_stats_.frame_count);
                            zf_stats_.precoder_exist_in_frame[frame_id] = false;
                            fft_stats_.symbol_data_count[frame_id] = 0;
                            print_per_frame_done(PRINT_DEMUL, demul_stats_.frame_count, frame_id);
                            
                            update_frame_count(&demul_stats_.frame_count);                    
                        }
                        // save_demul_data_to_file(frame_id, data_subframe_id);
                        demul_count++;
                        if (demul_count == demul_stats_.max_symbol_count * 9000) {
                            demul_count = 0;
                            double diff = get_time() - demul_begin;
                            int samples_num_per_UE = OFDM_DATA_NUM * demul_stats_.max_symbol_count * 1000;
                            printf("Frame %d: Receive %d samples (per-client) from %d clients in %f secs, throughtput %f bps per-client (16QAM), current task queue length %zu\n", 
                                demul_stats_.frame_count, samples_num_per_UE, UE_NUM, diff, samples_num_per_UE * log2(16.0f) / diff, fft_queue_.size_approx());
                            demul_begin = get_time();
                        }                       
                    }
                }
                break;

                case EVENT_DECODE: {
                    int offset_demul = event.data;                   
                    int frame_id, data_subframe_id, cb_id;
                    interpreteOffset3d(offset_demul, &frame_id, &data_subframe_id, &cb_id);
                    decode_stats_.task_count[frame_id][data_subframe_id]++;
                    if (decode_stats_.task_count[frame_id][data_subframe_id] == decode_stats_.max_task_count) {
                        decode_stats_.task_count[frame_id][data_subframe_id] = 0;
                        decode_stats_.symbol_count[frame_id]++;
                        print_per_subframe_done(PRINT_DECODE, decode_stats_.frame_count, frame_id, data_subframe_id);
                        if (decode_stats_.symbol_count[frame_id] == decode_stats_.max_symbol_count) {
                            schedule_delayed_fft_tasks(decode_stats_.frame_count, frame_id, data_subframe_id, ptok);
                            #if BIGSTATION
                            prev_frame_counter[frame_id] = 0;
                            #endif  
                            stats_manager_->update_decode_processed(decode_stats_.frame_count);
                            print_per_frame_done(PRINT_DECODE, decode_stats_.frame_count, frame_id);
                            stats_manager_->update_stats_in_functions_uplink(decode_stats_.frame_count);
                            update_frame_count(&decode_stats_.frame_count);  
                        }
                    }
                }
                break;

                case EVENT_ENCODE: {
                    int offset_demul = event.data;                   
                    int frame_id, data_subframe_id, cb_id;
                    interpreteOffset3d(offset_demul, &frame_id, &data_subframe_id, &cb_id);
                    encode_stats_.task_count[frame_id][data_subframe_id]++;
                    if (encode_stats_.task_count[frame_id][data_subframe_id] == encode_stats_.max_task_count) {
                        schedule_precode_task(frame_id, data_subframe_id, ptok_precode); 
                        encode_stats_.task_count[frame_id][data_subframe_id] = 0;
                        encode_stats_.symbol_count[frame_id]++;
                        print_per_subframe_done(PRINT_ENCODE, encode_stats_.frame_count, frame_id, data_subframe_id);
                        if (encode_stats_.symbol_count[frame_id] == encode_stats_.max_symbol_count) {          
                            stats_manager_->update_encode_processed(encode_stats_.frame_count);
                            print_per_frame_done(PRINT_ENCODE, encode_stats_.frame_count, frame_id);
                            update_frame_count(&encode_stats_.frame_count);  
                        }         
                    }
                }
                break;

                case EVENT_PRECODE: {
                    /* Precoding is done, schedule ifft */
                    int offset_precode = event.data;
                    int frame_id, data_subframe_id, sc_id;
                    interpreteOffset3d(offset_precode, &frame_id, &data_subframe_id, &sc_id);

                    precode_stats_.task_count[frame_id][data_subframe_id]++;
                    print_per_task_done(PRINT_PRECODE, frame_id, data_subframe_id, sc_id);          
                    if (precode_stats_.task_count[frame_id][data_subframe_id] == precode_stats_.max_task_count) {
                        schedule_ifft_task(precode_stats_.frame_count, data_subframe_id, ptok_ifft);
                        if (data_subframe_id < dl_data_subframe_end - 1) {
                            #ifdef USE_LDPC
                            schedule_encode_task(frame_id, data_subframe_id + 1, ptok_encode);
                            #else
                            schedule_precode_task(frame_id, data_subframe_id + 1, ptok_precode); 
                            #endif
                        }

                        precode_stats_.task_count[frame_id][data_subframe_id] = 0;    
                        print_per_subframe_done(PRINT_PRECODE, precode_stats_.frame_count, frame_id, data_subframe_id);                   
                        precode_stats_.symbol_count[frame_id]++;
                        if (precode_stats_.symbol_count[frame_id] == precode_stats_.max_symbol_count) {
                            precode_stats_.symbol_count[frame_id] = 0;
                            stats_manager_->update_precode_processed(precode_stats_.frame_count);    
                            print_per_frame_done(PRINT_PRECODE, precode_stats_.frame_count, frame_id);       
                            update_frame_count(&precode_stats_.frame_count);                       
                        }
                    }
                }
                break;
                case EVENT_IFFT: {
                    /* IFFT is done, schedule data transmission */
                    int offset_ifft = event.data;
                    int frame_id, data_subframe_id, ant_id;
                    interpreteOffset3d(offset_ifft, &data_subframe_id, &ant_id, &frame_id);
 
                    Event_data do_tx_task;
                    do_tx_task.event_type = TASK_SEND;
                    do_tx_task.data = offset_ifft;      
                    int ptok_id = ant_id % SOCKET_RX_THREAD_NUM;          
                    schedule_task(do_tx_task, &tx_queue_, *tx_ptoks_ptr[ptok_id]);

                    frame_id = frame_id % TASK_BUFFER_FRAME_NUM;
                    print_per_task_done(PRINT_IFFT, frame_id, data_subframe_id, ant_id);
                    ifft_stats_.task_count[frame_id][data_subframe_id]++;
                    if (ifft_stats_.task_count[frame_id][data_subframe_id] == ifft_stats_.max_task_count) {
                        ifft_stats_.task_count[frame_id][data_subframe_id] = 0;
                        ifft_stats_.symbol_count[frame_id]++;
                        if (ifft_stats_.symbol_count[frame_id] == ifft_stats_.max_symbol_count) {
                            /* schedule fft for next frame */
                            schedule_delayed_fft_tasks(ifft_stats_.frame_count, frame_id, data_subframe_id, ptok);
                            stats_manager_->update_ifft_processed(ifft_stats_.frame_count);
                            print_per_frame_done(PRINT_IFFT, ifft_stats_.frame_count, frame_id);      
                            update_frame_count(&ifft_stats_.frame_count);
                        }
                    }
                }
                break;
                case EVENT_PACKET_SENT: {
                    /* Data is sent */
                    int offset_tx = event.data;
                    int frame_id, data_subframe_id, ant_id;
                    interpreteOffset3d(offset_tx, &data_subframe_id, &ant_id, &frame_id);
                    // printf("In main thread: tx finished for frame %d subframe %d ant %d\n", frame_id, data_subframe_id, ant_id);
                    frame_id = frame_id % TASK_BUFFER_FRAME_NUM;

                    print_per_task_done(PRINT_TX, frame_id, data_subframe_id, ant_id);
                    tx_stats_.task_count[frame_id][data_subframe_id]++;
                    if (tx_stats_.task_count[frame_id][data_subframe_id] == tx_stats_.max_task_count) {
                        tx_stats_.task_count[frame_id][data_subframe_id] = 0;
                        print_per_subframe_done(PRINT_TX, tx_stats_.frame_count, frame_id, data_subframe_id);
                        /* if tx of the first symbol is done */
                        if (data_subframe_id == dl_data_subframe_start) {
                            stats_manager_->update_tx_processed_first(tx_stats_.frame_count);
                            print_per_frame_done(PRINT_TX_FIRST, tx_stats_.frame_count, frame_id);
                        }
                        tx_stats_.symbol_count[frame_id]++;       
                        if (tx_stats_.symbol_count[frame_id] == tx_stats_.max_symbol_count) {
                            tx_stats_.symbol_count[frame_id] = 0; 
                            stats_manager_->update_tx_processed(tx_stats_.frame_count);    
                            print_per_frame_done(PRINT_TX, tx_stats_.frame_count, frame_id);                       
                            stats_manager_->update_stats_in_functions_downlink(tx_stats_.frame_count);
                            update_frame_count(&tx_stats_.frame_count); 
                        }
                        tx_count++;
                        if (tx_count == tx_stats_.max_symbol_count * 9000) {
                            tx_count = 0;
                            double diff = get_time() - tx_begin;
                            int samples_num_per_UE = OFDM_DATA_NUM * tx_stats_.max_symbol_count * 1000;
                            printf("Transmit %d samples (per-client) to %d clients in %f secs, throughtput %f bps per-client (16QAM), current tx queue length %zu\n", 
                                samples_num_per_UE, UE_NUM, diff, samples_num_per_UE * log2(16.0f) / diff, tx_queue_.size_approx());
                            tx_begin = get_time();
                        }
                    }         
                }
                break;
                default:
                    printf("Wrong event type in message queue!");
                    exit(0);
            } /* end of switch */
        } /* end of for */
    } /* end of while */
    this->stop();
    printf("Total dequeue trials: %d, missed %d\n", total_count, miss_count);
    int last_frame_id = downlink_mode ? tx_stats_.frame_count : demul_stats_.frame_count;
    stats_manager_->save_to_file(last_frame_id, SOCKET_RX_THREAD_NUM);
    stats_manager_->print_summary(last_frame_id);
    //exit(0);
}




void *Millipede::worker(int tid) 
{
    int core_offset = SOCKET_RX_THREAD_NUM + CORE_OFFSET + 1;
    pin_to_core_with_offset(Worker, core_offset, tid);
    moodycamel::ProducerToken *task_ptok_ptr = task_ptoks_ptr[tid];

    /* initialize operators */
    DoFFT *computeFFT = new DoFFT(cfg_, tid, transpose_block_size, &complete_task_queue_, task_ptok_ptr,
        socket_buffer_, socket_buffer_status_, data_buffer_, csi_buffer_, pilots_,
        dl_ifft_buffer_, dl_socket_buffer_, stats_manager_);

    DoZF *computeZF = new DoZF(cfg_, tid, zf_block_size, transpose_block_size, &complete_task_queue_, task_ptok_ptr,
        csi_buffer_, precoder_buffer_, dl_precoder_buffer_, recip_buffer_,  pred_csi_buffer_, stats_manager_);

    DoDemul *computeDemul = new DoDemul(cfg_, tid, demul_block_size, transpose_block_size, &(complete_task_queue_), task_ptok_ptr,
        data_buffer_, precoder_buffer_, equal_buffer_, demod_hard_buffer_, demod_soft_buffer_, stats_manager_);

    DoPrecode *computePrecode = new DoPrecode(cfg_, tid, demul_block_size, transpose_block_size, &(complete_task_queue_), task_ptok_ptr,
        dl_modulated_buffer_, dl_precoder_buffer_, dl_precoded_data_buffer_, dl_ifft_buffer_, dl_IQ_data, dl_encoded_buffer_,
        stats_manager_);

    #ifdef USE_LDPC
    DoCoding *computeCoding = new DoCoding(cfg_, tid, &(complete_task_queue_), task_ptok_ptr,
        dl_IQ_data, dl_encoded_buffer_, demod_soft_buffer_, decoded_buffer_, 
        stats_manager_);
    #endif

    Event_data event;
    bool ret = false;

    int queue_num;
    int *dequeue_order;
    int dequeue_order_DL_LDPC[] = {TASK_IFFT, TASK_PRECODE, TASK_ENCODE, TASK_ZF, TASK_FFT};
    int dequeue_order_UL_LDPC[] = {TASK_ZF, TASK_FFT, TASK_DEMUL, TASK_DECODE};
    int dequeue_order_DL[] = {TASK_IFFT, TASK_PRECODE, TASK_ZF, TASK_FFT};
    int dequeue_order_UL[] = {TASK_ZF, TASK_FFT, TASK_DEMUL};

#ifdef USE_LDPC
    if (downlink_mode) {
        queue_num = sizeof(dequeue_order_DL_LDPC) / sizeof(dequeue_order_DL_LDPC[0]);
        dequeue_order = dequeue_order_DL_LDPC;
    }
    else {
        queue_num = sizeof(dequeue_order_UL_LDPC) / sizeof(dequeue_order_UL_LDPC[0]);;
        dequeue_order = dequeue_order_UL_LDPC;
    }
#else
    if (downlink_mode) {
        queue_num = sizeof(dequeue_order_DL) / sizeof(dequeue_order_DL[0]);;
        dequeue_order = dequeue_order_DL;
    }
    else {
        queue_num = sizeof(dequeue_order_UL) / sizeof(dequeue_order_UL[0]);
        dequeue_order = dequeue_order_UL;
    }
#endif

    int dequeue_idx = 0;

    while(true) {
        switch(dequeue_order[dequeue_idx]) {
            case TASK_IFFT: 
                if (ret = ifft_queue_.try_dequeue(event)) 
                    computeFFT->IFFT(event.data);
            break;
            case TASK_PRECODE: 
                if (ret = precode_queue_.try_dequeue(event)) 
                    computePrecode->Precode(event.data);
            break;
            #ifdef USE_LDPC
            case TASK_ENCODE: 
                if (ret = encode_queue_.try_dequeue(event)) {
                    computeCoding->Encode(event.data);
                }
            break;
            case TASK_DECODE: 
                if (ret = decode_queue_.try_dequeue(event)) 
                    computeCoding->Decode(event.data);
            break;
            #endif
            case TASK_ZF: 
                if (ret = zf_queue_.try_dequeue(event)) 
                    computeZF->ZF(event.data);
            break;
            case TASK_FFT: 
                if (ret = fft_queue_.try_dequeue(event)) 
                    computeFFT->FFT(event.data);
            break;
            case TASK_DEMUL: 
                if (ret = demul_queue_.try_dequeue(event)) 
                    computeDemul->Demul(event.data);
            break;
            default: 
                printf("ERROR: unsupported task type in dequeue\n");
                exit(0);
        }
        if (ret) 
            dequeue_idx = 0;
        else
            dequeue_idx = (dequeue_idx + 1 ) % queue_num;
    }
    

}



void* Millipede::worker_fft(int tid)
{
    int core_offset = SOCKET_RX_THREAD_NUM + CORE_OFFSET + 1;
    pin_to_core_with_offset(Worker_FFT, core_offset, tid);
    moodycamel::ProducerToken *task_ptok_ptr = task_ptoks_ptr[tid];

    /* initialize FFT operator */
    DoFFT* computeFFT = new DoFFT(cfg_, tid, transpose_block_size, &(complete_task_queue_), task_ptok_ptr,
        socket_buffer_, socket_buffer_status_, data_buffer_, csi_buffer_, pilots_,
        dl_ifft_buffer_, dl_socket_buffer_, stats_manager_);


    Event_data event;
    bool ret = false;

    while(true) {
        ret = fft_queue_.try_dequeue(event);
        if (!ret) {
            if (downlink_mode)
            {
                ret = ifft_queue_.try_dequeue(event);
                if (!ret)
                    continue;
                else
                    computeFFT->IFFT(event.data);
            }
            else
                continue;
        }
        else
            computeFFT->FFT(event.data);
    }

}



void* Millipede::worker_zf(int tid)
{
    
    int core_offset = SOCKET_RX_THREAD_NUM + CORE_OFFSET + 1;
    pin_to_core_with_offset(Worker_ZF, core_offset, tid);

    moodycamel::ProducerToken *task_ptok_ptr = task_ptoks_ptr[tid];

    /* initialize ZF operator */
    DoZF *computeZF = new DoZF(cfg_, tid, zf_block_size, transpose_block_size, &(complete_task_queue_), task_ptok_ptr,
        csi_buffer_, precoder_buffer_, dl_precoder_buffer_, recip_buffer_,  pred_csi_buffer_, stats_manager_);


    Event_data event;
    bool ret_zf = false;

    while(true) {
        ret_zf = zf_queue_.try_dequeue(event);
        if (!ret_zf) 
            continue;
        else
            computeZF->ZF(event.data);
    }

}

void* Millipede::worker_demul(int tid)
{
    
    int core_offset = SOCKET_RX_THREAD_NUM + CORE_OFFSET + 1;
    pin_to_core_with_offset(Worker_Demul, core_offset, tid);
    moodycamel::ProducerToken *task_ptok_ptr = task_ptoks_ptr[tid];

    /* initialize Demul operator */
    DoDemul *computeDemul = new DoDemul(cfg_, tid, demul_block_size, transpose_block_size, &(complete_task_queue_), task_ptok_ptr,
        data_buffer_, precoder_buffer_, equal_buffer_, demod_hard_buffer_, demod_soft_buffer_, stats_manager_);


    /* initialize Precode operator */
    DoPrecode *computePrecode = new DoPrecode(cfg_, tid, demul_block_size, transpose_block_size, &(complete_task_queue_), task_ptok_ptr,
        dl_modulated_buffer_, dl_precoder_buffer_, dl_precoded_data_buffer_, dl_ifft_buffer_, dl_IQ_data, dl_encoded_buffer_,
        stats_manager_);



    Event_data event;
    bool ret_demul = false;
    bool ret_precode = false;
    // int cur_frame_id = 0;

    while(true) {         
        if (downlink_mode)
        {
            ret_precode = precode_queue_.try_dequeue(event);
            if (!ret_precode)
                continue;
            else
                computePrecode->Precode(event.data);
        }
        else
        {
            ret_demul = demul_queue_.try_dequeue(event);
            if (!ret_demul) {
                continue;
            }
            else {
                // int frame_id = event.data / (OFDM_CA_NUM * ul_data_subframe_num_perframe);
                // // check precoder status for the current frame
                // if (frame_id > cur_frame_id || frame_id == 0) {
                //     while (!precoder_status_[frame_id]);
                // }
                computeDemul->Demul(event.data);
            }
        }
    }

}

void Millipede::create_threads(thread_type thread, int tid_start, int tid_end)
{
    int ret;
    for(int i = tid_start; i < tid_end; i++) {
        context[i].obj_ptr = this;
        context[i].id = i;
        switch(thread) {
            case Worker:
                ret = pthread_create(&task_threads[i], NULL, pthread_fun_wrapper<Millipede, &Millipede::worker>, &context[i]);
                break;
            case Worker_FFT:
                ret = pthread_create(&task_threads[i], NULL, pthread_fun_wrapper<Millipede, &Millipede::worker_fft>, &context[i]);
                break;
            case Worker_ZF:
                ret = pthread_create(&task_threads[i], NULL, pthread_fun_wrapper<Millipede, &Millipede::worker_zf>, &context[i]);
                break;
            case Worker_Demul:
                ret = pthread_create(&task_threads[i], NULL, pthread_fun_wrapper<Millipede, &Millipede::worker_demul>, &context[i]);
                break;
            default:
                printf("ERROR: Wrong thread type to create workers\n");
                exit(0);
        }
        if (ret != 0) {
            perror("task thread create failed");
            exit(0);
        }
    }
}


inline void Millipede::update_frame_count(int *frame_count)
{
    *frame_count = *frame_count + 1;
    if(*frame_count == 1e9)
        *frame_count = 0;
}

void Millipede::schedule_task(Event_data do_task, moodycamel::ConcurrentQueue<Event_data> * in_queue, moodycamel::ProducerToken const& ptok) 
{
    if ( !in_queue->try_enqueue(ptok, do_task ) ) {
        printf("need more memory\n");
        if ( !in_queue->enqueue(ptok, do_task ) ) {
            printf("task enqueue failed\n");
            exit(0);
        }
    }
}

void Millipede::schedule_fft_task(int offset, int frame_id, int frame_id_in_buffer, int subframe_id, int ant_id, int prev_frame_id,
    moodycamel::ProducerToken const& ptok) 
{
    Event_data do_fft_task;
    do_fft_task.event_type = TASK_FFT;
    do_fft_task.data = offset;
    schedule_task(do_fft_task, &fft_queue_, ptok);
#if DEBUG_PRINT_PER_TASK_ENTER_QUEUE
    printf("Main thread: created FFT tasks for frame: %d, frame buffer: %d, subframe: %d, ant: %d\n", 
            frame_id, frame_id_in_buffer, subframe_id, ant_id);
#endif     
    rx_stats_.fft_created_count[frame_id_in_buffer]++;
    if (rx_stats_.fft_created_count[frame_id_in_buffer] == 1) {
        stats_manager_->update_processing_started(frame_id);
    }
    else if (rx_stats_.fft_created_count[frame_id_in_buffer] == rx_stats_.max_task_count) {
        rx_stats_.fft_created_count[frame_id_in_buffer] = 0;
#if DEBUG_PRINT_PER_FRAME_ENTER_QUEUE                            
        printf("Main thread: created FFT tasks for all packets in frame: %d, frame buffer: %d in %.5f us\n", 
                frame_id, frame_id_in_buffer, get_time()-stats_manager_->get_pilot_received(frame_id));
#endif  
#if !BIGSTATION
        prev_frame_counter[prev_frame_id] = 0;
#endif
    }        
}

void Millipede::schedule_delayed_fft_tasks(int frame_id, int frame_id_in_buffer, int data_subframe_id, moodycamel::ProducerToken const& ptok)
{
    int frame_id_next = (frame_id_in_buffer + 1) % TASK_BUFFER_FRAME_NUM;
    if (delay_fft_queue_cnt[frame_id_next] > 0) {
        for (int i = 0; i < delay_fft_queue_cnt[frame_id_next]; i++) {
            int offset_rx = delay_fft_queue[frame_id_next][i];
            schedule_fft_task(offset_rx, frame_id + 1, frame_id_next, data_subframe_id + UE_NUM, 0, frame_id_in_buffer, ptok);
        }
        delay_fft_queue_cnt[frame_id_next] = 0;
#if DEBUG_PRINT_PER_FRAME_ENTER_QUEUE 
    if (downlink_mode)
        printf("Main thread in IFFT: schedule fft for %d packets for frame %d is done\n", delay_fft_queue_cnt[frame_id_next], frame_id_next);
    else
        printf("Main thread in demul: schedule fft for %d packets for frame %d is done\n", delay_fft_queue_cnt[frame_id_next], frame_id_next);
#endif                                
    } 
}


void Millipede::schedule_zf_task(int frame_id, moodycamel::ProducerToken const& ptok_zf) 
{
    /* schedule normal ZF for all data subcarriers */                                                                                      
    Event_data do_zf_task;
    do_zf_task.event_type = TASK_ZF;
    for (int i = 0; i < zf_block_num; i++) {
        do_zf_task.data = generateOffset2d(frame_id, i * zf_block_size);
        schedule_task(do_zf_task, &zf_queue_, ptok_zf);
    }
#if DEBUG_PRINT_PER_FRAME_ENTER_QUEUE
    printf("Main thread: created ZF tasks for frame: %d\n", frame_id);
#endif
}


void Millipede::schedule_demul_task(int frame_id, int start_sche_id, int end_sche_id, moodycamel::ProducerToken const& ptok_demul) 
{
    for (int sche_subframe_id = start_sche_id; sche_subframe_id < end_sche_id; sche_subframe_id++) {
        int data_subframe_id = (sche_subframe_id - PILOT_NUM);
        if (fft_stats_.data_exist_in_symbol[frame_id][data_subframe_id]) {
            Event_data do_demul_task;
            do_demul_task.event_type = TASK_DEMUL;
            /* schedule demodulation task for subcarrier blocks */
            for(int i = 0; i < demul_block_num; i++) {
                do_demul_task.data = generateOffset3d(frame_id, data_subframe_id, i * demul_block_size);
                schedule_task(do_demul_task, &demul_queue_, ptok_demul);
            }
#if DEBUG_PRINT_PER_SUBFRAME_ENTER_QUEUE
            printf("Main thread: created Demodulation task for frame: %d,, start subframe: %d, current subframe: %d\n", 
                    frame_id, start_sche_id, data_subframe_id);
#endif                                         
            /* clear data status after scheduling */
            fft_stats_.data_exist_in_symbol[frame_id][data_subframe_id] = false;
        }
    }
}


void Millipede::schedule_decode_task(int frame_id, int data_subframe_id, moodycamel::ProducerToken const& ptok_decode) 
{
    Event_data do_decode_task;
    do_decode_task.event_type = TASK_DECODE;
    for(int i = 0; i < UE_NUM; i++) {
        for(int j = 0; j < LDPC_config.nblocksInSymbol; j++) {
            do_decode_task.data = generateOffset3d(frame_id, data_subframe_id, i * LDPC_config.nblocksInSymbol + j);
            schedule_task(do_decode_task, &decode_queue_, ptok_decode);
        }
    }
}


void Millipede::schedule_encode_task(int frame_id, int data_subframe_id, moodycamel::ProducerToken const& ptok_encode) 
{
    Event_data do_encode_task;
    do_encode_task.event_type = TASK_ENCODE;
    for(int i = 0; i < UE_NUM; i++) {
        for(int j = 0; j < LDPC_config.nblocksInSymbol; j++) {
            do_encode_task.data = generateOffset3d(frame_id, data_subframe_id, i * LDPC_config.nblocksInSymbol + j);
            schedule_task(do_encode_task, &encode_queue_, ptok_encode);
        }
    }
}

void Millipede::schedule_precode_task(int frame_id, int data_subframe_id, moodycamel::ProducerToken const& ptok_precode) 
{
    Event_data do_precode_task;
    do_precode_task.event_type = TASK_PRECODE;
    for(int j = 0; j < demul_block_num; j++) {
        do_precode_task.data = generateOffset3d(frame_id, data_subframe_id, j * demul_block_size);
        schedule_task(do_precode_task, &precode_queue_, ptok_precode);
    }
}

void Millipede::schedule_ifft_task(int frame_id, int data_subframe_id, moodycamel::ProducerToken const& ptok_ifft) 
{
    Event_data do_ifft_task;
    do_ifft_task.event_type = TASK_IFFT;
    for (int i = 0; i < BS_ANT_NUM; i++) {
        do_ifft_task.data = generateOffset3d(data_subframe_id, i, frame_id);
        schedule_task(do_ifft_task, &ifft_queue_, ptok_ifft);
    }
}

void Millipede::update_rx_counters(int frame_id, int frame_id_in_buffer, int subframe_id, int ant_id)
{
    int prev_frame_id = (frame_id - 1) % TASK_BUFFER_FRAME_NUM;
    if (cfg_->isPilot(frame_id, subframe_id)) { 
        rx_stats_.task_pilot_count[frame_id_in_buffer]++;
        if(rx_stats_.task_pilot_count[frame_id_in_buffer] == rx_stats_.max_task_pilot_count) {
            rx_stats_.task_pilot_count[frame_id_in_buffer] = 0;
            stats_manager_->update_pilot_all_received(frame_id);
            print_per_frame_done(PRINT_RX_PILOTS, frame_id, frame_id_in_buffer);    
        }
    }
    rx_stats_.task_count[frame_id_in_buffer]++;
    if (rx_stats_.task_count[frame_id_in_buffer] == 1) {   
        stats_manager_->update_pilot_received(frame_id);
#if DEBUG_PRINT_PER_FRAME_START 
        printf("Main thread: data received from frame %d, subframe %d, ant %d, in %.5f us, rx in prev frame: %d\n", \
                frame_id, subframe_id, ant_id, stats_manager_->get_pilot_received(frame_id) - stats_manager_->get_pilot_received(frame_id-1),
                rx_stats_.task_count[prev_frame_id]);
#endif
    }
    else if (rx_stats_.task_count[frame_id_in_buffer] == rx_stats_.max_task_count) {  
        stats_manager_->update_rx_processed(frame_id);
        print_per_frame_done(PRINT_RX, frame_id, frame_id_in_buffer);                        
        rx_stats_.task_count[frame_id_in_buffer] = 0;                                  
    } 
}


void Millipede::print_per_frame_done(int task_type, int frame_id, int frame_id_in_buffer)
{
#if DEBUG_PRINT_PER_FRAME_DONE
    switch(task_type) {
        case(PRINT_RX): {
            int prev_frame_id = (frame_id - 1) % TASK_BUFFER_FRAME_NUM;
            printf("Main thread: received all packets in frame: %d, frame buffer: %d in %.2f us, demul: %d done, FFT: %d,%d, rx in prev frame: %d\n", 
                frame_id, frame_id_in_buffer, stats_manager_->get_rx_processed(frame_id) - stats_manager_->get_pilot_received(frame_id),
                demul_stats_.symbol_count[frame_id_in_buffer], fft_stats_.symbol_data_count[frame_id_in_buffer], 
                fft_stats_.task_count[frame_id_in_buffer][fft_stats_.symbol_data_count[frame_id_in_buffer] + UE_NUM], 
                rx_stats_.task_count[prev_frame_id]);
            }
            break;
        case(PRINT_RX_PILOTS):
            printf("Main thread: received all pilots in frame: %d, frame buffer: %d in %.2f us\n", frame_id, frame_id_in_buffer, 
                stats_manager_->get_pilot_all_received(frame_id) - stats_manager_->get_pilot_received(frame_id));
            break;
        case(PRINT_FFT_PILOTS):
            printf("Main thread: pilot frame: %d, %d, finished FFT for all pilot subframes in %.2f us, pilot all received: %.2f\n", 
                frame_id, frame_id_in_buffer,
                stats_manager_->get_fft_processed(frame_id) - stats_manager_->get_pilot_received(frame_id),
                stats_manager_->get_pilot_all_received(frame_id) - stats_manager_->get_pilot_received(frame_id));
            break;
        case(PRINT_FFT_DATA):
            printf("Main thread: data frame: %d, %d, finished FFT for all data subframes in %.2f us\n", 
                frame_id, frame_id_in_buffer, 
                get_time()-stats_manager_->get_pilot_received(frame_id));
            break;
        case(PRINT_ZF):
            printf("Main thread: ZF done frame: %d, %d in %.2f us since pilot FFT done, total: %.2f us\n", 
                frame_id, frame_id_in_buffer, 
                stats_manager_->get_zf_processed(frame_id) - stats_manager_->get_fft_processed(frame_id),
                stats_manager_->get_zf_processed(frame_id) - stats_manager_->get_pilot_received(frame_id));
            break;
        case(PRINT_DEMUL):
            printf("Main thread: Demodulation done frame: %d, %d (%d UL subframes) in %.2f us since ZF done, total %.2f us\n",
                frame_id, frame_id_in_buffer, ul_data_subframe_num_perframe,
                stats_manager_->get_demul_processed(frame_id) - stats_manager_->get_zf_processed(frame_id),
                stats_manager_->get_demul_processed(frame_id) - stats_manager_->get_pilot_received(frame_id));
            break;
        case(PRINT_DECODE):
            printf("Main thread: Decoding done frame: %d, %d (%d UL subframes) in %.2f us since ZF done, total %.2f us\n",
                frame_id, frame_id_in_buffer, ul_data_subframe_num_perframe,
                stats_manager_->get_decode_processed(frame_id) - stats_manager_->get_zf_processed(frame_id),
                stats_manager_->get_decode_processed(frame_id) - stats_manager_->get_pilot_received(frame_id));
            break;
        case(PRINT_ENCODE):
            printf("Main thread: Encoding done frame: %d, %d in %.2f us since ZF done, total %.2f us\n",
                frame_id, frame_id_in_buffer, 
                stats_manager_->get_encode_processed(frame_id) - stats_manager_->get_zf_processed(frame_id),
                stats_manager_->get_encode_processed(frame_id) - stats_manager_->get_pilot_received(frame_id));
            break;
        case(PRINT_PRECODE):
            printf("Main thread: Precoding done frame: %d, %d in %.2f us since ZF done, total: %.2f us\n", 
                frame_id, frame_id_in_buffer, 
                stats_manager_->get_precode_processed(frame_id) - stats_manager_->get_zf_processed(frame_id),
                stats_manager_->get_precode_processed(frame_id) - stats_manager_->get_pilot_received(frame_id)); 
            break;
        case(PRINT_IFFT):
            printf("Main thread: IFFT done frame: %d, %d in %.2f us since precode done, total: %.2f us\n", 
                frame_id, frame_id_in_buffer,
                stats_manager_->get_ifft_processed(frame_id) - stats_manager_->get_precode_processed(frame_id),
                stats_manager_->get_ifft_processed(frame_id) - stats_manager_->get_pilot_received(frame_id));
            break;
        case(PRINT_TX_FIRST):
            printf("Main thread: TX of first subframe done frame: %d, %d in %.2f us since ZF done, total: %.2f us\n", 
                frame_id, frame_id_in_buffer, 
                stats_manager_->get_tx_processed_first(frame_id) - stats_manager_->get_zf_processed(frame_id),
                stats_manager_->get_tx_processed_first(frame_id) - stats_manager_->get_pilot_received(frame_id));
            break;
        case(PRINT_TX):
            printf("Main thread: TX done frame: %d %d (%d DL subframes) in %.2f us since ZF done, total: %.2f us\n", 
                frame_id, frame_id_in_buffer, dl_data_subframe_num_perframe,
                stats_manager_->get_tx_processed(frame_id) - stats_manager_->get_zf_processed(frame_id),
                stats_manager_->get_tx_processed(frame_id) - stats_manager_->get_pilot_received(frame_id));
            break;
        default:
            printf("Wrong task type in frame done print!");
    }
#endif
}

void Millipede::print_per_subframe_done(int task_type, int frame_id, int frame_id_in_buffer, int subframe_id)
{
#if DEBUG_PRINT_PER_SUBFRAME_DONE
    switch(task_type) {
        case(PRINT_FFT_PILOTS):
            printf("Main thread: pilot FFT done frame: %d, %d, subframe: %d, num sumbframes done: %d\n", 
                frame_id, frame_id_in_buffer, subframe_id, fft_stats_.symbol_pilot_count[frame_id_in_buffer]);
            break;
        case(PRINT_FFT_DATA):
            printf("Main thread: data FFT done frame %d, %d, subframe %d, precoder status: %d, fft queue: %d, zf queue: %d, demul queue: %d, in %.2f\n", 
                frame_id, frame_id_in_buffer, subframe_id, zf_stats_.precoder_exist_in_frame[frame_id_in_buffer], 
                fft_queue_.size_approx(), zf_queue_.size_approx(), demul_queue_.size_approx(),
                get_time()-stats_manager_->get_pilot_received(frame_id));
            break;
        case(PRINT_DEMUL):
            printf("Main thread: Demodulation done frame %d %d, subframe: %d, num sumbframes done: %d\n", 
                frame_id, frame_id_in_buffer, subframe_id, demul_stats_.symbol_count[frame_id_in_buffer]);
            break;
        case(PRINT_DECODE):
            printf("Main thread: Decoding done frame %d %d, subframe: %d, num sumbframes done: %d\n", 
                frame_id, frame_id_in_buffer, subframe_id, decode_stats_.symbol_count[frame_id_in_buffer]);
            break;
        case(PRINT_ENCODE):
            printf("Main thread: Encoding done frame %d %d, subframe: %d, num sumbframes done: %d\n", 
                frame_id, frame_id_in_buffer, subframe_id, encode_stats_.symbol_count[frame_id_in_buffer]);
            break;
        case(PRINT_PRECODE):
            printf("Main thread: Precoding done frame: %d %d, subframe: %d in %.2f us\n", 
                frame_id, frame_id_in_buffer, subframe_id,
                get_time()-stats_manager_->get_pilot_received(frame_id));
            break;
        case(PRINT_TX):
            printf("Main thread: TX done frame: %d %d, subframe: %d in %.2f us\n", 
                frame_id, frame_id_in_buffer, subframe_id,
                get_time()-stats_manager_->get_pilot_received(frame_id));
            break;
        default:
            printf("Wrong task type in frame done print!");
    }
#endif
}

void Millipede::print_per_task_done(int task_type, int frame_id, int subframe_id, int ant_or_sc_id) 
{
#if DEBUG_PRINT_PER_TASK_DONE
    switch(task_type) {
        case(PRINT_ZF):
            printf("Main thread: ZF done frame: %d, subcarrier %d\n", frame_id, ant_or_sc_id);
            break;
        case(PRINT_DEMUL):
            printf("Main thread: Demodulation done frame: %d, subframe: %d, sc: %d, num blocks done: %d\n", 
                frame_id, subframe_id, ant_or_sc_id, demul_stats_.task_count[frame_id][subframe_id]);
            break;
        case(PRINT_DEMUL):
            printf("Main thread: Decoding done frame: %d, subframe: %d, sc: %d, num blocks done: %d\n", 
                frame_id, subframe_id, ant_or_sc_id, decode_stats.task_count[frame_id][subframe_id]);
            break;
        case(PRINT_PRECODE):
            printf("Main thread: Precoding done frame: %d, subframe: %d, subcarrier: %d, total SCs: %d\n", 
                frame_id, subframe_id, ant_or_sc_id, precode_stats_.task_count[frame_id][subframe_id]);
            break;
        case(PRINT_IFFT):
            printf("Main thread: IFFT done frame: %d, subframe: %d, antenna: %d, total ants: %d\n", 
                frame_id, subframe_id, ant_or_sc_id, ifft_stats_.task_count[frame_id]);
            break;
        case(PRINT_TX):
            printf("Main thread: TX done frame: %d, subframe: %d, antenna: %d, total packets: %d\n", 
                frame_id, subframe_id, ant_or_sc_id, tx_stats_.task_count[frame_id][subframe_id]);
            break;
        default:
            printf("Wrong task type in frame done print!");
    }

#endif
}


void Millipede::initialize_vars_from_cfg(Config *cfg)
{
    pilots_ = cfg->pilots_;
    dl_IQ_data = cfg->dl_IQ_data;

#if DEBUG_PRINT_PILOT
    cout<<"Pilot data"<<endl;
    for (int i = 0; i < OFDM_CA_NUM; i++) 
        cout<<pilots_[i]<<",";
    cout<<endl;
#endif

    BS_ANT_NUM = cfg->BS_ANT_NUM;
    UE_NUM = cfg->UE_NUM;
    PILOT_NUM = cfg->pilot_symbol_num_perframe;
    OFDM_CA_NUM = cfg->OFDM_CA_NUM;
    OFDM_DATA_NUM = cfg->OFDM_DATA_NUM;
    subframe_num_perframe = cfg->symbol_num_perframe;
    data_subframe_num_perframe = cfg->data_symbol_num_perframe;
    ul_data_subframe_num_perframe = cfg->ul_data_symbol_num_perframe;
    dl_data_subframe_num_perframe = cfg->dl_data_symbol_num_perframe;
    downlink_mode = cfg_->downlink_mode;
    dl_data_subframe_start = cfg->dl_data_symbol_start;
    dl_data_subframe_end = cfg->dl_data_symbol_end;
    packet_length = cfg->packet_length;

    TASK_THREAD_NUM = cfg->worker_thread_num;
    SOCKET_RX_THREAD_NUM = cfg->socket_thread_num;
    SOCKET_TX_THREAD_NUM = cfg->socket_thread_num;
    FFT_THREAD_NUM = cfg->fft_thread_num;
    DEMUL_THREAD_NUM = cfg->demul_thread_num;
    ZF_THREAD_NUM = cfg->zf_thread_num;
    CORE_OFFSET = cfg->core_offset;
    demul_block_size = cfg->demul_block_size;
    zf_block_size = cfg->zf_block_size;
    demul_block_num = OFDM_DATA_NUM / demul_block_size + (OFDM_DATA_NUM % demul_block_size == 0 ? 0 : 1);
    zf_block_num = OFDM_DATA_NUM/zf_block_size + (OFDM_DATA_NUM % zf_block_size == 0 ? 0 : 1);

    LDPC_config = cfg->LDPC_config;
    mod_type = cfg->mod_type;
}


void Millipede::initialize_queues() 
{
    message_queue_ = moodycamel::ConcurrentQueue<Event_data>(512 * data_subframe_num_perframe);
    complete_task_queue_ = moodycamel::ConcurrentQueue<Event_data>(512 * data_subframe_num_perframe * 4);

    fft_queue_ = moodycamel::ConcurrentQueue<Event_data>(512 * data_subframe_num_perframe * 4);
    zf_queue_ = moodycamel::ConcurrentQueue<Event_data>(512 * data_subframe_num_perframe * 4);;
    demul_queue_ = moodycamel::ConcurrentQueue<Event_data>(512 * data_subframe_num_perframe * 4);
    decode_queue_ = moodycamel::ConcurrentQueue<Event_data>(512 * data_subframe_num_perframe * 4);
    

    ifft_queue_ = moodycamel::ConcurrentQueue<Event_data>(512 * data_subframe_num_perframe * 4);
    // modulate_queue_ = moodycamel::ConcurrentQueue<Event_data>(512 * data_subframe_num_perframe * 4);
    encode_queue_ = moodycamel::ConcurrentQueue<Event_data>(512 * data_subframe_num_perframe * 4);
    precode_queue_ = moodycamel::ConcurrentQueue<Event_data>(512 * data_subframe_num_perframe * 4);
    tx_queue_ = moodycamel::ConcurrentQueue<Event_data>(512 * data_subframe_num_perframe * 4);

    rx_ptoks_ptr = (moodycamel::ProducerToken **)aligned_alloc(64, SOCKET_RX_THREAD_NUM * sizeof(moodycamel::ProducerToken *));
    for (int i = 0; i < SOCKET_RX_THREAD_NUM; i++) 
        rx_ptoks_ptr[i] = new moodycamel::ProducerToken(message_queue_);

    tx_ptoks_ptr = (moodycamel::ProducerToken **)aligned_alloc(64, SOCKET_RX_THREAD_NUM * sizeof(moodycamel::ProducerToken *));
    for (int i = 0; i < SOCKET_RX_THREAD_NUM; i++) 
        tx_ptoks_ptr[i] = new moodycamel::ProducerToken(tx_queue_);

    task_ptoks_ptr = (moodycamel::ProducerToken **)aligned_alloc(64, TASK_THREAD_NUM * sizeof(moodycamel::ProducerToken *));
    for (int i = 0; i < TASK_THREAD_NUM; i++)
        task_ptoks_ptr[i] = new moodycamel::ProducerToken(complete_task_queue_);
}


void Millipede::initialize_uplink_buffers()
{
    alloc_buffer_1d(&task_threads, TASK_THREAD_NUM, 64, 0);
    alloc_buffer_1d(&context, TASK_THREAD_NUM, 64, 0);
    // task_threads = (pthread_t *)malloc(TASK_THREAD_NUM * sizeof(pthread_t));
    // context = (EventHandlerContext *)malloc(TASK_THREAD_NUM * sizeof(EventHandlerContext));

    socket_buffer_size_ = (long long) packet_length * subframe_num_perframe * BS_ANT_NUM * SOCKET_BUFFER_FRAME_NUM; 
    socket_buffer_status_size_ = subframe_num_perframe * BS_ANT_NUM * SOCKET_BUFFER_FRAME_NUM;
    printf("socket_buffer_size %lld, socket_buffer_status_size %d\n", socket_buffer_size_, socket_buffer_status_size_);
    alloc_buffer_2d(&socket_buffer_, SOCKET_RX_THREAD_NUM, socket_buffer_size_, 64, 0);
    alloc_buffer_2d(&socket_buffer_status_, SOCKET_RX_THREAD_NUM, socket_buffer_status_size_, 64, 1);
    alloc_buffer_2d(&csi_buffer_ , PILOT_NUM * TASK_BUFFER_FRAME_NUM, BS_ANT_NUM * OFDM_DATA_NUM, 64, 0);
    alloc_buffer_2d(&data_buffer_ , data_subframe_num_perframe * TASK_BUFFER_FRAME_NUM, BS_ANT_NUM * OFDM_DATA_NUM, 64, 0);
    alloc_buffer_2d(&pred_csi_buffer_ , OFDM_DATA_NUM, BS_ANT_NUM * UE_NUM, 64, 0);
    alloc_buffer_2d(&precoder_buffer_ , OFDM_DATA_NUM * TASK_BUFFER_FRAME_NUM, UE_NUM * BS_ANT_NUM, 64, 0);
    alloc_buffer_2d(&equal_buffer_ , data_subframe_num_perframe * TASK_BUFFER_FRAME_NUM, OFDM_DATA_NUM * UE_NUM, 64, 0);
    alloc_buffer_2d(&demod_hard_buffer_ , data_subframe_num_perframe * TASK_BUFFER_FRAME_NUM, OFDM_DATA_NUM * UE_NUM, 64, 0);
    alloc_buffer_2d(&demod_soft_buffer_ , data_subframe_num_perframe * TASK_BUFFER_FRAME_NUM, mod_type * OFDM_DATA_NUM * UE_NUM, 64, 0);
    alloc_buffer_2d(&decoded_buffer_ , data_subframe_num_perframe * TASK_BUFFER_FRAME_NUM, OFDM_DATA_NUM * UE_NUM, 64, 0);
    
    int max_packet_num_per_frame = downlink_mode ? (BS_ANT_NUM * PILOT_NUM) : (BS_ANT_NUM * (ul_data_subframe_num_perframe + PILOT_NUM));
    rx_stats_.max_task_count = max_packet_num_per_frame;
    rx_stats_.max_task_pilot_count = BS_ANT_NUM * PILOT_NUM; 
    alloc_buffer_1d(&(rx_stats_.task_count), TASK_BUFFER_FRAME_NUM, 64, 1);
    alloc_buffer_1d(&(rx_stats_.task_pilot_count), TASK_BUFFER_FRAME_NUM, 64, 1);
    alloc_buffer_1d(&(rx_stats_.fft_created_count), TASK_BUFFER_FRAME_NUM, 64, 1);

    fft_stats_.max_task_count = BS_ANT_NUM;
    fft_stats_.max_symbol_pilot_count = PILOT_NUM;
    fft_stats_.max_symbol_data_count = ul_data_subframe_num_perframe;
    alloc_buffer_2d(&(fft_stats_.task_count), TASK_BUFFER_FRAME_NUM, subframe_num_perframe, 64, 1);
    alloc_buffer_2d(&(fft_stats_.data_exist_in_symbol), TASK_BUFFER_FRAME_NUM, data_subframe_num_perframe, 64, 1);
    alloc_buffer_1d(&(fft_stats_.symbol_pilot_count), TASK_BUFFER_FRAME_NUM, 64, 1);
    alloc_buffer_1d(&(fft_stats_.symbol_data_count), TASK_BUFFER_FRAME_NUM, 64, 1);

    zf_stats_.max_task_count = zf_block_num;
    alloc_buffer_1d(&(zf_stats_.task_count), TASK_BUFFER_FRAME_NUM, 64, 1);
    alloc_buffer_1d(&(zf_stats_.precoder_exist_in_frame), TASK_BUFFER_FRAME_NUM, 64, 1);

    demul_stats_.max_task_count = demul_block_num;
    demul_stats_.max_symbol_count = ul_data_subframe_num_perframe;
    alloc_buffer_2d(&(demul_stats_.task_count), TASK_BUFFER_FRAME_NUM, data_subframe_num_perframe, 64, 1);
    alloc_buffer_1d(&(demul_stats_.symbol_count), TASK_BUFFER_FRAME_NUM, 64, 1);

    decode_stats_.max_task_count = LDPC_config.nblocksInSymbol * UE_NUM;
    decode_stats_.max_symbol_count = ul_data_subframe_num_perframe;
    alloc_buffer_2d(&(decode_stats_.task_count), TASK_BUFFER_FRAME_NUM, data_subframe_num_perframe, 64, 1);
    alloc_buffer_1d(&(decode_stats_.symbol_count), TASK_BUFFER_FRAME_NUM, 64, 1);
    

    alloc_buffer_2d(&delay_fft_queue, TASK_BUFFER_FRAME_NUM, subframe_num_perframe * BS_ANT_NUM, 32, 1);
    alloc_buffer_1d(&delay_fft_queue_cnt, TASK_BUFFER_FRAME_NUM, 32, 1);
}


void Millipede::initialize_downlink_buffers()
{


    dl_socket_buffer_size_ = (long long) data_subframe_num_perframe * SOCKET_BUFFER_FRAME_NUM * packet_length * BS_ANT_NUM;
    dl_socket_buffer_status_size_ = data_subframe_num_perframe * BS_ANT_NUM * SOCKET_BUFFER_FRAME_NUM;
    alloc_buffer_1d(&dl_socket_buffer_, dl_socket_buffer_size_, 64, 0);
    alloc_buffer_1d(&dl_socket_buffer_status_, dl_socket_buffer_status_size_, 64, 1);
    alloc_buffer_2d(&dl_ifft_buffer_, BS_ANT_NUM * data_subframe_num_perframe * TASK_BUFFER_FRAME_NUM, OFDM_CA_NUM, 64, 1);
    alloc_buffer_2d(&dl_precoded_data_buffer_, data_subframe_num_perframe * TASK_BUFFER_FRAME_NUM, BS_ANT_NUM * OFDM_DATA_NUM, 64, 0);
    alloc_buffer_2d(&dl_modulated_buffer_, data_subframe_num_perframe * TASK_BUFFER_FRAME_NUM, UE_NUM * OFDM_DATA_NUM, 64, 0);
    alloc_buffer_2d(&dl_precoder_buffer_ , OFDM_DATA_NUM * TASK_BUFFER_FRAME_NUM, UE_NUM * BS_ANT_NUM, 64, 0);
    alloc_buffer_2d(&dl_encoded_buffer_ , data_subframe_num_perframe * TASK_BUFFER_FRAME_NUM, OFDM_DATA_NUM * UE_NUM, 64, 0);
    alloc_buffer_2d(&recip_buffer_ , OFDM_DATA_NUM, BS_ANT_NUM, 64, 0);

    encode_stats_.max_task_count = LDPC_config.nblocksInSymbol * UE_NUM;
    encode_stats_.max_symbol_count = dl_data_subframe_num_perframe;
    alloc_buffer_2d(&(encode_stats_.task_count), TASK_BUFFER_FRAME_NUM, data_subframe_num_perframe, 64, 1);
    alloc_buffer_1d(&(encode_stats_.symbol_count), TASK_BUFFER_FRAME_NUM, 64, 1);

    precode_stats_.max_task_count = demul_block_num;
    precode_stats_.max_symbol_count = dl_data_subframe_num_perframe;
    alloc_buffer_2d(&(precode_stats_.task_count), TASK_BUFFER_FRAME_NUM, data_subframe_num_perframe, 64, 1);
    alloc_buffer_1d(&(precode_stats_.symbol_count), TASK_BUFFER_FRAME_NUM, 64, 1);

    ifft_stats_.max_task_count = BS_ANT_NUM;
    ifft_stats_.max_symbol_count = dl_data_subframe_num_perframe;
    alloc_buffer_2d(&(ifft_stats_.task_count), TASK_BUFFER_FRAME_NUM, data_subframe_num_perframe, 64, 1);
    alloc_buffer_1d(&(ifft_stats_.symbol_count), TASK_BUFFER_FRAME_NUM, 64, 1);

    tx_stats_.max_task_count = BS_ANT_NUM;
    tx_stats_.max_symbol_count = dl_data_subframe_num_perframe;
    alloc_buffer_2d(&(tx_stats_.task_count), TASK_BUFFER_FRAME_NUM, data_subframe_num_perframe, 64, 1);
    alloc_buffer_1d(&(tx_stats_.symbol_count), TASK_BUFFER_FRAME_NUM, 64, 1);
}


void Millipede::free_uplink_buffers()
{
    //free_buffer_1d(&pilots_);
    free_buffer_2d(&socket_buffer_, SOCKET_RX_THREAD_NUM);
    free_buffer_2d(&socket_buffer_status_, SOCKET_RX_THREAD_NUM);
    free_buffer_2d(&csi_buffer_, UE_NUM * TASK_BUFFER_FRAME_NUM);
    free_buffer_2d(&data_buffer_, data_subframe_num_perframe * TASK_BUFFER_FRAME_NUM);
    free_buffer_2d(&pred_csi_buffer_ , OFDM_DATA_NUM);
    free_buffer_2d(&precoder_buffer_ , OFDM_DATA_NUM * TASK_BUFFER_FRAME_NUM);
    free_buffer_2d(&equal_buffer_ , data_subframe_num_perframe * TASK_BUFFER_FRAME_NUM);
    free_buffer_2d(&demod_hard_buffer_ , data_subframe_num_perframe * TASK_BUFFER_FRAME_NUM);
    free_buffer_2d(&demod_soft_buffer_ , data_subframe_num_perframe * TASK_BUFFER_FRAME_NUM);
    free_buffer_2d(&decoded_buffer_ , data_subframe_num_perframe * TASK_BUFFER_FRAME_NUM);

    free_buffer_1d(&(rx_stats_.task_count));
    free_buffer_1d(&(rx_stats_.task_pilot_count));
    free_buffer_1d(&(rx_stats_.fft_created_count));
    free_buffer_2d(&(fft_stats_.task_count), TASK_BUFFER_FRAME_NUM);
    free_buffer_2d(&(fft_stats_.data_exist_in_symbol), TASK_BUFFER_FRAME_NUM);
    free_buffer_1d(&(fft_stats_.symbol_pilot_count));
    free_buffer_1d(&(fft_stats_.symbol_data_count));
    free_buffer_1d(&(zf_stats_.task_count));
    free_buffer_1d(&(zf_stats_.precoder_exist_in_frame));
    free_buffer_2d(&(demul_stats_.task_count), TASK_BUFFER_FRAME_NUM);
    free_buffer_1d(&(demul_stats_.symbol_count));
    free_buffer_2d(&(decode_stats_.task_count), TASK_BUFFER_FRAME_NUM);
    free_buffer_1d(&(decode_stats_.symbol_count));

    free_buffer_2d(&delay_fft_queue, TASK_BUFFER_FRAME_NUM);
    free_buffer_1d(&delay_fft_queue_cnt);
}

void Millipede::free_downlink_buffers()
{
    free_buffer_1d(&dl_socket_buffer_);
    free_buffer_1d(&dl_socket_buffer_status_);

    free_buffer_2d(&dl_ifft_buffer_, BS_ANT_NUM * data_subframe_num_perframe * TASK_BUFFER_FRAME_NUM);
    free_buffer_2d(&dl_precoded_data_buffer_, data_subframe_num_perframe * TASK_BUFFER_FRAME_NUM);
    free_buffer_2d(&dl_modulated_buffer_, data_subframe_num_perframe * TASK_BUFFER_FRAME_NUM);


    free_buffer_2d(&(encode_stats_.task_count), TASK_BUFFER_FRAME_NUM);
    free_buffer_1d(&(encode_stats_.symbol_count));
    free_buffer_2d(&(precode_stats_.task_count), TASK_BUFFER_FRAME_NUM);
    free_buffer_1d(&(precode_stats_.symbol_count));
    free_buffer_2d(&(ifft_stats_.task_count), TASK_BUFFER_FRAME_NUM);
    free_buffer_1d(&(ifft_stats_.symbol_count));
    free_buffer_2d(&(tx_stats_.task_count), TASK_BUFFER_FRAME_NUM);
    free_buffer_1d(&(tx_stats_.symbol_count));
}



void Millipede::save_demul_data_to_file(int frame_id, int data_subframe_id)
{
#if WRITE_DEMUL
    std::string cur_directory = TOSTRING(PROJECT_DIRECTORY);
    std::string filename = cur_directory + "/data/demul_data.txt";
    FILE* fp = fopen(filename.c_str(),"a");
    int total_data_subframe_id = frame_id * data_subframe_num_perframe + data_subframe_id;
    for (int cc = 0; cc < OFDM_DATA_NUM; cc++) {
        int *cx = &demod_hard_buffer_[total_data_subframe_id][cc * UE_NUM];
        fprintf(fp, "SC: %d, Frame %d, subframe: %d, ", cc, frame_id, data_subframe_id);
        for(int kk = 0; kk < UE_NUM; kk++)  
            fprintf(fp, "%d ", cx[kk]);
        fprintf(fp, "\n");
    }
    fclose(fp);
#endif    
}



void Millipede::getDemulData(int **ptr, int *size)
{
    *ptr = (int *)&equal_buffer_[max_equaled_frame * data_subframe_num_perframe][0];
    *size = UE_NUM * OFDM_CA_NUM;
}

void Millipede::getEqualData(float **ptr, int *size)
{
    // max_equaled_frame = 0;
    *ptr = (float *)&equal_buffer_[max_equaled_frame * data_subframe_num_perframe][0];
    // *ptr = equal_output;
    *size = UE_NUM*OFDM_DATA_NUM*2;
    
    //printf("In getEqualData()\n");
    //for(int ii = 0; ii < UE_NUM*OFDM_DATA_NUM; ii++)
    //{
    //    // printf("User %d: %d, ", ii,demul_ptr2(ii));
    //    printf("[%.4f+j%.4f] ", *(*ptr+ii*UE_NUM*2), *(*ptr+ii*UE_NUM*2+1));
    //}
    //printf("\n");
    //printf("\n");
    
}



extern "C"
{
    EXPORT Millipede* Millipede_new(Config *cfg) {
        // printf("Size of Millipede: %d\n",sizeof(Millipede *));
        Millipede *millipede = new Millipede(cfg);
        
        return millipede;
    }
    EXPORT void Millipede_start(Millipede *millipede) {millipede->start();}
    EXPORT void Millipede_stop(/*Millipede *millipede*/) {SignalHandler::setExitSignal(true); /*millipede->stop();*/}
    EXPORT void Millipede_destroy(Millipede *millipede) {delete millipede;}
    EXPORT void Millipede_getEqualData(Millipede *millipede, float **ptr, int *size) {return millipede->getEqualData(ptr, size);}
    EXPORT void Millipede_getDemulData(Millipede *millipede, int **ptr, int *size) {return millipede->getDemulData(ptr, size);}
}




