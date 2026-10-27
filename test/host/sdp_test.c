/*
 * SPDX-FileCopyrightText: 2026 Leon de Rooij (OSHLabs)
 *
 * SPDX-License-Identifier: Apache-2.0
 */
/*
 * SDP parser and generator tests, on the host. Sample descriptions are
 * modelled on what this node, the aes67-linux-daemon, a Dante device in
 * AES67 mode and Merging SIENNA put on the wire.
 */

#include "aes67_sdp.h"
#include "aes67_config.h"
#include "aes67_net.h"
#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond) do { if (!(cond)) { failures++; \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static const char sdp_own[] =
    "v=0\r\no=- 1 2 IN IP4 172.31.1.36\r\ns=ESP32-P4 Mic\r\nc=IN IP4 239.69.0.1/64\r\nt=0 0\r\n"
    "m=audio 5004 RTP/AVP 98\r\ni=Channels 1-2\r\na=sync-time:0\r\na=framecount:48\r\n"
    "a=rtpmap:98 L24/48000/2\r\na=ptime:1\r\na=sendonly\r\n"
    "a=ts-refclk:ptp=IEEE1588-2008:D8-3A-DD-FF-FE-88-6F-1F:0\r\n"
    "a=ts-refclk:ptp=IEEE1588-2008:traceable\r\na=mediaclk:direct=0\r\na=clock-domain:PTPv2 0\r\n";

static const char sdp_linux_daemon[] =
    "v=0\no=- 1 0 IN IP4 172.31.0.35\ns=rtpd listener\nc=IN IP4 239.69.1.10/15\nt=0 0\n"
    "a=clock-domain:PTPv2 0\nm=audio 5004 RTP/AVP 98\nc=IN IP4 239.69.1.10/15\n"
    "a=rtpmap:98 L24/48000/2\na=sync-time:0\na=framecount:48\na=ptime:1\na=mediaclk:direct=0\n"
    "a=ts-refclk:ptp=IEEE1588-2008:00-1D-C1-FF-FE-0A-0B-0C:0\na=recvonly\n";

static const char sdp_dante[] =
    "v=0\r\no=- 1311738121 1311738121 IN IP4 169.254.98.63\r\ns=AVIO-USB : 2\r\n"
    "c=IN IP4 239.69.83.133/32\r\nt=0 0\r\na=keywds:Dante\r\na=recvonly\r\n"
    "m=audio 5004 RTP/AVP 97\r\ni=2 channels: Left, Right\r\na=rtpmap:97 L24/48000/2\r\n"
    "a=ptime:1\r\na=ts-refclk:ptp=IEEE1588-2008:00-1D-C1-FF-FE-C4-37-A2:0\r\na=mediaclk:direct=0\r\n";

static const char sdp_sienna_333[] =
    "v=0\r\no=- 5 1 IN IP4 10.0.0.9\r\ns=Sienna Out 1-8\r\nc=IN IP4 239.1.2.3/15\r\nt=0 0\r\n"
    "m=audio 5004 RTP/AVP 98\r\na=rtpmap:98 l24/48000/8\r\na=ptime:0.333\r\na=framecount:16\r\n"
    "a=ts-refclk:ptp=IEEE1588-2019:traceable\r\na=mediaclk:direct=0\r\n";

static void test_parse_samples(void)
{
    aes67_sdp_t s;

    CHECK(aes67_sdp_parse(sdp_own, &s) == ESP_OK);
    CHECK(s.port == 5004 && s.payload_type == 98 && s.channels == 2);
    CHECK(s.codec == AES67_CODEC_L24 && s.word_length == 3 && s.sample_rate == 48000);
    CHECK(s.ptime_us == 1000 && s.framecount == 48);
    CHECK(s.has_ptp_refclk && s.ptp_traceable && s.ptp_domain == 0);
    CHECK(s.ptp_grandmaster_id[0] == 0xD8 && s.ptp_grandmaster_id[7] == 0x1F);
    CHECK(s.session_version == 2 && strcmp(s.session_name, "ESP32-P4 Mic") == 0);
    CHECK(s.connection_ip == aes67_net_ip_to_u32("239.69.0.1") && s.ttl == 64);

    CHECK(aes67_sdp_parse(sdp_linux_daemon, &s) == ESP_OK);      /* LF only, c= twice */
    CHECK(s.connection_ip == aes67_net_ip_to_u32("239.69.1.10") && s.ttl == 15);
    CHECK(s.has_ptp_refclk && !s.ptp_traceable && s.ptp_grandmaster_id[2] == 0xC1);

    CHECK(aes67_sdp_parse(sdp_dante, &s) == ESP_OK);
    CHECK(s.payload_type == 97 && s.ptime_us == 1000 && s.framecount == 0);
    CHECK(s.has_ptp_refclk && s.ptp_grandmaster_id[5] == 0xC4);

    CHECK(aes67_sdp_parse(sdp_sienna_333, &s) == ESP_OK);        /* lower-case codec, 2019 */
    CHECK(s.codec == AES67_CODEC_L24 && s.channels == 8);
    CHECK(s.ptime_us == 333 && s.framecount == 16);
    CHECK(!s.has_ptp_refclk && s.ptp_traceable);
    CHECK(aes67_frames_per_packet(s.sample_rate, s.ptime_us) == 16);
}

static void test_reject(void)
{
    aes67_sdp_t s;
    const char *bad[] = {
        /* no media line */
        "v=0\r\no=- 1 1 IN IP4 1.2.3.4\r\ns=x\r\nc=IN IP4 239.1.1.1/15\r\nt=0 0\r\n",
        /* unknown encoding */
        "v=0\r\no=- 1 1 IN IP4 1.2.3.4\r\ns=x\r\nc=IN IP4 239.1.1.1/15\r\nt=0 0\r\n"
        "m=audio 5004 RTP/AVP 96\r\na=rtpmap:96 opus/48000/2\r\na=ptime:1\r\n",
        /* too many channels */
        "v=0\r\no=- 1 1 IN IP4 1.2.3.4\r\ns=x\r\nc=IN IP4 239.1.1.1/15\r\nt=0 0\r\n"
        "m=audio 5004 RTP/AVP 98\r\na=rtpmap:98 L24/48000/64\r\na=ptime:1\r\n",
        /* packet time out of range (would wrap a 16-bit parser) */
        "v=0\r\no=- 1 1 IN IP4 1.2.3.4\r\ns=x\r\nc=IN IP4 239.1.1.1/15\r\nt=0 0\r\n"
        "m=audio 5004 RTP/AVP 98\r\na=rtpmap:98 L24/48000/2\r\na=ptime:70\r\n",
        /* unsupported sample rate */
        "v=0\r\no=- 1 1 IN IP4 1.2.3.4\r\ns=x\r\nc=IN IP4 239.1.1.1/15\r\nt=0 0\r\n"
        "m=audio 5004 RTP/AVP 98\r\na=rtpmap:98 L24/32000/2\r\na=ptime:1\r\n",
        /* no connection address */
        "v=0\r\no=- 1 1 IN IP4 1.2.3.4\r\ns=x\r\nt=0 0\r\n"
        "m=audio 5004 RTP/AVP 98\r\na=rtpmap:98 L24/48000/2\r\na=ptime:1\r\n",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        CHECK(aes67_sdp_parse(bad[i], &s) != ESP_OK);
    }
    CHECK(aes67_sdp_parse("", &s) != ESP_OK);
}

static void test_refclk_forms(void)
{
    aes67_sdp_t s;
    const char head[] = "v=0\r\no=- 1 1 IN IP4 1.2.3.4\r\ns=x\r\nc=IN IP4 239.1.1.1/15\r\nt=0 0\r\n"
                        "m=audio 5004 RTP/AVP 98\r\na=rtpmap:98 L24/48000/2\r\na=ptime:1\r\n";
    char buf[1024];

    snprintf(buf, sizeof(buf), "%sa=ts-refclk:ptp=IEEE1588-2008:traceable\r\n", head);
    CHECK(aes67_sdp_parse(buf, &s) == ESP_OK && !s.has_ptp_refclk && s.ptp_traceable);

    snprintf(buf, sizeof(buf), "%sa=ts-refclk:ptp=traceable\r\n", head);
    CHECK(aes67_sdp_parse(buf, &s) == ESP_OK && s.ptp_traceable);

    snprintf(buf, sizeof(buf), "%sa=ts-refclk:ptp=IEEE1588-2002:00-11-22-33-44-55-66-77:5\r\n", head);
    CHECK(aes67_sdp_parse(buf, &s) == ESP_OK && s.has_ptp_refclk && s.ptp_domain == 5);

    snprintf(buf, sizeof(buf), "%sa=ts-refclk:ptp=IEEE1588-2008:00-11-22\r\n", head);   /* short */
    CHECK(aes67_sdp_parse(buf, &s) == ESP_OK && !s.has_ptp_refclk);

    snprintf(buf, sizeof(buf), "%sa=ts-refclk:ntp=/traceable/\r\n", head);                /* other */
    CHECK(aes67_sdp_parse(buf, &s) == ESP_OK && !s.has_ptp_refclk && !s.ptp_traceable);

    /* no ptime at all: the AES67 default */
    const char nop[] = "v=0\r\no=- 1 1 IN IP4 1.2.3.4\r\ns=x\r\nc=IN IP4 239.1.1.1/15\r\nt=0 0\r\n"
                       "m=audio 5004 RTP/AVP 98\r\na=rtpmap:98 L24/48000/2\r\n";
    CHECK(aes67_sdp_parse(nop, &s) == ESP_OK && s.ptime_us == 1000);
}

static void test_generate_roundtrip(void)
{
    aes67_sdp_t out, in;
    char text[1024];
    memset(&out, 0, sizeof(out));
    strcpy(out.origin_username, "-");
    out.session_id = 7; out.session_version = 3;
    out.origin_ip = aes67_net_ip_to_u32("172.31.1.36");
    strcpy(out.session_name, "Round \"trip\" source");
    out.connection_ip = aes67_net_ip_to_u32("239.69.0.3"); out.ttl = 64;
    out.port = 5004; out.payload_type = 98;
    out.codec = AES67_CODEC_L24; out.sample_rate = 48000; out.channels = 2; out.word_length = 3;
    out.ptime_us = 333;
    out.is_source = true;
    out.has_ptp_refclk = true; out.ptp_traceable = true; out.ptp_domain = 3;
    memcpy(out.ptp_grandmaster_id, "\x00\x11\x22\xff\xfe\x33\x44\x55", 8);
    out.has_mediaclk = true;

    int n = aes67_sdp_generate(&out, text, sizeof(text));
    CHECK(n > 0);
    CHECK(strstr(text, "a=framecount:16\r\n") != NULL);          /* 0.333 ms is 16 frames */
    CHECK(strstr(text, "a=ptime:0.333\r\n") != NULL);
    CHECK(strstr(text, "a=ts-refclk:ptp=IEEE1588-2008:00-11-22-FF-FE-33-44-55:3\r\n") != NULL);
    CHECK(strstr(text, "a=ts-refclk:ptp=IEEE1588-2008:traceable\r\n") != NULL);
    CHECK(strstr(text, "a=sendonly\r\n") != NULL);

    CHECK(aes67_sdp_parse(text, &in) == ESP_OK);
    CHECK(in.port == out.port && in.payload_type == out.payload_type);
    CHECK(in.codec == out.codec && in.channels == out.channels && in.sample_rate == out.sample_rate);
    CHECK(in.ptime_us == out.ptime_us && in.framecount == 16);
    CHECK(in.has_ptp_refclk && in.ptp_traceable && in.ptp_domain == 3);
    CHECK(memcmp(in.ptp_grandmaster_id, out.ptp_grandmaster_id, 8) == 0);
    CHECK(in.connection_ip == out.connection_ip && in.ttl == out.ttl);
    CHECK(in.session_version == 3 && strcmp(in.session_name, out.session_name) == 0);

    /* not locked: no clock claim at all */
    out.has_ptp_refclk = false; out.ptp_traceable = false;
    CHECK(aes67_sdp_generate(&out, text, sizeof(text)) > 0);
    CHECK(strstr(text, "ts-refclk") == NULL);

    /* a buffer too small is an error, not an overflow */
    CHECK(aes67_sdp_generate(&out, text, 64) < 0);
}

static void test_names(void)
{
    CHECK(aes67_name_is_clean("ESP32-P4 Mic"));
    CHECK(!aes67_name_is_clean(""));
    CHECK(!aes67_name_is_clean("line\nbreak"));
    CHECK(!aes67_name_is_clean("del\x7f"));
    char longname[80]; memset(longname, 'a', 79); longname[79] = 0;
    CHECK(!aes67_name_is_clean(longname));
}

int main(void)
{
    test_parse_samples();
    test_reject();
    test_refclk_forms();
    test_generate_roundtrip();
    test_names();
    if (failures) {
        printf("sdp_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("sdp_test: all checks passed\n");
    return 0;
}
