#include <robotweax/srt_telemetry.h>

#include <string.h>

int main(void)
{
    return strcmp(robotweax_srt_telemetry_version(), "0.1.0-alpha.0") == 0 ? 0 : 1;
}
