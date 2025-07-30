#include "nack_generator.hpp"
#include "rtprtcp_pub.hpp"
#include "logger.hpp"
#include "timeex.hpp"
#include <algorithm>
#include <sstream>

namespace cpp_streamer
{

NackGenerator::NackGenerator(uv_loop_t* loop, Logger* logger, NackGeneratorCallbackI* cb)
    : TimerInterface(loop, NACK_DEFAULT_TIMEOUT), logger_(logger), cb_(cb) {
    StartTimer();
}

NackGenerator::~NackGenerator() {
    StopTimer();
}

void NackGenerator::UpdateRtt(int64_t rtt) {
    LogInfof(logger_, "nack %u updateRTT %ld", ssrc_, rtt);
    rtt_ = rtt;
}

void NackGenerator::InputPacket(RtpPacket* pkt) {
    uint16_t seq = pkt->GetSeq();
    if (pkt->GetSsrc()!=ssrc_)
        ssrc_ = pkt->GetSsrc();
    if (!init_flag_) {
        init_flag_ = true;
        last_seq_  = seq;
        return;
    }

    //receive repeated sequence
    if (seq == last_seq_) {
        return;
    }

    //LogInfof(logger_, "nack %u packet seq:%d", ssrc_, seq);
    if (SeqLowerThan(seq, last_seq_)) {
        auto iter = nack_map_.find(seq);

        //the seq has been in the nack list, remove it.
        if (iter != nack_map_.end()) {
            nack_map_.erase(iter);
            LogInfof(logger_, "nack %u recover seq:%d, last seq:%d, pt:%d, rtt:%ld",
                pkt->GetSsrc(), seq, last_seq_, pkt->GetPayloadType(), rtt_);
            return;
        }
        /*
        std::stringstream ss;
        ss << "[";
        for (auto item : nack_map_) {
            ss << " " << item.first;
        }
        ss << " ]";
        LogInfof(logger_, "receive the old packet which is not in nack list, ssrc:%u, seq:%d, last seq:%d, pt:%d, nack list:%s, rtt:%ld",
            pkt->GetSsrc(), seq, last_seq_, pkt->GetPayloadType(), ss.str().c_str(), rtt_);
        */
        return;
    }

    if ((last_seq_ + 1) == seq) {
        last_seq_ = seq;
        return;
    }

    uint16_t seq_start = last_seq_;
    uint16_t seq_end   = seq;
    
    last_seq_ = seq;
    LogInfof(logger_, "nack %u detect lost %d->%d", ssrc_, seq_start + 1, seq_end - 1);
    //add seqs in nack list
    for (uint16_t key_seq = seq_start + 1; key_seq < seq_end; key_seq++) {
        if (0 == nack_map_.count(key_seq)) {
            nack_map_.insert(std::make_pair(key_seq, NACK_INFO(key_seq, 0, 0)));
        }
    }
}

void NackGenerator::OnTimer() {
    if (nack_map_.empty()) {
        return;
    }

    int64_t now_ms = now_millisec();
    std::vector<uint16_t> lost_seq_list;

    auto iter = nack_map_.begin();
    while(iter != nack_map_.end()) {
        if (iter->second.retry > NACK_RETRY_MAX) {
            iter = nack_map_.erase(iter);
            continue;
        }

        if (now_ms - iter->second.sent_ms >= rtt_) {
            iter->second.sent_ms = now_ms;
            iter->second.retry++;
            lost_seq_list.push_back(iter->first);
        }
        iter++;
    }

    //LogInfof(logger_, "nack list len:%lu", lost_seq_list.size());
    if (!lost_seq_list.empty()) {
        std::sort(lost_seq_list.begin(), lost_seq_list.end());
        cb_->GenerateNackList(lost_seq_list);
    }

    if (nack_map_.size() > NACK_LIST_MAX) {
        LogWarnf(logger_, "nack %u list overflow: got %lu, max %d", ssrc_, nack_map_.size(), NACK_LIST_MAX);
        while (nack_map_.size() > NACK_LIST_MAX) {
            nack_map_.erase(nack_map_.begin());
        }
    }

}

}

