/*
 * test_sdp.c - host unit tests for ngx_rtc_sdp.c (offer parser and answer
 * generator).
 */

#include "ngx_rtc_test.h"
#include "ngx_rtc_sdp.h"

static const char g_offer[] =
    "v=0\r\n"
    "o=- 0 0 IN IP4 127.0.0.1\r\n"
    "s=-\r\n"
    "t=0 0\r\n"
    "a=group:BUNDLE 0 1\r\n"
    "a=msid-semantic: WMS\r\n"
    "a=ice-ufrag:offerUfrag\r\n"
    "a=ice-pwd:offerPwd123\r\n"
    "a=fingerprint:sha-256 AB:CD:EF:01:02:03\r\n"
    "a=setup:actpass\r\n"
    "m=video 9 UDP/TLS/RTP/SAVPF 96 102\r\n"
    "a=rtpmap:96 VP8/90000\r\n"
    "a=rtpmap:102 H264/90000\r\n"
    "a=fmtp:102 level-asymmetry-allowed=1;packetization-mode=1\r\n"
    "a=ssrc:12345 cname:videoCname\r\n"
    "m=audio 9 UDP/TLS/RTP/SAVPF 111\r\n"
    "a=rtpmap:111 opus/48000/2\r\n"
    "a=fmtp:111 minptime=10;useinbandfec=1\r\n"
    "a=ssrc:67890 cname:audioCname\r\n";

static int str_contains(const char *haystack, const char *needle)
{
    return (NULL != strstr(haystack, needle)) ? 1 : 0;
}

NGX_RTC_TEST(sdp_parse_offer)
{
    ngx_rtc_sdp_offer_t offer;

    NGX_RTC_TEST_ASSERT_I64_EQ(
        ngx_rtc_sdp_parse_offer(g_offer, (uint32_t)strlen(g_offer), &offer),
        NGX_RTC_OK);

    NGX_RTC_TEST_ASSERT_STR_EQ(offer.session.ice_ufrag, "offerUfrag");
    NGX_RTC_TEST_ASSERT_STR_EQ(offer.session.ice_pwd, "offerPwd123");
    NGX_RTC_TEST_ASSERT_STR_EQ(offer.session.fingerprint_algo, "sha-256");
    NGX_RTC_TEST_ASSERT_STR_EQ(offer.session.fingerprint, "AB:CD:EF:01:02:03");
    NGX_RTC_TEST_ASSERT_STR_EQ(offer.session.setup, "actpass");

    NGX_RTC_TEST_ASSERT_I64_EQ(offer.video_pt, 102);
    NGX_RTC_TEST_ASSERT_U64_EQ(offer.video_ssrc, 12345u);
    NGX_RTC_TEST_ASSERT_U64_EQ(offer.video_clock_rate, 90000u);
    NGX_RTC_TEST_ASSERT_STR_EQ(offer.video_encoding, "H264");
    NGX_RTC_TEST_ASSERT_STR_EQ(offer.video_fmtp,
                               "level-asymmetry-allowed=1;packetization-mode=1");

    NGX_RTC_TEST_ASSERT_I64_EQ(offer.audio_pt, 111);
    NGX_RTC_TEST_ASSERT_U64_EQ(offer.audio_ssrc, 67890u);
    NGX_RTC_TEST_ASSERT_U64_EQ(offer.audio_clock_rate, 48000u);
    NGX_RTC_TEST_ASSERT_I64_EQ(offer.audio_channels, 2);
    NGX_RTC_TEST_ASSERT_STR_EQ(offer.audio_encoding, "opus");
    NGX_RTC_TEST_ASSERT_STR_EQ(offer.audio_fmtp, "minptime=10;useinbandfec=1");
}

NGX_RTC_TEST(sdp_parse_offer_lf_only)
{
    static const char offer_lf[] =
        "v=0\n"
        "o=- 0 0 IN IP4 127.0.0.1\n"
        "s=-\n"
        "t=0 0\n"
        "a=ice-ufrag:lfUfrag\n"
        "a=ice-pwd:lfPwd\n"
        "m=video 9 RTP/AVP 102\n"
        "a=rtpmap:102 H264/90000\n"
        "m=audio 9 RTP/AVP 111\n"
        "a=rtpmap:111 opus/48000/2\n";
    ngx_rtc_sdp_offer_t offer;

    NGX_RTC_TEST_ASSERT_I64_EQ(
        ngx_rtc_sdp_parse_offer(offer_lf, (uint32_t)strlen(offer_lf), &offer),
        NGX_RTC_OK);
    NGX_RTC_TEST_ASSERT_STR_EQ(offer.session.ice_ufrag, "lfUfrag");
    NGX_RTC_TEST_ASSERT_I64_EQ(offer.video_pt, 102);
    NGX_RTC_TEST_ASSERT_I64_EQ(offer.audio_pt, 111);
    NGX_RTC_TEST_ASSERT_I64_EQ(offer.audio_channels, 2);
}

/* A Chrome 143 offer lists every supported codec + its rtx/red variants, so a
 * single media section carries 20+ payload types. The parser must not reject it
 * (regression: NGX_RTC_SDP_MAX_PT was 8 and overflowed on real Chrome offers). */
NGX_RTC_TEST(sdp_parse_offer_chrome_many_codecs)
{
    static const char chrome_offer[] =
        "v=0\r\n"
        "o=- 4611731400430051336 2 IN IP4 127.0.0.1\r\n"
        "s=-\r\n"
        "t=0 0\r\n"
        "a=group:BUNDLE 0 1\r\n"
        "a=extmap-allow-mixed\r\n"
        "a=msid-semantic: WMS\r\n"
        "a=ice-ufrag:abcd\r\n"
        "a=ice-pwd:asd88fgpdd777uzjYhagZg\r\n"
        "a=ice-options:trickle\r\n"
        "a=fingerprint:sha-256 A1:1F:2E:3D:4C:5B:6A:79:88:97:A6:B5:C4:D3:E2:F1:00:11:22:33:44:55:66:77:88:99:AA:BB\r\n"
        "a=setup:actpass\r\n"
        "m=audio 9 UDP/TLS/RTP/SAVPF 111 63 103 104 9 0 8 106 105 13 110 112 113 126\r\n"
        "c=IN IP4 0.0.0.0\r\n"
        "a=mid:0\r\n"
        "a=rtcp-mux\r\n"
        "a=rtpmap:111 opus/48000/2\r\n"
        "a=fmtp:111 minptime=10;useinbandfec=1\r\n"
        "a=rtpmap:63 red/48000/2\r\n"
        "a=fmtp:63 111/111\r\n"
        "a=rtpmap:103 ISAC/16000\r\n"
        "a=rtpmap:104 ISAC/32000\r\n"
        "a=rtpmap:9 G722/8000\r\n"
        "a=rtpmap:0 PCMU/8000\r\n"
        "a=rtpmap:8 PCMA/8000\r\n"
        "a=rtpmap:106 CN/32000\r\n"
        "a=rtpmap:105 CN/16000\r\n"
        "a=rtpmap:13 CN/8000\r\n"
        "a=rtpmap:110 telephone-event/48000\r\n"
        "a=rtpmap:112 telephone-event/32000\r\n"
        "a=rtpmap:113 telephone-event/16000\r\n"
        "a=rtpmap:126 telephone-event/8000\r\n"
        "a=ssrc:12345678 cname:audioCname\r\n"
        "m=video 9 UDP/TLS/RTP/SAVPF 96 97 98 99 100 101 102 122 127 121 125 107 108 109 124 120 123 119 114 115 116\r\n"
        "c=IN IP4 0.0.0.0\r\n"
        "a=mid:1\r\n"
        "a=rtcp-mux\r\n"
        "a=rtcp-rsize\r\n"
        "a=rtpmap:96 VP8/90000\r\n"
        "a=rtpmap:97 rtx/90000\r\n"
        "a=fmtp:97 apt=96\r\n"
        "a=rtpmap:98 VP9/90000\r\n"
        "a=fmtp:98 profile-id=0\r\n"
        "a=rtpmap:99 rtx/90000\r\n"
        "a=fmtp:99 apt=98\r\n"
        "a=rtpmap:100 H264/90000\r\n"
        "a=fmtp:100 level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42e01f\r\n"
        "a=rtpmap:101 rtx/90000\r\n"
        "a=fmtp:101 apt=100\r\n"
        "a=rtpmap:102 red/90000\r\n"
        "a=rtpmap:122 rtx/90000\r\n"
        "a=fmtp:122 apt=102\r\n"
        "a=ssrc:22345678 cname:videoCname\r\n";
    ngx_rtc_sdp_offer_t offer;

    NGX_RTC_TEST_ASSERT_I64_EQ(
        ngx_rtc_sdp_parse_offer(chrome_offer, (uint32_t)strlen(chrome_offer), &offer),
        NGX_RTC_OK);

    NGX_RTC_TEST_ASSERT_I64_EQ(offer.video_pt, 100);
    NGX_RTC_TEST_ASSERT_U64_EQ(offer.video_ssrc, 22345678u);
    NGX_RTC_TEST_ASSERT_U64_EQ(offer.video_clock_rate, 90000u);
    NGX_RTC_TEST_ASSERT_STR_EQ(offer.video_encoding, "H264");
    NGX_RTC_TEST_ASSERT_I64_EQ(offer.audio_pt, 111);
    NGX_RTC_TEST_ASSERT_U64_EQ(offer.audio_ssrc, 12345678u);
    NGX_RTC_TEST_ASSERT_I64_EQ(offer.audio_channels, 2);
    NGX_RTC_TEST_ASSERT_STR_EQ(offer.audio_encoding, "opus");
}

NGX_RTC_TEST(sdp_answer_init_defaults)
{
    ngx_rtc_sdp_answer_t cfg;

    ngx_rtc_sdp_answer_init(&cfg);

    NGX_RTC_TEST_ASSERT_STR_EQ(cfg.proto, "UDP/TLS/RTP/SAVPF");
    NGX_RTC_TEST_ASSERT_I64_EQ(cfg.video_pt, 102);
    NGX_RTC_TEST_ASSERT_I64_EQ(cfg.audio_pt, 111);
    NGX_RTC_TEST_ASSERT_I64_EQ(cfg.video_clock_rate, 90000);
    NGX_RTC_TEST_ASSERT_I64_EQ(cfg.audio_clock_rate, 48000);
    NGX_RTC_TEST_ASSERT_I64_EQ(cfg.audio_channels, 2);
    NGX_RTC_TEST_ASSERT_I64_EQ(cfg.sendonly, 1);
    NGX_RTC_TEST_ASSERT_I64_EQ(cfg.rtcp_mux, 1);
    NGX_RTC_TEST_ASSERT_I64_EQ(cfg.rtcp_rsize, 1);
}

NGX_RTC_TEST(sdp_generate_answer)
{
    ngx_rtc_sdp_answer_t cfg;
    char buf[2048];
    uint32_t out_len = 0;

    ngx_rtc_sdp_answer_init(&cfg);
    cfg.ice_ufrag = "srvUfrag";
    cfg.ice_pwd = "srvPwd";
    cfg.fingerprint_algo = "sha-256";
    cfg.fingerprint = "AA:BB:CC:DD";
    cfg.setup = "passive";
    cfg.candidate_ip = "192.168.1.10";
    cfg.candidate_port = 8000u;
    cfg.video_ssrc = 111u;
    cfg.audio_ssrc = 222u;

    NGX_RTC_TEST_ASSERT_I64_EQ(
        ngx_rtc_sdp_generate_answer(&cfg, buf, sizeof(buf), &out_len),
        NGX_RTC_OK);
    NGX_RTC_TEST_ASSERT(out_len > 0);
    NGX_RTC_TEST_ASSERT((uint32_t)out_len < sizeof(buf));

    NGX_RTC_TEST_ASSERT(str_contains(buf, "v=0\r\n"));
    NGX_RTC_TEST_ASSERT(str_contains(buf, "a=ice-lite\r\n"));
    NGX_RTC_TEST_ASSERT(str_contains(buf, "a=ice-ufrag:srvUfrag\r\n"));
    NGX_RTC_TEST_ASSERT(str_contains(buf, "a=ice-pwd:srvPwd\r\n"));
    NGX_RTC_TEST_ASSERT(str_contains(buf, "a=fingerprint:sha-256 AA:BB:CC:DD\r\n"));
    NGX_RTC_TEST_ASSERT(str_contains(buf, "a=setup:passive\r\n"));
    NGX_RTC_TEST_ASSERT(str_contains(buf,
        "a=candidate:1 1 udp 2130706431 192.168.1.10 8000 typ host generation 0\r\n"));
    NGX_RTC_TEST_ASSERT(str_contains(buf, "m=video 9 UDP/TLS/RTP/SAVPF 102\r\n"));
    NGX_RTC_TEST_ASSERT(str_contains(buf, "a=rtpmap:102 H264/90000\r\n"));
    NGX_RTC_TEST_ASSERT(str_contains(buf, "a=rtcp-fb:102 nack pli\r\n"));
    NGX_RTC_TEST_ASSERT(str_contains(buf, "a=rtcp-fb:102 nack\r\n"));
    NGX_RTC_TEST_ASSERT(str_contains(buf, "m=audio 9 UDP/TLS/RTP/SAVPF 111\r\n"));
    NGX_RTC_TEST_ASSERT(str_contains(buf, "a=rtpmap:111 opus/48000/2\r\n"));
    NGX_RTC_TEST_ASSERT(str_contains(buf, "a=ssrc:111 cname:111\r\n"));
    NGX_RTC_TEST_ASSERT(str_contains(buf, "a=ssrc:222 cname:222\r\n"));
}

/*
 * The WHIP answer. a=rtcp-fb declares what the answer's author can RECEIVE, and
 * the recvonly direction produces only one of the two: the server sends PLI
 * upstream when a viewer cannot be served from the cache, but it has no
 * receive-side NACK generator at all. Declaring nack here makes the publisher
 * enable RTX and wait for retransmission requests that never arrive -- feedback
 * advertised and never sent, which is what the comment above the emit site
 * used to talk itself into.
 */
NGX_RTC_TEST(sdp_generate_answer_recvonly_omits_nack)
{
    ngx_rtc_sdp_answer_t cfg;
    char buf[2048];
    uint32_t out_len = 0;

    ngx_rtc_sdp_answer_init(&cfg);
    cfg.ice_ufrag = "srvUfrag";
    cfg.ice_pwd = "srvPwd";
    cfg.fingerprint_algo = "sha-256";
    cfg.fingerprint = "AA:BB:CC:DD";
    cfg.setup = "passive";
    cfg.candidate_ip = "192.168.1.10";
    cfg.candidate_port = 8000u;
    cfg.video_ssrc = 111u;
    cfg.audio_ssrc = 222u;
    cfg.sendonly = -1; /* WHIP: the server receives the publisher's media */

    NGX_RTC_TEST_ASSERT_I64_EQ(
        ngx_rtc_sdp_generate_answer(&cfg, buf, sizeof(buf), &out_len),
        NGX_RTC_OK);

    NGX_RTC_TEST_ASSERT(str_contains(buf, "a=recvonly\r\n"));
    NGX_RTC_TEST_ASSERT(str_contains(buf, "a=rtcp-fb:102 nack pli\r\n"));
    NGX_RTC_TEST_ASSERT(!str_contains(buf, "a=rtcp-fb:102 nack\r\n"));
}

NGX_RTC_TEST(sdp_generate_answer_too_small)
{
    ngx_rtc_sdp_answer_t cfg;
    char buf[64];
    uint32_t out_len = 0;

    ngx_rtc_sdp_answer_init(&cfg);
    cfg.ice_ufrag = "srvUfrag";
    cfg.ice_pwd = "srvPwd";
    cfg.video_ssrc = 111u;
    cfg.audio_ssrc = 222u;

    NGX_RTC_TEST_ASSERT_I64_EQ(
        ngx_rtc_sdp_generate_answer(&cfg, buf, sizeof(buf), &out_len),
        NGX_RTC_ERR_TOO_SMALL);
}

/* RFC 3264: the answer m-line order must match the offer. Chrome offers audio
 * (mid 0) before video (mid 1); the answer must mirror that, not hardcode
 * video-first. */
NGX_RTC_TEST(sdp_answer_mirrors_offer_media_order)
{
    ngx_rtc_sdp_answer_t cfg;
    char buf[2048];
    uint32_t out_len = 0;
    char *audio_pos;
    char *video_pos;

    ngx_rtc_sdp_answer_init(&cfg);
    cfg.ice_ufrag = "srvUfrag";
    cfg.ice_pwd = "srvPwd";
    cfg.video_ssrc = 111u;
    cfg.audio_ssrc = 222u;

    strcpy(cfg.media_type[0], "audio");
    strcpy(cfg.media_mid[0], "0");
    strcpy(cfg.media_type[1], "video");
    strcpy(cfg.media_mid[1], "1");

    NGX_RTC_TEST_ASSERT_I64_EQ(
        ngx_rtc_sdp_generate_answer(&cfg, buf, sizeof(buf), &out_len),
        NGX_RTC_OK);

    audio_pos = strstr(buf, "m=audio");
    video_pos = strstr(buf, "m=video");
    NGX_RTC_TEST_ASSERT(NULL != audio_pos);
    NGX_RTC_TEST_ASSERT(NULL != video_pos);
    NGX_RTC_TEST_ASSERT(audio_pos < video_pos);
    NGX_RTC_TEST_ASSERT(str_contains(buf, "a=group:BUNDLE 0 1\r\n"));
    NGX_RTC_TEST_ASSERT(str_contains(buf, "a=mid:0\r\n"));
    NGX_RTC_TEST_ASSERT(str_contains(buf, "a=mid:1\r\n"));
}

NGX_RTC_TEST(sdp_twcc_extmap_parse_and_echo)
{
    static const char offer_twcc[] =
        "v=0\r\n"
        "o=- 0 0 IN IP4 127.0.0.1\r\n"
        "s=-\r\n"
        "t=0 0\r\n"
        "a=ice-ufrag:uf\r\n"
        "a=ice-pwd:pw\r\n"
        "a=fingerprint:sha-256 AB:CD:EF:01:02:03\r\n"
        "a=setup:actpass\r\n"
        "m=video 9 UDP/TLS/RTP/SAVPF 102\r\n"
        "a=rtpmap:102 H264/90000\r\n"
        "a=extmap:3 http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01\r\n"
        "a=ssrc:12345 cname:v\r\n"
        "m=audio 9 UDP/TLS/RTP/SAVPF 111\r\n"
        "a=rtpmap:111 opus/48000/2\r\n"
        "a=extmap:5 http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01\r\n"
        "a=ssrc:67890 cname:a\r\n";
    ngx_rtc_sdp_offer_t offer;
    ngx_rtc_sdp_answer_t cfg;
    char buf[4096];
    uint32_t out_len = 0;

    (void)memset(&offer, 0, sizeof(offer));
    NGX_RTC_TEST_ASSERT_I64_EQ(
        ngx_rtc_sdp_parse_offer(offer_twcc,
                                (uint32_t)strlen(offer_twcc), &offer),
        NGX_RTC_OK);
    NGX_RTC_TEST_ASSERT_I64_EQ(offer.video_twcc_ext, 3);
    NGX_RTC_TEST_ASSERT_I64_EQ(offer.audio_twcc_ext, 5);

    /* An offer without transport-cc leaves the ids at 0. */
    NGX_RTC_TEST_ASSERT_I64_EQ(
        ngx_rtc_sdp_parse_offer(g_offer, (uint32_t)strlen(g_offer), &offer),
        NGX_RTC_OK);
    NGX_RTC_TEST_ASSERT_I64_EQ(offer.video_twcc_ext, 0);
    NGX_RTC_TEST_ASSERT_I64_EQ(offer.audio_twcc_ext, 0);

    /* Echo the ids + exact URIs into the answer: one extmap line per
     * negotiated medium (answer_init already mirrors the offer order). */
    NGX_RTC_TEST_ASSERT_I64_EQ(
        ngx_rtc_sdp_parse_offer(offer_twcc,
                                (uint32_t)strlen(offer_twcc), &offer),
        NGX_RTC_OK);
    ngx_rtc_sdp_answer_init(&cfg);
    cfg.video_twcc_ext = 3;
    cfg.audio_twcc_ext = 5;
    cfg.video_twcc_uri = offer.video_twcc_uri;
    cfg.audio_twcc_uri = offer.audio_twcc_uri;
    NGX_RTC_TEST_ASSERT_I64_EQ(
        ngx_rtc_sdp_generate_answer(&cfg, buf, sizeof(buf), &out_len),
        NGX_RTC_OK);
    NGX_RTC_TEST_ASSERT(str_contains(
        buf, "a=extmap:3 http://www.ietf.org/id/draft-holmer-rmcat-"
             "transport-wide-cc-extensions-01\r\n"));
    NGX_RTC_TEST_ASSERT(str_contains(
        buf, "a=extmap:5 http://www.ietf.org/id/draft-holmer-rmcat-"
             "transport-wide-cc-extensions-01\r\n"));

    /* Not negotiated -> the answer must not advertise transport-cc. */
    ngx_rtc_sdp_answer_init(&cfg);
    cfg.video_twcc_ext = 0;
    cfg.audio_twcc_ext = 0;
    NGX_RTC_TEST_ASSERT_I64_EQ(
        ngx_rtc_sdp_generate_answer(&cfg, buf, sizeof(buf), &out_len),
        NGX_RTC_OK);
    NGX_RTC_TEST_ASSERT(!str_contains(buf, "transport-wide-cc"));
}
