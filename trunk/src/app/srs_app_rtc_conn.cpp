/**
 * The MIT License (MIT)
 *
 * Copyright (c) 2013-2020 John
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
 * the Software, and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include <srs_app_rtc_conn.hpp>

using namespace std;

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>

#include <sstream>

#include <srs_core_autofree.hpp>
#include <srs_kernel_buffer.hpp>
#include <srs_kernel_rtc_rtp.hpp>
#include <srs_kernel_error.hpp>
#include <srs_kernel_log.hpp>
#include <srs_rtc_stun_stack.hpp>
#include <srs_rtmp_stack.hpp>
#include <srs_rtmp_msg_array.hpp>
#include <srs_app_utility.hpp>
#include <srs_app_config.hpp>
#include <srs_app_rtc_queue.hpp>
#include <srs_app_source.hpp>
#include <srs_app_server.hpp>
#include <srs_service_utility.hpp>
#include <srs_http_stack.hpp>
#include <srs_app_http_api.hpp>
#include <srs_app_statistic.hpp>
#include <srs_app_pithy_print.hpp>
#include <srs_service_st.hpp>
#include <srs_app_rtc_server.hpp>
#include <srs_app_rtc_source.hpp>
#include <srs_protocol_utility.hpp>

#ifdef SRS_CXX14
#include <srs_api/srs_webrtc_api.hpp>
#endif

void srs_session_request_keyframe(SrsRtcStream* source, uint32_t ssrc)
{
    // When enable some video tracks, we should request PLI for that SSRC.
    if (!source) {
        return;
    }

    ISrsRtcPublishStream* publisher = source->publish_stream();
    if (!publisher) {
        return;
    }

    publisher->request_keyframe(ssrc);
}

SrsSecurityTransport::SrsSecurityTransport(SrsRtcConnection* s)
{
    session_ = s;

    dtls_ = new SrsDtls((ISrsDtlsCallback*)this);
    srtp_ = new SrsSRTP();

    handshake_done = false;
}

SrsSecurityTransport::~SrsSecurityTransport()
{
    srs_freep(dtls_);
    srs_freep(srtp_);
}

srs_error_t SrsSecurityTransport::initialize(SrsSessionConfig* cfg)
{
    return dtls_->initialize(cfg->dtls_role, cfg->dtls_version);
}

srs_error_t SrsSecurityTransport::start_active_handshake()
{
    return dtls_->start_active_handshake();
}

srs_error_t SrsSecurityTransport::write_dtls_data(void* data, int size) 
{
    srs_error_t err = srs_success;
    if (size) {
        if ((err = session_->sendonly_skt->sendto(data, size, 0)) != srs_success) {
            return srs_error_wrap(err, "send dtls packet");
        }
    }

    if (_srs_blackhole->blackhole) {
        _srs_blackhole->sendto(data, size);
    }

    return err;
}

srs_error_t SrsSecurityTransport::on_dtls(char* data, int nb_data)
{
    return dtls_->on_dtls(data, nb_data);
}

srs_error_t SrsSecurityTransport::on_dtls_handshake_done()
{
    srs_error_t err = srs_success;

    if (handshake_done) {
        return err;
    }

    srs_trace("RTC session=%s, DTLS handshake done.", session_->id().c_str());

    handshake_done = true;
    if ((err = srtp_initialize()) != srs_success) {
        return srs_error_wrap(err, "srtp init failed");
    }

    return session_->on_connection_established();
}

srs_error_t SrsSecurityTransport::on_dtls_application_data(const char* buf, const int nb_buf)
{
    srs_error_t err = srs_success;

    // TODO: process SCTP protocol(WebRTC datachannel support)

    return err;
}

srs_error_t SrsSecurityTransport::srtp_initialize()
{
    srs_error_t err = srs_success;

    std::string send_key;
    std::string recv_key;

    if ((err = dtls_->get_srtp_key(recv_key, send_key)) != srs_success) {
        return err;
    }
    
    if ((err = srtp_->initialize(recv_key, send_key)) != srs_success) {
        return srs_error_wrap(err, "srtp init failed");
    }

    return err;
}

srs_error_t SrsSecurityTransport::protect_rtp(const char* plaintext, char* cipher, int& nb_cipher)
{
    if (!srtp_) {
        return srs_error_new(ERROR_RTC_SRTP_PROTECT, "rtp protect failed");
    }

    return srtp_->protect_rtp(plaintext, cipher, nb_cipher);
}

srs_error_t SrsSecurityTransport::protect_rtcp(const char* plaintext, char* cipher, int& nb_cipher)
{
    if (!srtp_) {
        return srs_error_new(ERROR_RTC_SRTP_PROTECT, "rtcp protect failed");
    }

    return srtp_->protect_rtcp(plaintext, cipher, nb_cipher);
}

// TODO: FIXME: Merge with protect_rtp.
srs_error_t SrsSecurityTransport::protect_rtp2(void* rtp_hdr, int* len_ptr)
{
    if (!srtp_) {
        return srs_error_new(ERROR_RTC_SRTP_PROTECT, "rtp protect");
    }

    return srtp_->protect_rtp2(rtp_hdr, len_ptr);
}

srs_error_t SrsSecurityTransport::unprotect_rtp(const char* cipher, char* plaintext, int& nb_plaintext)
{
    if (!srtp_) {
        return srs_error_new(ERROR_RTC_SRTP_UNPROTECT, "rtp unprotect failed");
    }
    
    return srtp_->unprotect_rtp(cipher, plaintext, nb_plaintext);
}

srs_error_t SrsSecurityTransport::unprotect_rtcp(const char* cipher, char* plaintext, int& nb_plaintext)
{
    if (!srtp_) {
        return srs_error_new(ERROR_RTC_SRTP_UNPROTECT, "rtcp unprotect failed");
    }

    return srtp_->unprotect_rtcp(cipher, plaintext, nb_plaintext);
}

SrsRtcPlayStreamStatistic::SrsRtcPlayStreamStatistic()
{
#if defined(SRS_DEBUG)
    debug_id = 0;
#endif

    nn_rtp_pkts = 0;
    nn_audios = nn_extras = 0;
    nn_videos = nn_samples = 0;
    nn_bytes = nn_rtp_bytes = 0;
    nn_padding_bytes = nn_paddings = 0;
}

SrsRtcPlayStreamStatistic::~SrsRtcPlayStreamStatistic()
{
}

SrsRtcPlayStream::SrsRtcPlayStream(SrsRtcConnection* s, SrsContextId parent_cid)
{
    _parent_cid = parent_cid;
    trd = new SrsDummyCoroutine();

    session_ = s;

    mw_msgs = 0;
    realtime = true;

    nack_enabled_ = false;
    is_started = false;

    _srs_config->subscribe(this);

    switch_context_ = new SrsStreamSwitchContext();
}

SrsRtcPlayStream::~SrsRtcPlayStream()
{
    _srs_config->unsubscribe(this);

    srs_freep(trd);

    // Free context before tracks, because context refers to them.
    srs_freep(switch_context_);

    if (true) {
        std::map<uint32_t, SrsRtcAudioSendTrack*>::iterator it;
        for (it = audio_tracks_.begin(); it != audio_tracks_.end(); ++it) {
            srs_freep(it->second);
        }
    }

    if (true) {
        std::map<uint32_t, SrsRtcVideoSendTrack*>::iterator it;
        for (it = video_tracks_.begin(); it != video_tracks_.end(); ++it) {
            srs_freep(it->second);
        }
    }
}

srs_error_t SrsRtcPlayStream::initialize(SrsRequest* req, std::map<uint32_t, SrsRtcTrackDescription*> sub_relations)
{
    srs_error_t err = srs_success;

    if (true) {
        std::map<uint32_t, SrsRtcTrackDescription*>::iterator it = sub_relations.begin();
        while (it != sub_relations.end()) {
            if (it->second->type_ == "audio") {
                audio_tracks_.insert(make_pair(it->first, new SrsRtcAudioSendTrack(session_, it->second)));
            }

            if (it->second->type_ == "video") {
                video_tracks_.insert(make_pair(it->first, new SrsRtcVideoSendTrack(session_, it->second)));
            }
            ++it;
        }
    }

    nack_enabled_ = _srs_config->get_rtc_nack_enabled(req->vhost);
    srs_trace("RTC player nack=%d", nack_enabled_);

    session_->stat_->nn_subscribers++;

    return err;
}

srs_error_t SrsRtcPlayStream::on_reload_vhost_play(string vhost)
{
    SrsRequest* req = session_->req;

    if (req->vhost != vhost) {
        return srs_success;
    }

    realtime = _srs_config->get_realtime_enabled(req->vhost, true);
    mw_msgs = _srs_config->get_mw_msgs(req->vhost, realtime, true);

    srs_trace("Reload play realtime=%d, mw_msgs=%d", realtime, mw_msgs);

    return srs_success;
}

srs_error_t SrsRtcPlayStream::on_reload_vhost_realtime(string vhost)
{
    return on_reload_vhost_play(vhost);
}

SrsContextId SrsRtcPlayStream::cid()
{
    return trd->cid();
}

srs_error_t SrsRtcPlayStream::start()
{
    srs_error_t err = srs_success;

    // If player coroutine allocated, we think the player is started.
    // To prevent play multiple times for this play stream.
    // @remark Allow start multiple times, for DTLS may retransmit the final packet.
    if (is_started) {
        return err;
    }

    srs_freep(trd);
    trd = new SrsSTCoroutine("rtc_sender", this, _parent_cid);

    if ((err = trd->start()) != srs_success) {
        return srs_error_wrap(err, "rtc_sender");
    }

    if (_srs_rtc_hijacker) {
        if ((err = _srs_rtc_hijacker->on_start_play(session_, this, session_->req)) != srs_success) {
            return srs_error_wrap(err, "on start play");
        }
    }

    // When start play the stream, we request PLI to enable player to decode frame ASAP.
    std::map<uint32_t, SrsRtcVideoSendTrack*>::iterator it;
    for (it = video_tracks_.begin(); it != video_tracks_.end(); ++it) {
        SrsRtcVideoSendTrack* track = it->second;

        // If the track is merging stream, we should request PLI when it startup.
        if (switch_context_->is_track_preparing(track)) {
            srs_session_request_keyframe(session_->source_, it->first);
        }
    }

    is_started = true;

    return err;
}

void SrsRtcPlayStream::stop()
{
    trd->stop();
}

void SrsRtcPlayStream::stop_loop()
{
    trd->interrupt();
}

srs_error_t SrsRtcPlayStream::cycle()
{
    srs_error_t err = srs_success;

    SrsRtcStream* source = NULL;
    SrsRequest* req = session_->req;

    if ((err = _srs_rtc_sources->fetch_or_create(req, &source)) != srs_success) {
        return srs_error_wrap(err, "rtc fetch source failed");
    }

    SrsRtcConsumer* consumer = NULL;
    SrsAutoFree(SrsRtcConsumer, consumer);
    if ((err = source->create_consumer(consumer)) != srs_success) {
        return srs_error_wrap(err, "rtc create consumer, source url=%s", req->get_stream_url().c_str());
    }

    // TODO: FIXME: Dumps the SPS/PPS from gop cache, without other frames.
    if ((err = source->consumer_dumps(consumer)) != srs_success) {
        return srs_error_wrap(err, "dumps consumer, source url=%s", req->get_stream_url().c_str());
    }

    realtime = _srs_config->get_realtime_enabled(req->vhost, true);
    mw_msgs = _srs_config->get_mw_msgs(req->vhost, realtime, true);

    SrsContextId cid = source->source_id();
    if (!cid.empty()) {
        _srs_context->bind(cid, "RTC play");
    }
    srs_trace("RTC source url=%s, source_id=[%d][%s], encrypt=%d, realtime=%d, mw_msgs=%d", req->get_stream_url().c_str(),
        ::getpid(), cid.c_str(), session_->encrypt, realtime, mw_msgs);

    SrsPithyPrint* pprint = SrsPithyPrint::create_rtc_play();
    SrsAutoFree(SrsPithyPrint, pprint);

    srs_trace("RTC session=%s, start play", session_->id().c_str());
    bool stat_enabled = _srs_config->get_rtc_server_perf_stat();
    SrsStatistic* stat = SrsStatistic::instance();

    // TODO: FIXME: Use cache for performance?
    vector<SrsRtpPacket2*> pkts;

    if (_srs_rtc_hijacker) {
        if ((err = _srs_rtc_hijacker->on_start_consume(session_, this, session_->req, consumer)) != srs_success) {
            return srs_error_wrap(err, "on start consuming");
        }
    }

    while (true) {
        if ((err = trd->pull()) != srs_success) {
            return srs_error_wrap(err, "rtc sender thread");
        }

        // Wait for amount of packets.
        consumer->wait(mw_msgs);

        // TODO: FIXME: Handle error.
        consumer->dump_packets(pkts);

        int msg_count = (int)pkts.size();
        if (!msg_count) {
            continue;
        }

        // Update stats for session.
        session_->stat_->nn_out_rtp += msg_count;

        // Send-out all RTP packets and do cleanup.
        // TODO: FIXME: Handle error.
        send_packets(source, pkts, info);

        for (int i = 0; i < msg_count; i++) {
            SrsRtpPacket2* pkt = pkts[i];
            srs_freep(pkt);
        }
        pkts.clear();

        // Stat for performance analysis.
        if (!stat_enabled) {
            continue;
        }

        // Stat the original RAW AV frame, maybe h264+aac.
        stat->perf_on_msgs(msg_count);
        // Stat the RTC packets, RAW AV frame, maybe h.264+opus.
        int nn_rtc_packets = srs_max(info.nn_audios, info.nn_extras) + info.nn_videos;
        stat->perf_on_rtc_packets(nn_rtc_packets);
        // Stat the RAW RTP packets, which maybe group by GSO.
        stat->perf_on_rtp_packets(msg_count);
        // Stat the bytes and paddings.
        stat->perf_on_rtc_bytes(info.nn_bytes, info.nn_rtp_bytes, info.nn_padding_bytes);

        pprint->elapse();
        if (pprint->can_print()) {
            // TODO: FIXME: Print stat like frame/s, packet/s, loss_packets.
            srs_trace("-> RTC PLAY %d msgs, %d/%d packets, %d audios, %d extras, %d videos, %d samples, %d/%d/%d bytes, %d pad, %d/%d cache",
                msg_count, msg_count, info.nn_rtp_pkts, info.nn_audios, info.nn_extras, info.nn_videos, info.nn_samples, info.nn_bytes,
                info.nn_rtp_bytes, info.nn_padding_bytes, info.nn_paddings, msg_count, msg_count);
        }
    }
}

srs_error_t SrsRtcPlayStream::send_packets(SrsRtcStream* source, const vector<SrsRtpPacket2*>& pkts, SrsRtcPlayStreamStatistic& info)
{
    srs_error_t err = srs_success;

    // If DTLS is not OK, drop all messages.
    if (!session_->transport_) {
        return err;
    }

    vector<SrsRtpPacket2*> send_pkts;
    // Covert kernel messages to RTP packets.
    for (int i = 0; i < (int)pkts.size(); i++) {
        SrsRtpPacket2* pkt = pkts[i];

        // TODO: FIXME: Maybe refine for performance issue.
        if (!audio_tracks_.count(pkt->header.get_ssrc()) && !video_tracks_.count(pkt->header.get_ssrc())) {
            continue;
        }
        
        // For audio, we transcoded AAC to opus in extra payloads.
        if (pkt->is_audio()) {
            SrsRtcAudioSendTrack* audio_track = audio_tracks_[pkt->header.get_ssrc()];
            // TODO: FIXME: Any simple solution?
            if ((err = audio_track->on_rtp(pkt, info)) != srs_success) {
                return srs_error_wrap(err, "audio_track on rtp");
            }
            // TODO: FIXME: Padding audio to the max payload in RTP packets.
        } else {
            SrsRtcVideoSendTrack* video_track = video_tracks_[pkt->header.get_ssrc()];

            // If got keyframe, switch to the preparing track,
            // and disable previous active track.
            switch_context_->try_switch_stream(video_track, pkt);

            // TODO: FIXME: Any simple solution?
            if ((err = video_track->on_rtp(pkt, info)) != srs_success) {
                return srs_error_wrap(err, "audio_track on rtp");
            }
        }

        // Detail log, should disable it in release version.
        srs_info("RTC: Update PT=%u, SSRC=%#x, Time=%u, %u bytes", pkt->header.get_payload_type(), pkt->header.get_ssrc(),
            pkt->header.get_timestamp(), pkt->nb_bytes());
    }

    return err;
}

void SrsRtcPlayStream::nack_fetch(vector<SrsRtpPacket2*>& pkts, uint32_t ssrc, uint16_t seq)
{
    if (true) {
        std::map<uint32_t, SrsRtcAudioSendTrack*>::iterator it;
        for (it = audio_tracks_.begin(); it != audio_tracks_.end(); ++it) {
            if (it->second->has_ssrc(ssrc)) {
                SrsRtpPacket2* pkt = it->second->fetch_rtp_packet(seq);
                if (pkt != NULL) {
                    pkts.push_back(pkt);
                }
                return;
            }
        }
    }

    if (true) {
        std::map<uint32_t, SrsRtcVideoSendTrack*>::iterator it;
        for (it = video_tracks_.begin(); it != video_tracks_.end(); ++it) {
            if (it->second->has_ssrc(ssrc)) {
                SrsRtpPacket2* pkt = it->second->fetch_rtp_packet(seq);
                if (pkt != NULL) {
                    pkts.push_back(pkt);
                }
                return;
            }
        }
    }
}

srs_error_t SrsRtcPlayStream::on_rtcp(char* data, int nb_data)
{
    srs_error_t err = srs_success;

    char* ph = data;
    int nb_left = nb_data;
    while (nb_left) {
        uint8_t payload_type = ph[1];
        uint16_t length_4bytes = (((uint16_t)ph[2]) << 8) | ph[3];

        int length = (length_4bytes + 1) * 4;

        if (length > nb_data) {
            return srs_error_new(ERROR_RTC_RTCP, "invalid rtcp packet, length=%u", length);
        }

        srs_verbose("on rtcp, payload_type=%u", payload_type);

        switch (payload_type) {
            case kSR: {
                err = on_rtcp_sr(ph, length);
                break;
            }
            case kRR: {
                err = on_rtcp_rr(ph, length);
                break;
            }
            case kSDES: {
                break;
            }
            case kBye: {
                break;
            }
            case kApp: {
                break;
            }
            case kRtpFb: {
                err = on_rtcp_feedback(ph, length);
                break;
            }
            case kPsFb: {
                err = on_rtcp_ps_feedback(ph, length);
                break;
            }
            case kXR: {
                err = on_rtcp_xr(ph, length);
                break;
            }
            default:{
                return srs_error_new(ERROR_RTC_RTCP_CHECK, "unknown rtcp type=%u", payload_type);
                break;
            }
        }

        if (err != srs_success) {
            return srs_error_wrap(err, "rtcp");
        }

        ph += length;
        nb_left -= length;
    }

    return err;
}

srs_error_t SrsRtcPlayStream::on_rtcp_sr(char* buf, int nb_buf)
{
    srs_error_t err = srs_success;

    // TODO: FIXME: Implements it.

    session_->stat_->nn_sr++;

    return err;
}

srs_error_t SrsRtcPlayStream::on_rtcp_xr(char* buf, int nb_buf)
{
    srs_error_t err = srs_success;

    // TODO: FIXME: Implements it.

    session_->stat_->nn_xr++;

    return err;
}

srs_error_t SrsRtcPlayStream::on_rtcp_feedback(char* buf, int nb_buf)
{
    srs_error_t err = srs_success;

    if (nb_buf < 12) {
        return srs_error_new(ERROR_RTC_RTCP_CHECK, "invalid rtp feedback packet, nb_buf=%d", nb_buf);
    }

    SrsBuffer* stream = new SrsBuffer(buf, nb_buf);
    SrsAutoFree(SrsBuffer, stream);

    // @see: https://tools.ietf.org/html/rfc4585#section-6.1
    /*
        0                   1                   2                   3
        0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
       +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
       |V=2|P|   FMT   |       PT      |          length               |
       +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
       |                  SSRC of packet sender                        |
       +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
       |                  SSRC of media source                         |
       +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
       :            Feedback Control Information (FCI)                 :
       :                                                               :
    */
    uint8_t first = stream->read_1bytes();
    //uint8_t version = first & 0xC0;
    //uint8_t padding = first & 0x20;
    uint8_t fmt = first & 0x1F;
    if(15 == fmt) {
        return session_->on_rtcp_feedback(buf, nb_buf);
    }

    /*uint8_t payload_type = */stream->read_1bytes();
    /*uint16_t length = */stream->read_2bytes();
    /*uint32_t ssrc_of_sender = */stream->read_4bytes();
    uint32_t ssrc_of_media_source = stream->read_4bytes();

    /*
         0                   1                   2                   3
         0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
        +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
        |            PID                |             BLP               |
        +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    */

    uint16_t pid = stream->read_2bytes();
    int blp = stream->read_2bytes();

    // TODO: FIXME: Support ARQ.
    vector<SrsRtpPacket2*> resend_pkts;
    nack_fetch(resend_pkts, ssrc_of_media_source, pid);

    // If NACK disabled, print a log.
    if (!nack_enabled_) {
        srs_trace("RTC NACK seq=%u, ignored", pid);
        return err;
    }

    uint16_t mask = 0x01;
    for (int i = 1; i < 16 && blp; ++i, mask <<= 1) {
        if (!(blp & mask)) {
            continue;
        }

        uint32_t loss_seq = pid + i;
        nack_fetch(resend_pkts, ssrc_of_media_source, loss_seq);
    }

    for (int i = 0; i < (int)resend_pkts.size(); ++i) {
        SrsRtpPacket2* pkt = resend_pkts[i];
        info.nn_bytes += pkt->nb_bytes();
        
        srs_trace("RTC NACK ARQ seq=%u, ssrc=%u, ts=%u, %d bytes", pkt->header.get_sequence(),
            pkt->header.get_ssrc(), pkt->header.get_timestamp(), pkt->nb_bytes());
    }

    // By default, we send packets by sendmmsg.
    if ((err = session_->do_send_packets(resend_pkts, info)) != srs_success) {
        return srs_error_wrap(err, "raw send");
    }

    session_->stat_->nn_nack++;

    return err;
}

srs_error_t SrsRtcPlayStream::on_rtcp_ps_feedback(char* buf, int nb_buf)
{
    srs_error_t err = srs_success;

    if (nb_buf < 12) {
        return srs_error_new(ERROR_RTC_RTCP_CHECK, "invalid rtp feedback packet, nb_buf=%d", nb_buf);
    }

    SrsBuffer* stream = new SrsBuffer(buf, nb_buf);
    SrsAutoFree(SrsBuffer, stream);

    uint8_t first = stream->read_1bytes();
    //uint8_t version = first & 0xC0;
    //uint8_t padding = first & 0x20;
    uint8_t fmt = first & 0x1F;

    /*uint8_t payload_type = */stream->read_1bytes();
    /*uint16_t length = */stream->read_2bytes();
    /*uint32_t ssrc_of_sender = */stream->read_4bytes();
    uint32_t ssrc_of_media_source = stream->read_4bytes();

    switch (fmt) {
        case kPLI: {
            ISrsRtcPublishStream* publisher = session_->source_->publish_stream();
            if (publisher) {
                uint32_t ssrc = get_video_publish_ssrc(ssrc_of_media_source);
                if (ssrc != 0) {
                    publisher->request_keyframe(ssrc);
                    srs_trace("RTC request PLI");
                }
            }

            session_->stat_->nn_pli++;
            break;
        }
        case kSLI: {
            srs_verbose("sli");
            break;
        }
        case kRPSI: {
            srs_verbose("rpsi");
            break;
        }
        case kAFB: {
            srs_verbose("afb");
            break;
        }
        default: {
            return srs_error_new(ERROR_RTC_RTCP, "unknown payload specific feedback=%u", fmt);
        }
    }

    return err;
}

srs_error_t SrsRtcPlayStream::on_rtcp_rr(char* data, int nb_data)
{
    srs_error_t err = srs_success;

    // TODO: FIXME: Implements it.

    session_->stat_->nn_rr++;

    return err;
}

uint32_t SrsRtcPlayStream::get_video_publish_ssrc(uint32_t play_ssrc)
{
    std::map<uint32_t, SrsRtcVideoSendTrack*>::iterator it;
    for (it = video_tracks_.begin(); it != video_tracks_.end(); ++it) {
        if (it->second->has_ssrc(play_ssrc)) {
            return it->first;
        }
    }

    return 0;
}

void SrsRtcPlayStream::set_track_active(const std::vector<SrsTrackConfig>& cfgs)
{
    // set video track inactive
    if (true) {
        std::map<uint32_t, SrsRtcVideoSendTrack*>::iterator it;
        for (it = video_tracks_.begin(); it != video_tracks_.end(); ++it) {
            SrsRtcVideoSendTrack* track = it->second;

            // For example, track is small stream, that is track_id is sophon_video_camera_small,
            // so the merge_track_id is parsed as sophon_video_camera which is the merged stream,
            // if video_group_active_track_ is current track, we should not disable it.
            if (switch_context_->is_track_immutable(track)) {
                continue;
            }

            track->set_track_status(false);
        }
    }

    // set audio track inactive.
    if (true) {
        std::map<uint32_t, SrsRtcAudioSendTrack*>::iterator it;
        for (it = audio_tracks_.begin(); it != audio_tracks_.end(); ++it) {
            SrsRtcAudioSendTrack* track = it->second;
            track->set_track_status(false);
        }
    }

    for (int i = 0; i < cfgs.size(); ++i) {
        const SrsTrackConfig& cfg = cfgs.at(i);

        if (cfg.type_ == "audio") {
            std::map<uint32_t, SrsRtcAudioSendTrack*>::iterator it;
            for (it = audio_tracks_.begin(); it != audio_tracks_.end(); ++it) {
                SrsRtcAudioSendTrack* track = it->second;
                if (track->get_track_id() == cfg.label_) {
                    track->set_track_status(cfg.active);
                }
            }
        }

        if (cfg.type_ == "video") {
            std::map<uint32_t, SrsRtcVideoSendTrack*>::iterator it;
            for (it = video_tracks_.begin(); it != video_tracks_.end(); ++it) {
                SrsRtcVideoSendTrack* track = it->second;

                bool should_active_track = (track->get_track_id() == cfg.label_);
                if (!should_active_track) {
                    continue;
                }

                // If stream will be merged, we will active it in future.
                if (switch_context_->active_it_in_future(track, cfg)) {
                    srs_session_request_keyframe(session_->source_, it->first);
                    continue;
                }

                track->set_track_status(cfg.active);
            }
        }
    }
}

SrsRtcPublishStream::SrsRtcPublishStream(SrsRtcConnection* session)
{
    report_timer = new SrsHourGlass(this, 200 * SRS_UTIME_MILLISECONDS);

    session_ = session;
    request_keyframe_ = false;

    source = NULL;
    nn_simulate_nack_drop = 0;
    nack_enabled_ = false;
    pt_to_drop_ = 0;

    nn_audio_frames = 0;
    twcc_id_ = 0;
    last_twcc_feedback_time_ = 0;
    twcc_fb_count_ = 0;
    is_started = false;
}

SrsRtcPublishStream::~SrsRtcPublishStream()
{
    // TODO: FIXME: Do unpublish when session timeout.
    if (source) {
        source->set_publish_stream(NULL);
        source->on_unpublish();
    }

    srs_freep(req);
    srs_freep(report_timer);
}

srs_error_t SrsRtcPublishStream::initialize(SrsRequest* r, SrsRtcStreamDescription* stream_desc)
{
    srs_error_t err = srs_success;

    req = r->copy();

    audio_tracks_.push_back(new SrsRtcAudioRecvTrack(session_, stream_desc->audio_track_desc_));
    for (int i = 0; i < stream_desc->video_track_descs_.size(); ++i) {
        SrsRtcTrackDescription* desc = stream_desc->video_track_descs_.at(i);
        video_tracks_.push_back(new SrsRtcVideoRecvTrack(session_, desc));
    }

    int twcc_id = -1;
    uint32_t media_ssrc = 0;
    int picture_id = 0;
    // because audio_track_desc have not twcc id, for example, h5demo
    // fetch twcc_id from video track description, 
    for (int i = 0; i < stream_desc->video_track_descs_.size(); ++i) {
        SrsRtcTrackDescription* desc = stream_desc->video_track_descs_.at(i);
        twcc_id = desc->get_rtp_extension_id(kTWCCExt);
        media_ssrc = desc->ssrc_;
        picture_id = desc->get_rtp_extension_id(kPictureIDExt);
        break;
    }
    if (twcc_id != -1) {
        twcc_id_ = twcc_id;
        extension_types_.register_by_uri(twcc_id_, kTWCCExt);
        rtcp_twcc_.set_media_ssrc(media_ssrc);
    }
    if (picture_id) {
        extension_types_.register_by_uri(picture_id, kPictureIDExt);
    }

    nack_enabled_ = _srs_config->get_rtc_nack_enabled(req->vhost);
    pt_to_drop_ = (uint16_t)_srs_config->get_rtc_drop_for_pt(req->vhost);
    bool twcc_enabled = _srs_config->get_rtc_twcc_enabled(req->vhost);

    srs_trace("RTC publisher nack=%d, pt-drop=%u, twcc=%u/%d, picture_id=%u", nack_enabled_, pt_to_drop_, twcc_enabled, twcc_id, picture_id);

    session_->stat_->nn_publishers++;

    return err;
}

srs_error_t SrsRtcPublishStream::start()
{
    srs_error_t err = srs_success;

    // If report_timer started, we think the publisher is started.
    if (is_started) {
        return err;
    }

    if ((err = report_timer->tick(0 * SRS_UTIME_MILLISECONDS)) != srs_success) {
        return srs_error_wrap(err, "hourglass tick");
    }

    if ((err = report_timer->start()) != srs_success) {
        return srs_error_wrap(err, "start report_timer");
    }

    if ((err = _srs_rtc_sources->fetch_or_create(req, &source)) != srs_success) {
        return srs_error_wrap(err, "create source");
    }

    if ((err = source->on_publish()) != srs_success) {
        return srs_error_wrap(err, "on publish");
    }

    source->set_publish_stream(this);

    if (_srs_rtc_hijacker) {
        if ((err = _srs_rtc_hijacker->on_start_publish(session_, this, req)) != srs_success) {
            return srs_error_wrap(err, "on start publish");
        }
    }

    is_started = true;

    return err;
}

srs_error_t SrsRtcPublishStream::send_rtcp_rr()
{
    srs_error_t err = srs_success;

    for (int i = 0; i < video_tracks_.size(); ++i) {
        SrsRtcVideoRecvTrack* track = video_tracks_.at(i);
        track->send_rtcp_rr();
    }

    for (int i = 0; i < audio_tracks_.size(); ++i) {
        SrsRtcAudioRecvTrack* track = audio_tracks_.at(i);
        track->send_rtcp_rr();
    }

    session_->stat_->nn_rr++;

    return err;
}

srs_error_t SrsRtcPublishStream::send_rtcp_xr_rrtr()
{
    srs_error_t err = srs_success;

    for (int i = 0; i < video_tracks_.size(); ++i) {
        SrsRtcVideoRecvTrack* track = video_tracks_.at(i);
        track->send_rtcp_xr_rrtr();
    }

    for (int i = 0; i < audio_tracks_.size(); ++i) {
        SrsRtcAudioRecvTrack* track = audio_tracks_.at(i);
        track->send_rtcp_xr_rrtr();
    }

    session_->stat_->nn_xr++;

    return err;
}

srs_error_t SrsRtcPublishStream::on_twcc(uint16_t sn) {
    srs_error_t err = srs_success;

    srs_utime_t now = srs_get_system_time();
    err = rtcp_twcc_.recv_packet(sn, now);

    session_->stat_->nn_in_twcc++;

    return err;
}

srs_error_t SrsRtcPublishStream::on_rtp(char* data, int nb_data)
{
    srs_error_t err = srs_success;

    session_->stat_->nn_in_rtp++;

    // For NACK simulator, drop packet.
    if (nn_simulate_nack_drop) {
        SrsBuffer b(data, nb_data); SrsRtpHeader h; h.decode(&b);
        simulate_drop_packet(&h, nb_data);
        return err;
    }

    // Decode the header first.
    SrsRtpHeader h;
    if (pt_to_drop_ && twcc_id_) {
        SrsBuffer b(data, nb_data);
        h.ignore_padding(true); h.set_extensions(&extension_types_);
        if ((err = h.decode(&b)) != srs_success) {
            return srs_error_wrap(err, "twcc decode header");
        }
    }

    // We must parse the TWCC from RTP header before SRTP unprotect, because:
    //      1. Client may send some padding packets with invalid SequenceNumber, which causes the SRTP fail.
    //      2. Server may send multiple duplicated NACK to client, and got more than one ARQ packet, which also fail SRTP.
    // so, we must parse the header before SRTP unprotect(which may fail and drop packet).
    if (twcc_id_) {
        uint16_t twcc_sn = 0;
        if ((err = h.get_twcc_sequence_number(twcc_sn)) == srs_success) {
            if((err = on_twcc(twcc_sn)) != srs_success) {
                return srs_error_wrap(err, "on twcc");
            }
        } else {
            srs_error_reset(err);
        }
    }

    // If payload type is configed to drop, ignore this packet.
    if (pt_to_drop_ && pt_to_drop_ == h.get_payload_type()) {
        return err;
    }

    // Decrypt the cipher to plaintext RTP data.
    int nb_unprotected_buf = nb_data;
    char* unprotected_buf = new char[kRtpPacketSize];
    if ((err = session_->transport_->unprotect_rtp(data, unprotected_buf, nb_unprotected_buf)) != srs_success) {
        // We try to decode the RTP header for more detail error informations.
        SrsBuffer b(data, nb_data); SrsRtpHeader h; h.decode(&b);
        err = srs_error_wrap(err, "marker=%u, pt=%u, seq=%u, ts=%u, ssrc=%u, pad=%u, payload=%uB", h.get_marker(), h.get_payload_type(),
            h.get_sequence(), h.get_timestamp(), h.get_ssrc(), h.get_padding(), nb_data - b.pos());

        srs_freepa(unprotected_buf);
        return err;
    }

    if (_srs_blackhole->blackhole) {
        _srs_blackhole->sendto(unprotected_buf, nb_unprotected_buf);
    }

    char* buf = unprotected_buf;
    int nb_buf = nb_unprotected_buf;

    // Decode the RTP packet from buffer.
    SrsRtpPacket2* pkt = new SrsRtpPacket2();
    SrsAutoFree(SrsRtpPacket2, pkt);

    if (true) {
        pkt->set_decode_handler(this);
        pkt->set_extension_types(&extension_types_);
        pkt->shared_msg = new SrsSharedPtrMessage();
        pkt->shared_msg->wrap(buf, nb_buf);

        SrsBuffer b(buf, nb_buf);
        if ((err = pkt->decode(&b)) != srs_success) {
            return srs_error_wrap(err, "decode rtp packet");
        }
    }

    // For source to consume packet.
    uint32_t ssrc = pkt->header.get_ssrc();
    SrsRtcAudioRecvTrack* audio_track = get_audio_track(ssrc);
    SrsRtcVideoRecvTrack* video_track = get_video_track(ssrc);
    if (audio_track) {
        pkt->frame_type = SrsFrameTypeAudio;
        if ((err = audio_track->on_rtp(source, pkt)) != srs_success) {
            return srs_error_wrap(err, "on audio");
        }
    } else if (video_track) {
        pkt->frame_type = SrsFrameTypeVideo;
        if ((err = video_track->on_rtp(source, pkt)) != srs_success) {
            return srs_error_wrap(err, "on video");
        }
    } else {
        return srs_error_new(ERROR_RTC_RTP, "unknown ssrc=%u", ssrc);
    }

    if (_srs_rtc_hijacker) {
        if ((err = _srs_rtc_hijacker->on_rtp_packet(session_, this, req, pkt->copy())) != srs_success) {
            return srs_error_wrap(err, "on rtp packet");
        }
    }

    return err;
}

void SrsRtcPublishStream::on_before_decode_payload(SrsRtpPacket2* pkt, SrsBuffer* buf, ISrsRtpPayloader** ppayload)
{
    // No payload, ignore.
    if (buf->empty()) {
        return;
    }

    uint32_t ssrc = pkt->header.get_ssrc();
    if (get_audio_track(ssrc)) {
        *ppayload = new SrsRtpRawPayload();
    } else if (get_video_track(ssrc)) {
        uint8_t v = (uint8_t)pkt->nalu_type;
        if (v == kStapA) {
            *ppayload = new SrsRtpSTAPPayload();
        } else if (v == kFuA) {
            *ppayload = new SrsRtpFUAPayload2();
        } else {
            *ppayload = new SrsRtpRawPayload();
        }
    }
}

srs_error_t SrsRtcPublishStream::send_periodic_twcc()
{
    srs_error_t err = srs_success;
    srs_utime_t now = srs_get_system_time();
    if(0 == last_twcc_feedback_time_) {
        last_twcc_feedback_time_ = now;
        return err;
    }
    srs_utime_t diff = now - last_twcc_feedback_time_;
    if( diff >= 50 * SRS_UTIME_MILLISECONDS) {
        last_twcc_feedback_time_ = now;
        char pkt[kRtcpPacketSize];
        SrsBuffer *buffer = new SrsBuffer(pkt, sizeof(pkt));
        SrsAutoFree(SrsBuffer, buffer);
        rtcp_twcc_.set_feedback_count(twcc_fb_count_);
        twcc_fb_count_++;
        if((err = rtcp_twcc_.encode(buffer)) != srs_success) {
            return srs_error_wrap(err, "fail to generate twcc feedback packet");
        }
        int nb_protected_buf = buffer->pos();
        char protected_buf[kRtpPacketSize];
        if (session_->transport_->protect_rtcp(pkt, protected_buf, nb_protected_buf) == srs_success) {
            session_->sendonly_skt->sendto(protected_buf, nb_protected_buf, 0);
        }
    }

    return err;
}

srs_error_t SrsRtcPublishStream::on_rtcp(char* data, int nb_data)
{
    srs_error_t err = srs_success;

    char* ph = data;
    int nb_left = nb_data;
    while (nb_left) {
        uint8_t payload_type = ph[1];
        uint16_t length_4bytes = (((uint16_t)ph[2]) << 8) | ph[3];

        int length = (length_4bytes + 1) * 4;

        if (length > nb_data) {
            return srs_error_new(ERROR_RTC_RTCP, "invalid rtcp packet, length=%u", length);
        }

        srs_verbose("on rtcp, payload_type=%u", payload_type);

        switch (payload_type) {
            case kSR: {
                err = on_rtcp_sr(ph, length);
                break;
            }
            case kRR: {
                err = on_rtcp_rr(ph, length);
                break;
            }
            case kSDES: {
                break;
            }
            case kBye: {
                break;
            }
            case kApp: {
                break;
            }
            case kRtpFb: {
                err = on_rtcp_feedback(ph, length);
                break;
            }
            case kPsFb: {
                err = on_rtcp_ps_feedback(ph, length);
                break;
            }
            case kXR: {
                err = on_rtcp_xr(ph, length);
                break;
            }
            default:{
                return srs_error_new(ERROR_RTC_RTCP_CHECK, "unknown rtcp type=%u", payload_type);
                break;
            }
        }

        if (err != srs_success) {
            return srs_error_wrap(err, "rtcp");
        }

        ph += length;
        nb_left -= length;
    }

    return err;
}

srs_error_t SrsRtcPublishStream::on_rtcp_sr(char* buf, int nb_buf)
{
    srs_error_t err = srs_success;

    if (nb_buf < 28) {
        return srs_error_new(ERROR_RTC_RTCP_CHECK, "invalid rtp sender report packet, nb_buf=%d", nb_buf);
    }

    SrsBuffer* stream = new SrsBuffer(buf, nb_buf);
    SrsAutoFree(SrsBuffer, stream);

    // @see: https://tools.ietf.org/html/rfc3550#section-6.4.1
    /*
        0                   1                   2                   3
        0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
       +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
header |V=2|P|    RC   |   PT=SR=200   |             length            |
       +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
       |                         SSRC of sender                        |
       +=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+
sender |              NTP timestamp, most significant word             |
info   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
       |             NTP timestamp, least significant word             |
       +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
       |                         RTP timestamp                         |
       +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
       |                     sender's packet count                     |
       +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
       |                      sender's octet count                     |
       +=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+
report |                 SSRC_1 (SSRC of first source)                 |
block  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  1    | fraction lost |       cumulative number of packets lost       |
       +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
       |           extended highest sequence number received           |
       +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
       |                      interarrival jitter                      |
       +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
       |                         last SR (LSR)                         |
       +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
       |                   delay since last SR (DLSR)                  |
       +=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+
report |                 SSRC_2 (SSRC of second source)                |
block  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  2    :                               ...                             :
       +=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+
       |                  profile-specific extensions                  |
       +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    */
    uint8_t first = stream->read_1bytes();
    uint8_t rc = first & 0x1F;

    uint8_t payload_type = stream->read_1bytes();
    srs_assert(payload_type == kSR);
    uint16_t length = stream->read_2bytes();

    if (((length + 1) * 4) != (rc * 24 + 28)) {
        return srs_error_new(ERROR_RTC_RTCP_CHECK, "invalid rtcp sender report packet, length=%u, rc=%u", length, rc);
    }

    uint32_t ssrc_of_sender = stream->read_4bytes();
    uint64_t ntp = stream->read_8bytes();
    SrsNtp srs_ntp = SrsNtp::to_time_ms(ntp);
    uint32_t rtp_time = stream->read_4bytes();
    uint32_t sender_packet_count = stream->read_4bytes();
    uint32_t sender_octec_count = stream->read_4bytes();

    (void)sender_packet_count; (void)sender_octec_count; (void)rtp_time;
    srs_verbose("sender report, ssrc_of_sender=%u, rtp_time=%u, sender_packet_count=%u, sender_octec_count=%u",
        ssrc_of_sender, rtp_time, sender_packet_count, sender_octec_count);

    for (int i = 0; i < rc; ++i) {
        uint32_t ssrc = stream->read_4bytes();
        uint8_t fraction_lost = stream->read_1bytes();
        uint32_t cumulative_number_of_packets_lost = stream->read_3bytes();
        uint32_t highest_seq = stream->read_4bytes();
        uint32_t jitter = stream->read_4bytes();
        uint32_t lst = stream->read_4bytes();
        uint32_t dlsr = stream->read_4bytes();

        (void)ssrc; (void)fraction_lost; (void)cumulative_number_of_packets_lost; (void)highest_seq; (void)jitter; (void)lst; (void)dlsr;
        srs_verbose("sender report, ssrc=%u, fraction_lost=%u, cumulative_number_of_packets_lost=%u, highest_seq=%u, jitter=%u, lst=%u, dlst=%u",
            ssrc, fraction_lost, cumulative_number_of_packets_lost, highest_seq, jitter, lst, dlsr);
    }

    update_send_report_time(ssrc_of_sender, srs_ntp);

    return err;
}

srs_error_t SrsRtcPublishStream::on_rtcp_xr(char* buf, int nb_buf)
{
    srs_error_t err = srs_success;

    /*
     @see: http://www.rfc-editor.org/rfc/rfc3611.html#section-2

      0                   1                   2                   3
      0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
     +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
     |V=2|P|reserved |   PT=XR=207   |             length            |
     +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
     |                              SSRC                             |
     +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
     :                         report blocks                         :
     +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
     */

    SrsBuffer stream(buf, nb_buf);
    /*uint8_t first = */stream.read_1bytes();
    uint8_t pt = stream.read_1bytes();
    srs_assert(pt == kXR);
    uint16_t length = (stream.read_2bytes() + 1) * 4;
    /*uint32_t ssrc = */stream.read_4bytes();

    if (length != nb_buf) {
        return srs_error_new(ERROR_RTC_RTCP_CHECK, "invalid XR packet, length=%u, nb_buf=%d", length, nb_buf);
    }

    while (stream.pos() + 4 < length) {
        uint8_t bt = stream.read_1bytes();
        stream.skip(1);
        uint16_t block_length = (stream.read_2bytes() + 1) * 4;

        if (stream.pos() + block_length - 4 > nb_buf) {
            return srs_error_new(ERROR_RTC_RTCP_CHECK, "invalid XR packet block, block_length=%u, nb_buf=%d", block_length, nb_buf);
        }

        if (bt == 5) {
            for (int i = 4; i < block_length; i += 12) {
                uint32_t ssrc = stream.read_4bytes();
                uint32_t lrr = stream.read_4bytes();
                uint32_t dlrr = stream.read_4bytes();

                SrsNtp cur_ntp = SrsNtp::from_time_ms(srs_update_system_time() / 1000);
                uint32_t compact_ntp = (cur_ntp.ntp_second_ << 16) | (cur_ntp.ntp_fractions_ >> 16);

                int rtt_ntp = compact_ntp - lrr - dlrr;
                int rtt = ((rtt_ntp * 1000) >> 16) + ((rtt_ntp >> 16) * 1000);
                srs_verbose("ssrc=%u, compact_ntp=%u, lrr=%u, dlrr=%u, rtt=%d",
                    ssrc, compact_ntp, lrr, dlrr, rtt);

                update_rtt(ssrc, rtt);
            }
        }
    }

    return err;
}

srs_error_t SrsRtcPublishStream::on_rtcp_feedback(char* buf, int nb_buf)
{
    srs_error_t err = srs_success;
    // TODO: FIXME: Implements it.
    return err;
}

srs_error_t SrsRtcPublishStream::on_rtcp_ps_feedback(char* buf, int nb_buf)
{
    srs_error_t err = srs_success;

    if (nb_buf < 12) {
        return srs_error_new(ERROR_RTC_RTCP_CHECK, "invalid rtp feedback packet, nb_buf=%d", nb_buf);
    }

    SrsBuffer* stream = new SrsBuffer(buf, nb_buf);
    SrsAutoFree(SrsBuffer, stream);

    uint8_t first = stream->read_1bytes();
    //uint8_t version = first & 0xC0;
    //uint8_t padding = first & 0x20;
    uint8_t fmt = first & 0x1F;

    /*uint8_t payload_type = */stream->read_1bytes();
    /*uint16_t length = */stream->read_2bytes();
    /*uint32_t ssrc_of_sender = */stream->read_4bytes();
    /*uint32_t ssrc_of_media_source = */stream->read_4bytes();

    switch (fmt) {
        case kPLI: {
            srs_verbose("pli");
            break;
        }
        case kSLI: {
            srs_verbose("sli");
            break;
        }
        case kRPSI: {
            srs_verbose("rpsi");
            break;
        }
        case kAFB: {
            srs_verbose("afb");
            break;
        }
        default: {
            return srs_error_new(ERROR_RTC_RTCP, "unknown payload specific feedback=%u", fmt);
        }
    }

    return err;
}

srs_error_t SrsRtcPublishStream::on_rtcp_rr(char* buf, int nb_buf)
{
    srs_error_t err = srs_success;

    if (nb_buf < 8) {
        return srs_error_new(ERROR_RTC_RTCP_CHECK, "invalid rtp receiver report packet, nb_buf=%d", nb_buf);
    }

    SrsBuffer* stream = new SrsBuffer(buf, nb_buf);
    SrsAutoFree(SrsBuffer, stream);

    // @see: https://tools.ietf.org/html/rfc3550#section-6.4.2
    /*
        0                   1                   2                   3
        0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
       +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
header |V=2|P|    RC   |   PT=RR=201   |             length            |
       +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
       |                     SSRC of packet sender                     |
       +=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+
report |                 SSRC_1 (SSRC of first source)                 |
block  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  1    | fraction lost |       cumulative number of packets lost       |
       +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
       |           extended highest sequence number received           |
       +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
       |                      interarrival jitter                      |
       +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
       |                         last SR (LSR)                         |
       +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
       |                   delay since last SR (DLSR)                  |
       +=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+
report |                 SSRC_2 (SSRC of second source)                |
block  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  2    :                               ...                             :
       +=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+
       |                  profile-specific extensions                  |
       +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    */
    uint8_t first = stream->read_1bytes();
    //uint8_t version = first & 0xC0;
    //uint8_t padding = first & 0x20;
    uint8_t rc = first & 0x1F;

    /*uint8_t payload_type = */stream->read_1bytes();
    uint16_t length = stream->read_2bytes();
    /*uint32_t ssrc_of_sender = */stream->read_4bytes();

    if (((length + 1) * 4) != (rc * 24 + 8)) {
        return srs_error_new(ERROR_RTC_RTCP_CHECK, "invalid rtcp receiver packet, length=%u, rc=%u", length, rc);
    }

    for (int i = 0; i < rc; ++i) {
        uint32_t ssrc = stream->read_4bytes();
        uint8_t fraction_lost = stream->read_1bytes();
        uint32_t cumulative_number_of_packets_lost = stream->read_3bytes();
        uint32_t highest_seq = stream->read_4bytes();
        uint32_t jitter = stream->read_4bytes();
        uint32_t lst = stream->read_4bytes();
        uint32_t dlsr = stream->read_4bytes();

        (void)ssrc; (void)fraction_lost; (void)cumulative_number_of_packets_lost; (void)highest_seq; (void)jitter; (void)lst; (void)dlsr;
        srs_verbose("ssrc=%u, fraction_lost=%u, cumulative_number_of_packets_lost=%u, highest_seq=%u, jitter=%u, lst=%u, dlst=%u",
            ssrc, fraction_lost, cumulative_number_of_packets_lost, highest_seq, jitter, lst, dlsr);
    }

    return err;
}

// TODO: FIXME: Use async request PLI to prevent dup requests.
void SrsRtcPublishStream::request_keyframe(uint32_t ssrc)
{
    SrsContextId scid = _srs_context->get_id();
    SrsContextId pcid = session_->context_id();
    srs_trace("RTC play=[%d][%s] SSRC=%u PLI from publish=[%d][%s]", ::getpid(), scid.c_str(), ssrc, ::getpid(), pcid.c_str());

    SrsRtcVideoRecvTrack* video_track = get_video_track(ssrc);
    if (video_track) {
        video_track->request_keyframe();
    }

    session_->stat_->nn_pli++;
}

srs_error_t SrsRtcPublishStream::notify(int type, srs_utime_t interval, srs_utime_t tick)
{
    srs_error_t err = srs_success;

    // TODO: FIXME: Check error.
    send_rtcp_rr();
    send_rtcp_xr_rrtr();

    // TODO: FIXME: Check error.
    // We should not depends on the received packet,
    // instead we should send feedback every Nms.
    send_periodic_twcc();

    return err;
}

void SrsRtcPublishStream::simulate_nack_drop(int nn)
{
    nn_simulate_nack_drop = nn;
}

void SrsRtcPublishStream::simulate_drop_packet(SrsRtpHeader* h, int nn_bytes)
{
    srs_warn("RTC NACK simulator #%d drop seq=%u, ssrc=%u/%s, ts=%u, %d bytes", nn_simulate_nack_drop,
        h->get_sequence(), h->get_ssrc(), (get_video_track(h->get_ssrc())? "Video":"Audio"), h->get_timestamp(),
        nn_bytes);

    nn_simulate_nack_drop--;
}

SrsRtcVideoRecvTrack* SrsRtcPublishStream::get_video_track(uint32_t ssrc)
{
    for (int i = 0; i < video_tracks_.size(); ++i) {
        SrsRtcVideoRecvTrack* track = video_tracks_.at(i);
        if (track->has_ssrc(ssrc)) {
            return track;
        }
    }

    return NULL;
}

SrsRtcAudioRecvTrack* SrsRtcPublishStream::get_audio_track(uint32_t ssrc)
{
    for (int i = 0; i < audio_tracks_.size(); ++i) {
        SrsRtcAudioRecvTrack* track = audio_tracks_.at(i);
        if (track->has_ssrc(ssrc)) {
            return track;
        }
    }

    return NULL;
}

void SrsRtcPublishStream::update_rtt(uint32_t ssrc, int rtt)
{
    SrsRtcVideoRecvTrack* video_track = get_video_track(ssrc);
    if (video_track) {
        return video_track->update_rtt(rtt);
    }

    SrsRtcAudioRecvTrack* audio_track = get_audio_track(ssrc);
    if (audio_track) {
        return audio_track->update_rtt(rtt);
    }
}

void SrsRtcPublishStream::update_send_report_time(uint32_t ssrc, const SrsNtp& ntp)
{
    SrsRtcVideoRecvTrack* video_track = get_video_track(ssrc);
    if (video_track) {
        return video_track->update_send_report_time(ntp);
    }

    SrsRtcAudioRecvTrack* audio_track = get_audio_track(ssrc);
    if (audio_track) {
        return audio_track->update_send_report_time(ntp);
    }
}

SrsRtcConnectionStatistic::SrsRtcConnectionStatistic()
{
    dead = born = srs_get_system_time();
    nn_publishers = nn_subscribers = 0;
    nn_rr = nn_xr = 0;
    nn_sr = nn_nack = nn_pli = 0;
    nn_in_twcc = nn_in_rtp = nn_in_audios = nn_in_videos = 0;
    nn_out_twcc = nn_out_rtp = nn_out_audios = nn_out_videos = 0;
}

SrsRtcConnectionStatistic::~SrsRtcConnectionStatistic()
{
}

string SrsRtcConnectionStatistic::summary()
{
    dead = srs_get_system_time();

    stringstream ss;

    ss << "alive=" << srsu2msi(dead - born) << "ms";

    if (nn_publishers) ss << ", npub=" << nn_publishers;
    if (nn_subscribers) ss << ", nsub=" << nn_subscribers;

    if (nn_rr) ss << ", nrr=" << nn_rr;
    if (nn_xr) ss << ", nxr=" << nn_xr;
    
    if (nn_sr) ss << ", nsr=" << nn_sr;
    if (nn_nack) ss << ", nnack=" << nn_nack;
    if (nn_pli) ss << ", npli=" << nn_pli;

    if (nn_in_twcc) ss << ", in_ntwcc=" << nn_in_twcc;
    if (nn_in_rtp) ss << ", in_nrtp=" << nn_in_rtp;
    if (nn_in_audios) ss << ", in_naudio=" << nn_in_audios;
    if (nn_in_videos) ss << ", in_nvideo=" << nn_in_videos;

    if (nn_out_twcc) ss << ", out_ntwcc=" << nn_out_twcc;
    if (nn_out_rtp) ss << ", out_nrtp=" << nn_out_rtp;
    if (nn_out_audios) ss << ", out_naudio=" << nn_out_audios;
    if (nn_out_videos) ss << ", out_nvideo=" << nn_out_videos;

    return ss.str();
}

SrsRtcConnection::SrsRtcConnection(SrsRtcServer* s, SrsContextId context_id)
{
    req = NULL;
    is_publisher_ = false;
    encrypt = true;
    cid = context_id;
    stat_ = new SrsRtcConnectionStatistic();

    source_ = NULL;
    publisher_ = NULL;
    player_ = NULL;
    sendonly_skt = NULL;
    server_ = s;
    transport_ = new SrsSecurityTransport(this);

    state_ = INIT;
    last_stun_time = 0;
    session_timeout = 0;
    disposing_ = false;

    twcc_id_ = 0;
    nn_simulate_player_nack_drop = 0;
}

SrsRtcConnection::~SrsRtcConnection()
{
    srs_freep(player_);
    srs_freep(publisher_);
    srs_freep(transport_);
    srs_freep(req);
    srs_freep(sendonly_skt);
    srs_freep(stat_);
}

SrsSdp* SrsRtcConnection::get_local_sdp()
{
    return &local_sdp;
}

void SrsRtcConnection::set_local_sdp(const SrsSdp& sdp)
{
    local_sdp = sdp;
}

SrsSdp* SrsRtcConnection::get_remote_sdp()
{
    return &remote_sdp;
}

void SrsRtcConnection::set_remote_sdp(const SrsSdp& sdp)
{
    remote_sdp = sdp;
}

SrsRtcConnectionStateType SrsRtcConnection::state()
{
    return state_;
}

void SrsRtcConnection::set_state(SrsRtcConnectionStateType state)
{
    state_ = state;
}

string SrsRtcConnection::id()
{
    return peer_id_ + "/" + username_;
}


string SrsRtcConnection::peer_id()
{
    return peer_id_;
}

string SrsRtcConnection::username()
{
    return username_;
}

void SrsRtcConnection::set_encrypt(bool v)
{
    encrypt = v;
}

void SrsRtcConnection::switch_to_context()
{
    _srs_context->set_id(cid);
}

SrsContextId SrsRtcConnection::context_id()
{
    return cid;
}

srs_error_t SrsRtcConnection::add_publisher(SrsRequest* req, const SrsSdp& remote_sdp, SrsSdp& local_sdp)
{
    srs_error_t err = srs_success;

    SrsRtcStreamDescription* stream_desc = new SrsRtcStreamDescription();
    SrsAutoFree(SrsRtcStreamDescription, stream_desc);
    if ((err = negotiate_publish_capability(req, remote_sdp, stream_desc)) != srs_success) {
        return srs_error_wrap(err, "publish negotiate");
    }

    if ((err = generate_publish_local_sdp(req, local_sdp, stream_desc)) != srs_success) {
        return srs_error_wrap(err, "generate local sdp");
    }

    SrsRtcStream* source = NULL;
    if ((err = _srs_rtc_sources->fetch_or_create(req, &source)) != srs_success) {
        return srs_error_wrap(err, "create source");
    }

    source->set_stream_desc(stream_desc->copy());

    if ((err = create_publisher(req, stream_desc)) != srs_success) {
        return srs_error_wrap(err, "create publish");
    }

    return err;
}

// TODO: FIXME: Error when play before publishing.
srs_error_t SrsRtcConnection::add_player(SrsRequest* req, const SrsSdp& remote_sdp, SrsSdp& local_sdp)
{
    srs_error_t err = srs_success;
    std::map<uint32_t, SrsRtcTrackDescription*> play_sub_relations;
    if ((err = negotiate_play_capability(req, remote_sdp, play_sub_relations)) != srs_success) {
        return srs_error_wrap(err, "play negotiate");
    }

    if (!play_sub_relations.size()) {
        return srs_error_new(ERROR_RTC_SDP_EXCHANGE, "no play relations");
    }

    SrsRtcStreamDescription* stream_desc = new SrsRtcStreamDescription();
    SrsAutoFree(SrsRtcStreamDescription, stream_desc);
    std::map<uint32_t, SrsRtcTrackDescription*>::iterator it = play_sub_relations.begin();
    while (it != play_sub_relations.end()) {
        SrsRtcTrackDescription* track_desc = it->second;

        if (track_desc->type_ == "audio" || !stream_desc->audio_track_desc_) {
            stream_desc->audio_track_desc_ = track_desc->copy();
        }

        if (track_desc->type_ == "video") {
            stream_desc->video_track_descs_.push_back(track_desc->copy());
        }
        ++it;
    }

    if ((err = generate_play_local_sdp(req, local_sdp, stream_desc)) != srs_success) {
        return srs_error_wrap(err, "generate local sdp");
    }

    if ((err = create_player(req, play_sub_relations)) != srs_success) {
        return srs_error_wrap(err, "create player");
    }

    return err;
}

srs_error_t SrsRtcConnection::add_player2(SrsRequest* req, SrsSdp& local_sdp)
{
    srs_error_t err = srs_success;

    std::map<uint32_t, SrsRtcTrackDescription*> play_sub_relations;
    if ((err = fetch_source_capability(req, play_sub_relations)) != srs_success) {
        return srs_error_wrap(err, "play negotiate");
    }

    if (!play_sub_relations.size()) {
        return srs_error_new(ERROR_RTC_SDP_EXCHANGE, "no play relations");
    }

    SrsRtcStreamDescription* stream_desc = new SrsRtcStreamDescription();
    SrsAutoFree(SrsRtcStreamDescription, stream_desc);

    std::map<uint32_t, SrsRtcTrackDescription*>::iterator it = play_sub_relations.begin();
    while (it != play_sub_relations.end()) {
        SrsRtcTrackDescription* track_desc = it->second;

        if (track_desc->type_ == "audio" || !stream_desc->audio_track_desc_) {
            stream_desc->audio_track_desc_ = track_desc->copy();
        }

        if (track_desc->type_ == "video") {
            stream_desc->video_track_descs_.push_back(track_desc->copy());
        }
        ++it;
    }

    if ((err = generate_play_local_sdp(req, local_sdp, stream_desc)) != srs_success) {
        return srs_error_wrap(err, "generate local sdp");
    }

    if ((err = create_player(req, play_sub_relations)) != srs_success) {
        return srs_error_wrap(err, "create player");
    }

    return err;
}

srs_error_t SrsRtcConnection::initialize(SrsRtcStream* source, SrsRequest* r, bool is_publisher, string username)
{
    srs_error_t err = srs_success;

    username_ = username;
    req = r->copy();
    is_publisher_ = is_publisher;
    source_ = source;

    SrsSessionConfig* cfg = &local_sdp.session_config_;
    if ((err = transport_->initialize(cfg)) != srs_success) {
        return srs_error_wrap(err, "init");
    }

    // TODO: FIXME: Support reload.
    session_timeout = _srs_config->get_rtc_stun_timeout(req->vhost);
    last_stun_time = srs_get_system_time();

    srs_trace("RTC init session, DTLS(role=%s, version=%s), timeout=%dms",
        cfg->dtls_role.c_str(), cfg->dtls_version.c_str(), srsu2msi(session_timeout));

    return err;
}

srs_error_t SrsRtcConnection::on_stun(SrsUdpMuxSocket* skt, SrsStunPacket* r)
{
    srs_error_t err = srs_success;

    if (!r->is_binding_request()) {
        return err;
    }

    last_stun_time = srs_get_system_time();

    // We are running in the ice-lite(server) mode. If client have multi network interface,
    // we only choose one candidate pair which is determined by client.
    if (!sendonly_skt || sendonly_skt->peer_id() != skt->peer_id()) {
        update_sendonly_socket(skt);
    }

    // Write STUN messages to blackhole.
    if (_srs_blackhole->blackhole) {
        _srs_blackhole->sendto(skt->data(), skt->size());
    }

    if ((err = on_binding_request(r)) != srs_success) {
        return srs_error_wrap(err, "stun binding request failed");
    }

    return err;
}

srs_error_t SrsRtcConnection::on_dtls(char* data, int nb_data)
{
    return transport_->on_dtls(data, nb_data);
}

srs_error_t SrsRtcConnection::on_rtcp(char* data, int nb_data)
{
    srs_error_t err = srs_success;

    if (transport_ == NULL) {
        return srs_error_new(ERROR_RTC_RTCP, "recv unexpect rtp packet before dtls done");
    }

    char unprotected_buf[kRtpPacketSize];
    int nb_unprotected_buf = nb_data;
    if ((err = transport_->unprotect_rtcp(data, unprotected_buf, nb_unprotected_buf)) != srs_success) {
        return srs_error_wrap(err, "rtcp unprotect failed");
    }

    if (_srs_blackhole->blackhole) {
        _srs_blackhole->sendto(unprotected_buf, nb_unprotected_buf);
    }

    if (player_) {
        return player_->on_rtcp(unprotected_buf, nb_unprotected_buf);
    }

    if (publisher_) {
        return publisher_->on_rtcp(unprotected_buf, nb_unprotected_buf);
    }

    return err;
}

srs_error_t SrsRtcConnection::on_rtcp_feedback(char* data, int nb_data) 
{
    srs_error_t err = srs_success;

#ifdef SRS_CXX14
    if (!twcc_id_) {
        return err;
    }

    if(srs_success != (err = twcc_controller.on_received_rtcp((uint8_t*)data, nb_data))) {
        return srs_error_wrap(err, "handle twcc feedback rtcp");
    }

    float lossrate = 0.0;
    int bitrate_bps = 0;
    int delay_bitrate_bps = 0;
    int rtt = 0;
    if(srs_success != (err = twcc_controller.get_network_status(lossrate, bitrate_bps, delay_bitrate_bps, rtt))) {
        return srs_error_wrap(err, "get twcc network status");
    }
    srs_verbose("twcc - lossrate:%f, bitrate:%d, delay_bitrate:%d, rtt:%d", lossrate, bitrate_bps, delay_bitrate_bps, rtt);
#endif

    return err;
}

srs_error_t SrsRtcConnection::on_rtp(char* data, int nb_data)
{
    if (publisher_ == NULL) {
        return srs_error_new(ERROR_RTC_RTCP, "rtc publisher null");
    }

    if (transport_ == NULL) {
        return srs_error_new(ERROR_RTC_RTCP, "recv unexpect rtp packet before dtls done");
    }

    //TODO: FIXME: add unprotect_rtcp.
    return publisher_->on_rtp(data, nb_data);
}

srs_error_t SrsRtcConnection::on_connection_established()
{
    srs_error_t err = srs_success;

    srs_trace("RTC %s session=%s, to=%dms connection established", (is_publisher_? "Publisher":"Subscriber"),
        id().c_str(), srsu2msi(session_timeout));

    if (is_publisher_) {
        if ((err = start_publish()) != srs_success) {
            return srs_error_wrap(err, "start publish");
        }
    } else {
        if ((err = start_play()) != srs_success) {
            return srs_error_wrap(err, "start play");
        }
    }

    return err;
}

srs_error_t SrsRtcConnection::start_play()
{
    srs_error_t err = srs_success;

    if ((err = player_->start()) != srs_success) {
        return srs_error_wrap(err, "start");
    }

    return err;
}

srs_error_t SrsRtcConnection::start_publish()
{
    srs_error_t err = srs_success;

    if ((err = publisher_->start()) != srs_success) {
        return srs_error_wrap(err, "start");
    }

    return err;
}

bool SrsRtcConnection::is_stun_timeout()
{
    return last_stun_time + session_timeout < srs_get_system_time();
}

// TODO: FIXME: We should support multiple addresses, because client may use more than one addresses.
void SrsRtcConnection::update_sendonly_socket(SrsUdpMuxSocket* skt)
{
    std::string old_peer_id;
    if (sendonly_skt) {
        srs_trace("session %s address changed, update %s -> %s",
            id().c_str(), sendonly_skt->peer_id().c_str(), skt->peer_id().c_str());
        old_peer_id = sendonly_skt->peer_id();
    }

    // Update the transport.
    srs_freep(sendonly_skt);
    sendonly_skt = skt->copy_sendonly();

    // Update the sessions to handle packets from the new address.
    peer_id_ = sendonly_skt->peer_id();
    server_->insert_into_id_sessions(peer_id_, this);

    // Remove the old address.
    if (!old_peer_id.empty()) {
        server_->remove_id_sessions(old_peer_id);
    }
}

void SrsRtcConnection::check_send_nacks(SrsRtpNackForReceiver* nack, uint32_t ssrc)
{
    // If DTLS is not OK, drop all messages.
    if (!transport_) {
        return;
    }

    // @see: https://tools.ietf.org/html/rfc4585#section-6.1
    vector<uint16_t> nack_seqs;
    nack->get_nack_seqs(nack_seqs);

    vector<uint16_t>::iterator iter = nack_seqs.begin();
    while (iter != nack_seqs.end()) {
        char buf[kRtpPacketSize];
        SrsBuffer stream(buf, sizeof(buf));
        // FIXME: Replace magic number.
        stream.write_1bytes(0x81);
        stream.write_1bytes(kRtpFb);
        stream.write_2bytes(3);
        stream.write_4bytes(ssrc); // TODO: FIXME: Should be 1?
        stream.write_4bytes(ssrc); // TODO: FIXME: Should be 0?
        uint16_t pid = *iter;
        uint16_t blp = 0;
        while (iter + 1 != nack_seqs.end() && (*(iter + 1) - pid <= 15)) {
            blp |= (1 << (*(iter + 1) - pid - 1));
            ++iter;
        }

        stream.write_2bytes(pid);
        stream.write_2bytes(blp);

        if (_srs_blackhole->blackhole) {
            _srs_blackhole->sendto(stream.data(), stream.pos());
        }

        char protected_buf[kRtpPacketSize];
        int nb_protected_buf = stream.pos();

        // FIXME: Merge nack rtcp into one packets.
        if (transport_->protect_rtcp(protected_buf, stream.data(), nb_protected_buf) == srs_success) {
            // TODO: FIXME: Check error.
            sendonly_skt->sendto(protected_buf, nb_protected_buf, 0);
        }

        ++iter;
    }
}

srs_error_t SrsRtcConnection::send_rtcp_rr(uint32_t ssrc, SrsRtpRingBuffer* rtp_queue, const uint64_t& last_send_systime, const SrsNtp& last_send_ntp)
{
    srs_error_t err = srs_success;

    // If DTLS is not OK, drop all messages.
    if (!transport_) {
        return err;
    }

    // @see https://tools.ietf.org/html/rfc3550#section-6.4.2
    char buf[kRtpPacketSize];
    SrsBuffer stream(buf, sizeof(buf));
    stream.write_1bytes(0x81);
    stream.write_1bytes(kRR);
    stream.write_2bytes(7);
    stream.write_4bytes(ssrc); // TODO: FIXME: Should be 1?

    // TODO: FIXME: Implements it.
    // TODO: FIXME: See https://github.com/ossrs/srs/blob/f81d35d20f04ebec01915cb78a882e45b7ee8800/trunk/src/app/srs_app_rtc_queue.cpp
    uint8_t fraction_lost = 0;
    uint32_t cumulative_number_of_packets_lost = 0 & 0x7FFFFF;
    uint32_t extended_highest_sequence = rtp_queue->get_extended_highest_sequence();
    uint32_t interarrival_jitter = 0;

    uint32_t rr_lsr = 0;
    uint32_t rr_dlsr = 0;

    if (last_send_systime > 0) {
        rr_lsr = (last_send_ntp.ntp_second_ << 16) | (last_send_ntp.ntp_fractions_ >> 16);
        uint32_t dlsr = (srs_update_system_time() - last_send_systime) / 1000;
        rr_dlsr = ((dlsr / 1000) << 16) | ((dlsr % 1000) * 65536 / 1000);
    }

    stream.write_4bytes(ssrc);
    stream.write_1bytes(fraction_lost);
    stream.write_3bytes(cumulative_number_of_packets_lost);
    stream.write_4bytes(extended_highest_sequence);
    stream.write_4bytes(interarrival_jitter);
    stream.write_4bytes(rr_lsr);
    stream.write_4bytes(rr_dlsr);

    srs_verbose("RR ssrc=%u, fraction_lost=%u, cumulative_number_of_packets_lost=%u, extended_highest_sequence=%u, interarrival_jitter=%u",
        ssrc, fraction_lost, cumulative_number_of_packets_lost, extended_highest_sequence, interarrival_jitter);

    char protected_buf[kRtpPacketSize];
    int nb_protected_buf = stream.pos();
    if ((err = transport_->protect_rtcp(stream.data(), protected_buf, nb_protected_buf)) != srs_success) {
        return srs_error_wrap(err, "protect rtcp rr");
    }

    // TDOO: FIXME: Check error.
    sendonly_skt->sendto(protected_buf, nb_protected_buf, 0);
    return err;
}

srs_error_t SrsRtcConnection::send_rtcp_xr_rrtr(uint32_t ssrc)
{
    srs_error_t err = srs_success;

    // If DTLS is not OK, drop all messages.
    if (!transport_) {
        return err;
    }

    /*
     @see: http://www.rfc-editor.org/rfc/rfc3611.html#section-2

      0                   1                   2                   3
      0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
     +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
     |V=2|P|reserved |   PT=XR=207   |             length            |
     +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
     |                              SSRC                             |
     +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
     :                         report blocks                         :
     +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+

     @see: http://www.rfc-editor.org/rfc/rfc3611.html#section-4.4

      0                   1                   2                   3
         0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
        +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
        |     BT=4      |   reserved    |       block length = 2        |
        +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
        |              NTP timestamp, most significant word             |
        +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
        |             NTP timestamp, least significant word             |
        +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    */
    srs_utime_t now = srs_update_system_time();
    SrsNtp cur_ntp = SrsNtp::from_time_ms(now / 1000);

    char buf[kRtpPacketSize];
    SrsBuffer stream(buf, sizeof(buf));
    stream.write_1bytes(0x80);
    stream.write_1bytes(kXR);
    stream.write_2bytes(4);
    stream.write_4bytes(ssrc);
    stream.write_1bytes(4);
    stream.write_1bytes(0);
    stream.write_2bytes(2);
    stream.write_4bytes(cur_ntp.ntp_second_);
    stream.write_4bytes(cur_ntp.ntp_fractions_);

    char protected_buf[kRtpPacketSize];
    int nb_protected_buf = stream.pos();
    if ((err = transport_->protect_rtcp(stream.data(), protected_buf, nb_protected_buf)) != srs_success) {
        return srs_error_wrap(err, "protect rtcp xr");
    }

    // TDOO: FIXME: Check error.
    sendonly_skt->sendto(protected_buf, nb_protected_buf, 0);

    return err;
}

srs_error_t SrsRtcConnection::send_rtcp_fb_pli(uint32_t ssrc)
{
    srs_error_t err = srs_success;

    // If DTLS is not OK, drop all messages.
    if (!transport_) {
        return err;
    }

    char buf[kRtpPacketSize];
    SrsBuffer stream(buf, sizeof(buf));
    stream.write_1bytes(0x81);
    stream.write_1bytes(kPsFb);
    stream.write_2bytes(2);
    stream.write_4bytes(ssrc);
    stream.write_4bytes(ssrc);

    srs_trace("RTC PLI ssrc=%u", ssrc);

    if (_srs_blackhole->blackhole) {
        _srs_blackhole->sendto(stream.data(), stream.pos());
    }

    char protected_buf[kRtpPacketSize];
    int nb_protected_buf = stream.pos();
    if ((err = transport_->protect_rtcp(stream.data(), protected_buf, nb_protected_buf)) != srs_success) {
        return srs_error_wrap(err, "protect rtcp psfb pli");
    }

    // TDOO: FIXME: Check error.
    sendonly_skt->sendto(protected_buf, nb_protected_buf, 0);

    return err;
}

void SrsRtcConnection::simulate_nack_drop(int nn)
{
    if (publisher_) {
        publisher_->simulate_nack_drop(nn);
    }

    nn_simulate_player_nack_drop = nn;
}

void SrsRtcConnection::simulate_player_drop_packet(SrsRtpHeader* h, int nn_bytes)
{
    srs_warn("RTC NACK simulator #%d player drop seq=%u, ssrc=%u, ts=%u, %d bytes", nn_simulate_player_nack_drop,
        h->get_sequence(), h->get_ssrc(), h->get_timestamp(),
        nn_bytes);

    nn_simulate_player_nack_drop--;
}

srs_error_t SrsRtcConnection::do_send_packets(const std::vector<SrsRtpPacket2*>& pkts, SrsRtcPlayStreamStatistic& info)
{
    srs_error_t err = srs_success;

    for (int i = 0; i < (int)pkts.size(); i++) {
        SrsRtpPacket2* pkt = pkts.at(i);

        // For this message, select the first iovec.
        iovec* iov = new iovec();
        SrsAutoFree(iovec, iov);

        char* iov_base = new char[kRtpPacketSize];
        SrsAutoFreeA(char, iov_base);

        iov->iov_base = iov_base;
        iov->iov_len = kRtpPacketSize;

        uint16_t twcc_sn = 0;
        // Marshal packet to bytes in iovec.
        if (true) {
#ifdef SRS_CXX14
            // should set twcc sn before packet encode.
            if(twcc_id_) {
                twcc_sn = twcc_controller.allocate_twcc_sn();
                pkt->header.set_twcc_sequence_number(twcc_id_, twcc_sn);
            }
#endif

            SrsBuffer stream((char*)iov->iov_base, iov->iov_len);
            if ((err = pkt->encode(&stream)) != srs_success) {
                return srs_error_wrap(err, "encode packet");
            }
            iov->iov_len = stream.pos();

#ifdef SRS_CXX14
            if(twcc_id_) {
                //store rtp in twcc adaptor
                if(srs_success != (err = twcc_controller.on_pre_send_packet(pkt->header.get_ssrc(),
                    pkt->header.get_sequence(),twcc_sn, stream.pos()))) {
                    return srs_error_wrap(err, "store sending rtp pkt in adaptor");
                }
            }
#endif
        }

        // Whether encrypt the RTP bytes.
        if (encrypt) {
            int nn_encrypt = (int)iov->iov_len;
            if ((err = transport_->protect_rtp2(iov->iov_base, &nn_encrypt)) != srs_success) {
                return srs_error_wrap(err, "srtp protect");
            }
            iov->iov_len = (size_t)nn_encrypt;
        }

        info.nn_rtp_bytes += (int)iov->iov_len;

        // When we send out a packet, increase the stat counter.
        info.nn_rtp_pkts++;

        // For NACK simulator, drop packet.
        if (nn_simulate_player_nack_drop) {
            simulate_player_drop_packet(&pkt->header, (int)iov->iov_len);
            iov->iov_len = 0;
            continue;
        }

        // TODO: FIXME: Handle error.
        sendonly_skt->sendto(iov->iov_base, iov->iov_len, 0);

        // Detail log, should disable it in release version.
        srs_info("RTC: SEND PT=%u, SSRC=%#x, SEQ=%u, Time=%u, %u/%u bytes", pkt->header.get_payload_type(), pkt->header.get_ssrc(),
            pkt->header.get_sequence(), pkt->header.get_timestamp(), pkt->nb_bytes(), iov->iov_len);

#ifdef SRS_CXX14
        if(twcc_id_) {
            if(srs_success != (err = twcc_controller.on_sent_packet(twcc_sn))) {
                return srs_error_wrap(err, "set sent event of rtp pkt in twcc");
            }
        }
#endif
    }

    return err;
}

#ifdef SRS_OSX
// These functions are similar to the older byteorder(3) family of functions.
// For example, be32toh() is identical to ntohl().
// @see https://linux.die.net/man/3/be32toh
#define be32toh ntohl
#endif

srs_error_t SrsRtcConnection::on_binding_request(SrsStunPacket* r)
{
    srs_error_t err = srs_success;

    bool strict_check = _srs_config->get_rtc_stun_strict_check(req->vhost);
    if (strict_check && r->get_ice_controlled()) {
        // @see: https://tools.ietf.org/html/draft-ietf-ice-rfc5245bis-00#section-6.1.3.1
        // TODO: Send 487 (Role Conflict) error response.
        return srs_error_new(ERROR_RTC_STUN, "Peer must not in ice-controlled role in ice-lite mode.");
    }

    SrsStunPacket stun_binding_response;
    char buf[kRtpPacketSize];
    SrsBuffer* stream = new SrsBuffer(buf, sizeof(buf));
    SrsAutoFree(SrsBuffer, stream);

    stun_binding_response.set_message_type(BindingResponse);
    stun_binding_response.set_local_ufrag(r->get_remote_ufrag());
    stun_binding_response.set_remote_ufrag(r->get_local_ufrag());
    stun_binding_response.set_transcation_id(r->get_transcation_id());
    // FIXME: inet_addr is deprecated, IPV6 support
    stun_binding_response.set_mapped_address(be32toh(inet_addr(sendonly_skt->get_peer_ip().c_str())));
    stun_binding_response.set_mapped_port(sendonly_skt->get_peer_port());

    if ((err = stun_binding_response.encode(get_local_sdp()->get_ice_pwd(), stream)) != srs_success) {
        return srs_error_wrap(err, "stun binding response encode failed");
    }

    if ((err = sendonly_skt->sendto(stream->data(), stream->pos(), 0)) != srs_success) {
        return srs_error_wrap(err, "stun binding response send failed");
    }

    if (state_ == WAITING_STUN) {
        state_ = DOING_DTLS_HANDSHAKE;
        srs_trace("RTC session=%s, STUN done, waiting DTLS handshake.", id().c_str());

        if((err = transport_->start_active_handshake()) != srs_success) {
            return srs_error_wrap(err, "fail to dtls handshake");
        }
    }

    if (_srs_blackhole->blackhole) {
        _srs_blackhole->sendto(stream->data(), stream->pos());
    }

    return err;
}

srs_error_t SrsRtcConnection::negotiate_publish_capability(SrsRequest* req, const SrsSdp& remote_sdp, SrsRtcStreamDescription* stream_desc)
{
    srs_error_t err = srs_success;

    if (!stream_desc) {
        return srs_error_new(ERROR_RTC_SDP_EXCHANGE, "stream description is NULL");
    }

    bool nack_enabled = _srs_config->get_rtc_nack_enabled(req->vhost);
    bool twcc_enabled = _srs_config->get_rtc_twcc_enabled(req->vhost);

    for (size_t i = 0; i < remote_sdp.media_descs_.size(); ++i) {
        const SrsMediaDesc& remote_media_desc = remote_sdp.media_descs_[i];

        SrsRtcTrackDescription* track_desc = new SrsRtcTrackDescription();
        SrsAutoFree(SrsRtcTrackDescription, track_desc);

        track_desc->set_direction("recvonly");
        track_desc->set_mid(remote_media_desc.mid_);
        // Whether feature enabled in remote extmap.
        int remote_twcc_id = 0;
        int picture_id = 0;
        if (true) {
            map<int, string> extmaps = remote_media_desc.get_extmaps();
            for(map<int, string>::iterator it = extmaps.begin(); it != extmaps.end(); ++it) {
                if (it->second == kTWCCExt) {
                    remote_twcc_id = it->first;
                } else if(it->second == kPictureIDExt) {
                    picture_id = it->first;
                }
            }
        }

        if (twcc_enabled && remote_twcc_id) {
            track_desc->add_rtp_extension_desc(remote_twcc_id, kTWCCExt);
        }

        if (picture_id) {
            track_desc->add_rtp_extension_desc(picture_id, kPictureIDExt);
        }

        if (remote_media_desc.is_audio()) {
            // TODO: check opus format specific param
            std::vector<SrsMediaPayloadType> payloads = remote_media_desc.find_media_with_encoding_name("opus");
            if (payloads.empty()) {
                return srs_error_new(ERROR_RTC_SDP_EXCHANGE, "no valid found opus payload type");
            }

            for (std::vector<SrsMediaPayloadType>::iterator iter = payloads.begin(); iter != payloads.end(); ++iter) {
                // if the playload is opus, and the encoding_param_ is channel
                SrsAudioPayload* audio_payload = new SrsAudioPayload(iter->payload_type_, iter->encoding_name_, iter->clock_rate_, ::atol(iter->encoding_param_.c_str()));
                audio_payload->set_opus_param_desc(iter->format_specific_param_);
                // TODO: FIXME: Only support some transport algorithms.
                for (int j = 0; j < (int)iter->rtcp_fb_.size(); ++j) {
                    if (nack_enabled) {
                        if (iter->rtcp_fb_.at(j) == "nack" || iter->rtcp_fb_.at(j) == "nack pli") {
                            audio_payload->rtcp_fbs_.push_back(iter->rtcp_fb_.at(j));
                        }
                    }
                    if (twcc_enabled && remote_twcc_id) {
                        if (iter->rtcp_fb_.at(j) == "transport-cc") {
                            audio_payload->rtcp_fbs_.push_back(iter->rtcp_fb_.at(j));
                        }
                    }
                }
                track_desc->type_ = "audio";
                track_desc->set_codec_payload((SrsCodecPayload*)audio_payload);
                // Only choose one match opus codec.
                break;
            }
        } else if (remote_media_desc.is_video()) {
            std::vector<SrsMediaPayloadType> payloads = remote_media_desc.find_media_with_encoding_name("H264");
            if (payloads.empty()) {
                return srs_error_new(ERROR_RTC_SDP_EXCHANGE, "no found valid H.264 payload type");
            }

            std::deque<SrsMediaPayloadType> backup_payloads;
            for (std::vector<SrsMediaPayloadType>::iterator iter = payloads.begin(); iter != payloads.end(); ++iter) {
                if (iter->format_specific_param_.empty()) {
                    backup_payloads.push_front(*iter);
                    continue;
                }
                H264SpecificParam h264_param;
                if ((err = srs_parse_h264_fmtp(iter->format_specific_param_, h264_param)) != srs_success) {
                    srs_error_reset(err); continue;
                }

                // Try to pick the "best match" H.264 payload type.
                if (h264_param.packetization_mode == "1" && h264_param.level_asymmerty_allow == "1") {
                    // if the playload is opus, and the encoding_param_ is channel
                    SrsVideoPayload* video_payload = new SrsVideoPayload(iter->payload_type_, iter->encoding_name_, iter->clock_rate_);
                    video_payload->set_h264_param_desc(iter->format_specific_param_);

                    // TODO: FIXME: Only support some transport algorithms.
                    for (int j = 0; j < (int)iter->rtcp_fb_.size(); ++j) {
                        if (nack_enabled) {
                            if (iter->rtcp_fb_.at(j) == "nack" || iter->rtcp_fb_.at(j) == "nack pli") {
                                video_payload->rtcp_fbs_.push_back(iter->rtcp_fb_.at(j));
                            }
                        }
                        if (twcc_enabled && remote_twcc_id) {
                            if (iter->rtcp_fb_.at(j) == "transport-cc") {
                                video_payload->rtcp_fbs_.push_back(iter->rtcp_fb_.at(j));
                            }
                        }
                    }

                    track_desc->type_ = "video";
                    track_desc->set_codec_payload((SrsCodecPayload*)video_payload);
                    // Only choose first match H.264 payload type.
                    break;
                }

                backup_payloads.push_back(*iter);
            }

            // Try my best to pick at least one media payload type.
            if (!track_desc->media_ && ! backup_payloads.empty()) {
                SrsMediaPayloadType media_pt= backup_payloads.front();
                // if the playload is opus, and the encoding_param_ is channel
                SrsVideoPayload* video_payload = new SrsVideoPayload(media_pt.payload_type_, media_pt.encoding_name_, media_pt.clock_rate_);

                std::vector<std::string> rtcp_fbs = media_pt.rtcp_fb_;
                // TODO: FIXME: Only support some transport algorithms.
                for (int j = 0; j < (int)rtcp_fbs.size(); ++j) {
                    if (nack_enabled) {
                        if (rtcp_fbs.at(j) == "nack" || rtcp_fbs.at(j) == "nack pli") {
                            video_payload->rtcp_fbs_.push_back(rtcp_fbs.at(j));
                        }
                    }

                    if (twcc_enabled && remote_twcc_id) {
                        if (rtcp_fbs.at(j) == "transport-cc") {
                            video_payload->rtcp_fbs_.push_back(rtcp_fbs.at(j));
                        }
                    }
                }

                track_desc->set_codec_payload((SrsCodecPayload*)video_payload);

                srs_warn("choose backup H.264 payload type=%d", backup_payloads.front().payload_type_);
            }

            // TODO: FIXME: Support RRTR?
            //local_media_desc.payload_types_.back().rtcp_fb_.push_back("rrtr");
        }

        // TODO: FIXME: use one parse paylod from sdp.

        track_desc->create_auxiliary_payload(remote_media_desc.find_media_with_encoding_name("red"));
        track_desc->create_auxiliary_payload(remote_media_desc.find_media_with_encoding_name("rtx"));
        track_desc->create_auxiliary_payload(remote_media_desc.find_media_with_encoding_name("ulpfec"));
        track_desc->create_auxiliary_payload(remote_media_desc.find_media_with_encoding_name("rsfec"));

        std::string track_id;
        for (int i = 0; i < remote_media_desc.ssrc_infos_.size(); ++i) {
            SrsSSRCInfo ssrc_info = remote_media_desc.ssrc_infos_.at(i);
            // ssrc have same track id, will be description in the same track description.
            if(track_id != ssrc_info.msid_tracker_) {
                SrsRtcTrackDescription* track_desc_copy = track_desc->copy();
                track_desc_copy->ssrc_ = ssrc_info.ssrc_;
                track_desc_copy->id_ = ssrc_info.msid_tracker_;
                track_desc_copy->msid_ = ssrc_info.msid_;

                if (remote_media_desc.is_audio() && !stream_desc->audio_track_desc_) {
                    stream_desc->audio_track_desc_ = track_desc_copy;
                } else if (remote_media_desc.is_video()) {
                    stream_desc->video_track_descs_.push_back(track_desc_copy);
                }
            }
            track_id = ssrc_info.msid_tracker_;
        }

        // set track fec_ssrc and rtx_ssrc
        for (int i = 0; i < remote_media_desc.ssrc_groups_.size(); ++i) {
            SrsSSRCGroup ssrc_group = remote_media_desc.ssrc_groups_.at(i);
            SrsRtcTrackDescription* track_desc = stream_desc->find_track_description_by_ssrc(ssrc_group.ssrcs_[0]);
            if (!track_desc) {
                continue;
            }

            if (ssrc_group.semantic_ == "FID") {
                track_desc->set_rtx_ssrc(ssrc_group.ssrcs_[1]);
            } else if (ssrc_group.semantic_ == "FEC") {
                track_desc->set_fec_ssrc(ssrc_group.ssrcs_[1]);
            }
        }
    }

    return err;
}

srs_error_t SrsRtcConnection::generate_publish_local_sdp(SrsRequest* req, SrsSdp& local_sdp, SrsRtcStreamDescription* stream_desc)
{
    srs_error_t err = srs_success;

    if (!stream_desc) {
        return srs_error_new(ERROR_RTC_SDP_EXCHANGE, "stream description is NULL");
    }

    local_sdp.version_ = "0";

    local_sdp.username_        = RTMP_SIG_SRS_SERVER;
    local_sdp.session_id_      = srs_int2str((int64_t)this);
    local_sdp.session_version_ = "2";
    local_sdp.nettype_         = "IN";
    local_sdp.addrtype_        = "IP4";
    local_sdp.unicast_address_ = "0.0.0.0";

    local_sdp.session_name_ = "SRSPublishSession";

    local_sdp.msid_semantic_ = "WMS";
    std::string stream_id = req->app + "/" + req->stream;
    local_sdp.msids_.push_back(stream_id);

    local_sdp.group_policy_ = "BUNDLE";

    // generate audio media desc
    if (stream_desc->audio_track_desc_) {
        SrsRtcTrackDescription* audio_track = stream_desc->audio_track_desc_;

        local_sdp.media_descs_.push_back(SrsMediaDesc("audio"));
        SrsMediaDesc& local_media_desc = local_sdp.media_descs_.back();

        local_media_desc.port_ = 9;
        local_media_desc.protos_ = "UDP/TLS/RTP/SAVPF";
        local_media_desc.rtcp_mux_ = true;
        local_media_desc.rtcp_rsize_ = true;

        local_media_desc.mid_ = audio_track->mid_;
        local_sdp.groups_.push_back(local_media_desc.mid_);

        // anwer not need set stream_id and track_id;
        // local_media_desc.msid_ = stream_id;
        // local_media_desc.msid_tracker_ = audio_track->id_;
        local_media_desc.extmaps_ = audio_track->extmaps_;

        if (audio_track->direction_ == "recvonly") {
            local_media_desc.recvonly_ = true;
        } else if (audio_track->direction_ == "sendonly") {
            local_media_desc.sendonly_ = true;
        } else if (audio_track->direction_ == "sendrecv") {
            local_media_desc.sendrecv_ = true;
        } else if (audio_track->direction_ == "inactive_") {
            local_media_desc.inactive_ = true;
        }

        SrsAudioPayload* payload = (SrsAudioPayload*)audio_track->media_;
        local_media_desc.payload_types_.push_back(payload->generate_media_payload_type());
    }

    for (int i = 0;  i < stream_desc->video_track_descs_.size(); ++i) {
        SrsRtcTrackDescription* video_track = stream_desc->video_track_descs_.at(i);

        local_sdp.media_descs_.push_back(SrsMediaDesc("video"));
        SrsMediaDesc& local_media_desc = local_sdp.media_descs_.back();

        local_media_desc.port_ = 9;
        local_media_desc.protos_ = "UDP/TLS/RTP/SAVPF";
        local_media_desc.rtcp_mux_ = true;
        local_media_desc.rtcp_rsize_ = true;

        local_media_desc.mid_ = video_track->mid_;
        local_sdp.groups_.push_back(local_media_desc.mid_);

        // anwer not need set stream_id and track_id;
        //local_media_desc.msid_ = stream_id;
        //local_media_desc.msid_tracker_ = video_track->id_;
        local_media_desc.extmaps_ = video_track->extmaps_;

        if (video_track->direction_ == "recvonly") {
            local_media_desc.recvonly_ = true;
        } else if (video_track->direction_ == "sendonly") {
            local_media_desc.sendonly_ = true;
        } else if (video_track->direction_ == "sendrecv") {
            local_media_desc.sendrecv_ = true;
        } else if (video_track->direction_ == "inactive_") {
            local_media_desc.inactive_ = true;
        }

        SrsVideoPayload* payload = (SrsVideoPayload*)video_track->media_;
        local_media_desc.payload_types_.push_back(payload->generate_media_payload_type());

        if (video_track->red_) {
            SrsRedPayload* payload = (SrsRedPayload*)video_track->red_;
            local_media_desc.payload_types_.push_back(payload->generate_media_payload_type());
        }

        if (video_track->rsfec_) {
            SrsCodecPayload* payload = (SrsCodecPayload*)video_track->rsfec_;
            local_media_desc.payload_types_.push_back(payload->generate_media_payload_type());
        }

        // only need media desc info, not ssrc info;
        break;
    }

    return err;
}

srs_error_t SrsRtcConnection::negotiate_play_capability(SrsRequest* req, const SrsSdp& remote_sdp, std::map<uint32_t, SrsRtcTrackDescription*>& sub_relations)
{
    srs_error_t err = srs_success;

    bool nack_enabled = _srs_config->get_rtc_nack_enabled(req->vhost);
    bool twcc_enabled = _srs_config->get_rtc_twcc_enabled(req->vhost);

    SrsRtcStream* source = NULL;
    if ((err = _srs_rtc_sources->fetch_or_create(req, &source)) != srs_success) {
        return srs_error_wrap(err, "fetch rtc source");
    }

    // for need merged track, use the same ssrc
    uint32_t merged_track_ssrc = SrsRtcSSRCGenerator::instance()->generate_ssrc();

    for (size_t i = 0; i < remote_sdp.media_descs_.size(); ++i) {
        const SrsMediaDesc& remote_media_desc = remote_sdp.media_descs_[i];
        // Whether feature enabled in remote extmap.
        int remote_twcc_id = 0;
        if (true) {
            map<int, string> extmaps = remote_media_desc.get_extmaps();
            for(map<int, string>::iterator it = extmaps.begin(); it != extmaps.end(); ++it) {
                if (it->second == kTWCCExt) {
                    remote_twcc_id = it->first;
                    break;
                }
            }
        }

        std::vector<SrsRtcTrackDescription*> track_descs;
        std::vector<std::string> remote_rtcp_fb;
        if (remote_media_desc.is_audio()) {
            // TODO: check opus format specific param
            std::vector<SrsMediaPayloadType> payloads = remote_media_desc.find_media_with_encoding_name("opus");
            if (payloads.empty()) {
                return srs_error_new(ERROR_RTC_SDP_EXCHANGE, "no valid found opus payload type");
            }

            SrsMediaPayloadType payload = payloads.at(0);
            remote_rtcp_fb = payload.rtcp_fb_;

            track_descs = source->get_track_desc("audio", "opus");
        } else if (remote_media_desc.is_video()) {
            // TODO: check opus format specific param
            std::vector<SrsMediaPayloadType> payloads = remote_media_desc.find_media_with_encoding_name("H264");
            if (payloads.empty()) {
                return srs_error_new(ERROR_RTC_SDP_EXCHANGE, "no valid found opus payload type");
            }

            SrsMediaPayloadType payload = payloads.at(0);
            remote_rtcp_fb = payload.rtcp_fb_;

            track_descs = source->get_track_desc("video", "H264");
        }

        for (int i = 0; i < track_descs.size(); ++i) {
            SrsRtcTrackDescription* track = track_descs[i]->copy();
            track->mid_ = remote_media_desc.mid_;
            uint32_t publish_ssrc = track->ssrc_;

            vector<string> rtcp_fb;
            track->media_->rtcp_fbs_.swap(rtcp_fb);
            for (int j = 0; j < (int)rtcp_fb.size(); j++) {
                if (nack_enabled) {
                    if (rtcp_fb.at(j) == "nack" || rtcp_fb.at(j) == "nack pli") {
                        track->media_->rtcp_fbs_.push_back(rtcp_fb.at(j));
                    }
                }
                if (twcc_enabled && remote_twcc_id) {
                    if (rtcp_fb.at(j) == "transport-cc") {
                        track->media_->rtcp_fbs_.push_back(rtcp_fb.at(j));
                    }
                    track->add_rtp_extension_desc(remote_twcc_id, kTWCCExt);
                }
            }

            if (_srs_track_id_group->get_merged_track_id(track->id_) != track->id_) {
                track->ssrc_ = merged_track_ssrc;
            } else {
                track->ssrc_ = SrsRtcSSRCGenerator::instance()->generate_ssrc();
            }
            
            // TODO: FIXME: set audio_payload rtcp_fbs_,
            // according by whether downlink is support transport algorithms.
            // TODO: FIXME: if we support downlink RTX, MUST assign rtx_ssrc_, rtx_pt, rtx_apt
            // not support rtx
            if (true) {
                srs_freep(track->rtx_);
                track->rtx_ssrc_ = 0;
            }

            track->set_direction("sendonly");
            sub_relations.insert(make_pair(publish_ssrc, track));
        }
    }

    return err;
}

srs_error_t SrsRtcConnection::fetch_source_capability(SrsRequest* req, std::map<uint32_t, SrsRtcTrackDescription*>& sub_relations)
{
    srs_error_t err = srs_success;

    bool nack_enabled = _srs_config->get_rtc_nack_enabled(req->vhost);
    bool twcc_enabled = _srs_config->get_rtc_twcc_enabled(req->vhost);

    SrsRtcStream* source = NULL;
    if ((err = _srs_rtc_sources->fetch_or_create(req, &source)) != srs_success) {
        return srs_error_wrap(err, "fetch rtc source");
    }

    // for need merged track, use the same ssrc
    uint32_t merged_track_ssrc = SrsRtcSSRCGenerator::instance()->generate_ssrc();

    std::vector<SrsRtcTrackDescription*> track_descs = source->get_track_desc("audio", "opus");
    std::vector<SrsRtcTrackDescription*> video_track_desc = source->get_track_desc("video", "H264");
    
    track_descs.insert(track_descs.end(), video_track_desc.begin(), video_track_desc.end());
    for (int i = 0; i < track_descs.size(); ++i) {
        SrsRtcTrackDescription* track = track_descs[i]->copy();
        uint32_t publish_ssrc = track->ssrc_;

        int local_twcc_id = track->get_rtp_extension_id(kTWCCExt);

        vector<string> rtcp_fb;
        track->media_->rtcp_fbs_.swap(rtcp_fb);
        for (int j = 0; j < (int)rtcp_fb.size(); j++) {
            if (nack_enabled) {
                if (rtcp_fb.at(j) == "nack" || rtcp_fb.at(j) == "nack pli") {
                    track->media_->rtcp_fbs_.push_back(rtcp_fb.at(j));
                }
            }
            if (twcc_enabled && local_twcc_id) {
                if (rtcp_fb.at(j) == "transport-cc") {
                    track->media_->rtcp_fbs_.push_back(rtcp_fb.at(j));
                }
                track->add_rtp_extension_desc(local_twcc_id, kTWCCExt);
            }
        }

        if (_srs_track_id_group->get_merged_track_id(track->id_) != track->id_) {
            track->ssrc_ = merged_track_ssrc;
        } else {
            track->ssrc_ = SrsRtcSSRCGenerator::instance()->generate_ssrc();
        }

        // TODO: FIXME: set audio_payload rtcp_fbs_,
        // according by whether downlink is support transport algorithms.
        // TODO: FIXME: if we support downlink RTX, MUST assign rtx_ssrc_, rtx_pt, rtx_apt
        // not support rtx
        srs_freep(track->rtx_);
        track->rtx_ssrc_ = 0;

        int local_picture_id = track->get_rtp_extension_id(kPictureIDExt);
        if (local_picture_id) {
            track->add_rtp_extension_desc(local_picture_id, kPictureIDExt);
        }

        track->set_direction("sendonly");
        sub_relations.insert(make_pair(publish_ssrc, track));
    }

    return err;
}

srs_error_t SrsRtcConnection::generate_play_local_sdp(SrsRequest* req, SrsSdp& local_sdp, SrsRtcStreamDescription* stream_desc)
{
    srs_error_t err = srs_success;

    if (!stream_desc) {
        return srs_error_new(ERROR_RTC_SDP_EXCHANGE, "stream description is NULL");
    }

    local_sdp.version_ = "0";

    local_sdp.username_        = RTMP_SIG_SRS_SERVER;
    local_sdp.session_id_      = srs_int2str((int64_t)this);
    local_sdp.session_version_ = "2";
    local_sdp.nettype_         = "IN";
    local_sdp.addrtype_        = "IP4";
    local_sdp.unicast_address_ = "0.0.0.0";

    local_sdp.session_name_ = "SRSPlaySession";

    local_sdp.msid_semantic_ = "WMS";
    std::string stream_id = req->app + "/" + req->stream;
    local_sdp.msids_.push_back(stream_id);

    local_sdp.group_policy_ = "BUNDLE";

    std::string cname = srs_random_str(16);

    bool track_merged = false;
    // generate audio media desc
    if (stream_desc->audio_track_desc_) {
        SrsRtcTrackDescription* audio_track = stream_desc->audio_track_desc_;

        local_sdp.media_descs_.push_back(SrsMediaDesc("audio"));
        SrsMediaDesc& local_media_desc = local_sdp.media_descs_.back();

        local_media_desc.port_ = 9;
        local_media_desc.protos_ = "UDP/TLS/RTP/SAVPF";
        local_media_desc.rtcp_mux_ = true;
        local_media_desc.rtcp_rsize_ = true;

        local_media_desc.extmaps_ = audio_track->extmaps_;

        local_media_desc.mid_ = audio_track->mid_;
        local_sdp.groups_.push_back(local_media_desc.mid_);

        if (audio_track->direction_ == "recvonly") {
            local_media_desc.recvonly_ = true;
        } else if (audio_track->direction_ == "sendonly") {
            local_media_desc.sendonly_ = true;
        } else if (audio_track->direction_ == "sendrecv") {
            local_media_desc.sendrecv_ = true;
        } else if (audio_track->direction_ == "inactive_") {
            local_media_desc.inactive_ = true;
        }

        if (audio_track->red_) {
            SrsRedPayload* red_payload = (SrsRedPayload*)audio_track->red_;
            local_media_desc.payload_types_.push_back(red_payload->generate_media_payload_type());
        }

        SrsAudioPayload* payload = (SrsAudioPayload*)audio_track->media_;
        local_media_desc.payload_types_.push_back(payload->generate_media_payload_type()); 

        //TODO: FIXME: add red, rtx, ulpfec, rsfec..., payload_types_.
        //local_media_desc.payload_types_.push_back(payload->generate_media_payload_type());

        local_media_desc.ssrc_infos_.push_back(SrsSSRCInfo(audio_track->ssrc_, cname, audio_track->msid_, audio_track->id_));

        if (audio_track->rtx_) {
            std::vector<uint32_t> group_ssrcs;
            group_ssrcs.push_back(audio_track->ssrc_);
            group_ssrcs.push_back(audio_track->rtx_ssrc_);

            local_media_desc.ssrc_groups_.push_back(SrsSSRCGroup("FID", group_ssrcs));
            local_media_desc.ssrc_infos_.push_back(SrsSSRCInfo(audio_track->rtx_ssrc_, cname, audio_track->msid_, audio_track->id_));
        }

        if (audio_track->ulpfec_ || audio_track->rsfec_) {
            std::vector<uint32_t> group_ssrcs;
            group_ssrcs.push_back(audio_track->ssrc_);
            group_ssrcs.push_back(audio_track->fec_ssrc_);
            local_media_desc.ssrc_groups_.push_back(SrsSSRCGroup("FEC", group_ssrcs));

            local_media_desc.ssrc_infos_.push_back(SrsSSRCInfo(audio_track->fec_ssrc_, cname, audio_track->msid_, audio_track->id_));
        }
    }

    for (int i = 0;  i < stream_desc->video_track_descs_.size(); ++i) {
        SrsRtcTrackDescription* track = stream_desc->video_track_descs_[i];

        // for plan b, we only add one m=
        if (i == 0) {
            local_sdp.media_descs_.push_back(SrsMediaDesc("video"));
            SrsMediaDesc& local_media_desc = local_sdp.media_descs_.back();

            local_media_desc.port_ = 9;
            local_media_desc.protos_ = "UDP/TLS/RTP/SAVPF";
            local_media_desc.rtcp_mux_ = true;
            local_media_desc.rtcp_rsize_ = true;

            local_media_desc.extmaps_ = track->extmaps_;

            local_media_desc.mid_ = track->mid_;
            local_sdp.groups_.push_back(local_media_desc.mid_);

            if (track->direction_ == "recvonly") {
                local_media_desc.recvonly_ = true;
            } else if (track->direction_ == "sendonly") {
                local_media_desc.sendonly_ = true;
            } else if (track->direction_ == "sendrecv") {
                local_media_desc.sendrecv_ = true;
            } else if (track->direction_ == "inactive_") {
                local_media_desc.inactive_ = true;
            }

            SrsVideoPayload* payload = (SrsVideoPayload*)track->media_;

            local_media_desc.payload_types_.push_back(payload->generate_media_payload_type());

            if (track->red_) {
                SrsRedPayload* red_payload = (SrsRedPayload*)track->red_;
                local_media_desc.payload_types_.push_back(red_payload->generate_media_payload_type());
            }

            if (track->rsfec_) {
                SrsCodecPayload* payload = (SrsCodecPayload*)track->rsfec_;
                local_media_desc.payload_types_.push_back(payload->generate_media_payload_type());
            }
        }

        SrsMediaDesc& local_media_desc = local_sdp.media_descs_.back();

        // only add merge track to sdp
        std::string merged_track_id = _srs_track_id_group->get_merged_track_id(track->id_);
        if (merged_track_id != track->id_) {
            if (track_merged) {
                continue;
            }
            track->id_ = merged_track_id;
            track_merged = true;
        }
        local_media_desc.ssrc_infos_.push_back(SrsSSRCInfo(track->ssrc_, cname, track->msid_, track->id_));

        if (track->rtx_ && track->rtx_ssrc_) {
            std::vector<uint32_t> group_ssrcs;
            group_ssrcs.push_back(track->ssrc_);
            group_ssrcs.push_back(track->rtx_ssrc_);

            local_media_desc.ssrc_groups_.push_back(SrsSSRCGroup("FID", group_ssrcs));
            local_media_desc.ssrc_infos_.push_back(SrsSSRCInfo(track->rtx_ssrc_, cname, track->msid_, track->id_));
        }

        if ((track->ulpfec_ || track->rsfec_) && track->fec_ssrc_) {
            std::vector<uint32_t> group_ssrcs;
            group_ssrcs.push_back(track->ssrc_);
            group_ssrcs.push_back(track->fec_ssrc_);
            local_media_desc.ssrc_groups_.push_back(SrsSSRCGroup("FEC", group_ssrcs));

            local_media_desc.ssrc_infos_.push_back(SrsSSRCInfo(track->fec_ssrc_, cname, track->msid_, track->id_));
        }
    }

    return err;
}

srs_error_t SrsRtcConnection::create_player(SrsRequest* req, std::map<uint32_t, SrsRtcTrackDescription*> sub_relations)
{
    srs_error_t err = srs_success;

    if (player_) {
        return err;
    }

    player_ = new SrsRtcPlayStream(this, _srs_context->get_id());
    if ((err = player_->initialize(req, sub_relations)) != srs_success) {
        return srs_error_wrap(err, "SrsRtcPlayStream init");
    }

    // TODO: FIXME: Support reload.
    // The TWCC ID is the ext-map ID in local SDP, and we set to enable GCC.
    // Whatever the ext-map, we will disable GCC when config disable it.
    int twcc_id = 0;
    if (true) {
        std::map<uint32_t, SrsRtcTrackDescription*>::iterator it = sub_relations.begin();
        while (it != sub_relations.end()) {
            if (it->second->type_ == "video") {
                SrsRtcTrackDescription* track = it->second;
                twcc_id = track->get_rtp_extension_id(kTWCCExt);
            }
            ++it;
        }
    }
    bool gcc_enabled = _srs_config->get_rtc_gcc_enabled(req->vhost);
    if (gcc_enabled) {
        twcc_id_ = twcc_id;
    }
    srs_trace("RTC connection player gcc=%u/%d", gcc_enabled, twcc_id);

#ifdef SRS_CXX14
    if(twcc_id_) {
        if(srs_success != (err = create_twcc_handler())) {
            return srs_error_wrap(err, "create twcc hanlder");
        }
    }
#endif

    return err;
}

srs_error_t SrsRtcConnection::create_publisher(SrsRequest* req, SrsRtcStreamDescription* stream_desc)
{
    srs_error_t err = srs_success;

    if (!stream_desc) {
        return srs_error_new(ERROR_RTC_STREAM_DESC, "rtc publisher init");
    }

    if (publisher_) {
        return err;
    }

    publisher_ = new SrsRtcPublishStream(this);
    if ((err = publisher_->initialize(req, stream_desc)) != srs_success) {
        return srs_error_wrap(err, "rtc publisher init");
    }

    return err;
}

srs_error_t SrsRtcConnection::set_play_track_active(const std::vector<SrsTrackConfig>& cfgs)
{
    srs_error_t err = srs_success;

    if (!player_) {
        return srs_error_new(ERROR_RTC_NO_PLAYER, "set play track");
    }

    player_->set_track_active(cfgs);

    return err;
}

#ifdef SRS_CXX14
srs_error_t SrsRtcConnection::create_twcc_handler()
{
    srs_error_t err = srs_success;

    if(srs_success != (err = twcc_controller.initialize())) {
        return srs_error_wrap(err, "fail to initial twcc controller");
    }

    return err;
}
#endif

ISrsRtcHijacker::ISrsRtcHijacker()
{
}

ISrsRtcHijacker::~ISrsRtcHijacker()
{
}

ISrsRtcHijacker* _srs_rtc_hijacker = NULL;

