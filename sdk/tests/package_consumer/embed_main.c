/* C consumer of the embed library: spec parsing, start on a free port,
 * NULL-safe calls, stop. Fake SRT functions: no SRT library is linked. */
#include <robotweax/srt_telemetry_embed.h>

#include <stdint.h>
#include <stdio.h>

static int no_stats(int32_t socket, struct CBytePerfMon *statistics, int clear, int instantaneous)
{
    (void)socket;
    (void)statistics;
    (void)clear;
    (void)instantaneous;
    return -1;
}

static int no_state(int32_t socket)
{
    (void)socket;
    return -1;
}

static int no_flag(int32_t socket, int option, void *value, int *length)
{
    (void)socket;
    (void)option;
    (void)value;
    (void)length;
    return -1;
}

int main(void)
{
    robotweax_srt_telemetry_embed_config_v1 config;
    robotweax_srt_telemetry_embed *embed = NULL;
    robotweax_srt_telemetry_embed_config_default(&config);
    config.bistats = no_stats;
    config.getsockstate = no_state;
    config.getrejectreason = no_state;
    config.getsockflag = no_flag;
    if (robotweax_srt_telemetry_embed_config_parse(&config, "127.0.0.1:0,name=cam-1,no_ui") !=
            ROBOTWEAX_SRT_TELEMETRY_OK ||
        robotweax_srt_telemetry_embed_start(&config, &embed) != ROBOTWEAX_SRT_TELEMETRY_OK)
    {
        return 1;
    }
    if (robotweax_srt_telemetry_embed_http_port(embed) == 0 ||
        robotweax_srt_telemetry_embed_detach(embed, 7) != ROBOTWEAX_SRT_TELEMETRY_NOT_FOUND ||
        robotweax_srt_telemetry_embed_attach(NULL, 7, ROBOTWEAX_SRT_TELEMETRY_ENDPOINT_ROLE_SENDER, NULL, 0) !=
            ROBOTWEAX_SRT_TELEMETRY_OK)
    {
        robotweax_srt_telemetry_embed_stop(embed);
        return 2;
    }
    robotweax_srt_telemetry_embed_stop(embed);
    puts("embed package consumer: ok");
    return 0;
}
