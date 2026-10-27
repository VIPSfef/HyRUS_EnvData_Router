/* Display aging is deliberately independent of strict measurement alignment. */
#include "monitor.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static tm_snapshot snapshot;
static tb_config config;
static unsigned checks,failures;
#define CHECK(condition) do { ++checks; if(!(condition)) { \
    fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#condition); ++failures; } } while(0)

static void render(char *out,size_t capacity)
{
    FILE *file=tmpfile();
    size_t n;
    if(!file) { fprintf(stderr,"cannot create temporary rendering stream\n"); exit(EXIT_FAILURE); }
    tm_render(file,&snapshot,&config);
    rewind(file); n=fread(out,1,capacity-1,file); out[n]='\0'; fclose(file);
}
int main(void)
{
    char text[32768];
    cfg_defaults(&config);
    (void)snprintf(config.input_mode,sizeof(config.input_mode),"mavlink");
    snapshot.now_ms=1000;
    render(text,sizeof(text));
    CHECK(strstr(text,"position=UNKNOWN state=UNKNOWN rx_age=unknown")!=NULL);
    CHECK(strstr(text,"geo lat=")==NULL);
    snapshot.have_last_geodetic=1; snapshot.have_geodetic=1;
    snapshot.position_kind=TM_POSITION_ESTGPS; snapshot.navigation_received_ms=1000;
    snapshot.latitude_deg=37; snapshot.longitude_deg=127; snapshot.altitude_msl_m=100;
    render(text,sizeof(text));
    CHECK(strstr(text,"position=ESTGPS state=LIVE rx_age=0ms")!=NULL);
    snapshot.have_geodetic=0; snapshot.now_ms=1999;
    render(text,sizeof(text));
    CHECK(strstr(text,"position=ESTGPS state=LIVE rx_age=999ms")!=NULL);
    CHECK(strstr(text,"geo lat=37.0000000 lon=127.0000000 alt_msl=100.00m")!=NULL);
    snapshot.now_ms=1000+config.link_timeout_ms;
    render(text,sizeof(text));
    CHECK(strstr(text,"position=ESTGPS state=STALE")!=NULL);
    CHECK(strstr(text,"geo lat=37.0000000")!=NULL);
    snapshot.navigation_invalid=1;
    render(text,sizeof(text));
    CHECK(strstr(text,"position=ESTGPS state=INVALID")!=NULL);
    snapshot.navigation_invalid=0; snapshot.position_kind=TM_POSITION_GPS;
    snapshot.navigation_received_ms=snapshot.now_ms;
    render(text,sizeof(text));
    CHECK(strstr(text,"position=GPS state=LIVE rx_age=0ms")!=NULL);
    /* Reject corrupt metadata without accidentally displaying a zero fix. */
    snapshot.latitude_deg=NAN;
    render(text,sizeof(text));
    CHECK(strstr(text,"position=UNKNOWN state=UNKNOWN")!=NULL);
    CHECK(strstr(text,"geo lat=")==NULL);
    printf("monitor position: %u checks, %u failures\n",checks,failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
