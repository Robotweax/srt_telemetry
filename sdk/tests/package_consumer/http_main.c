#include <robotweax/srt_telemetry_http.h>

#include <stdio.h>

/* Installed-package check for the HTTP server from a C-only project. */
int main(void)
{
    robotweax_srt_telemetry_sampler_config_v1 sampler_config;
    robotweax_srt_telemetry_sampler_config_default(&sampler_config);
    robotweax_srt_telemetry_sampler *sampler = NULL;
    if (robotweax_srt_telemetry_sampler_create(&sampler_config, &sampler) != ROBOTWEAX_SRT_TELEMETRY_OK)
    {
        return 1;
    }
    robotweax_srt_telemetry_http_server_config_v1 config;
    robotweax_srt_telemetry_http_server_config_default(sampler, &config);
    robotweax_srt_telemetry_http_server *server = NULL;
    if (robotweax_srt_telemetry_http_server_start(&config, &server) != ROBOTWEAX_SRT_TELEMETRY_OK)
    {
        robotweax_srt_telemetry_sampler_destroy(sampler);
        return 1;
    }
    printf("http server on port %u\n", (unsigned)robotweax_srt_telemetry_http_server_port(server));
    robotweax_srt_telemetry_http_server_stop(server);
    robotweax_srt_telemetry_sampler_destroy(sampler);
    return 0;
}
