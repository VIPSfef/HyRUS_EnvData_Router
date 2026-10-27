/* Include the actual production sender to exercise its private session
 * transaction, rather than duplicate its logic. Only its CLI entry is renamed;
 * no receiver, HTTP worker, hardware, or application window is started. This
 * samples exactly between next_session and the next receiver status callback,
 * the interleaving that allowed an old coordinate into a new session report. */
#define main sender_program_main
#include "../sender.c"
#undef main
#include "test_workdir.h"

static unsigned checks,failures;
#define CHECK(condition) do { ++checks; if(!(condition)) { \
    fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#condition); ++failures; } } while(0)

static void session_transition(uint32_t epoch)
{
    mi_event reset;
    tm_snapshot copy;
    ri_report report;
    char json[SR_WIRE_CAP],previous_session[65];
    uint64_t at=now_ms();
    memset(&state,0,sizeof(state));
    cfg_defaults(&state.config); state.session="session-test"; state.run_nonce=17;
    CHECK(mutex_init(&state.mutex));
    state.diag.have_position=state.diag.have_geodetic=state.diag.have_last_geodetic=1;
    state.diag.have_fix_age=state.diag.have_position_error=1;
    state.diag.position_kind=TM_POSITION_GPS;
    state.diag.navigation_received_ms=at;
    state.diag.latitude_deg=37.125; state.diag.longitude_deg=127.25; state.diag.altitude_msl_m=100;
    state.diag.estimated_error_m=2; state.diag.last_fix_age_ms=100;
    state.diag.gps_fix=3;
    state.diag.selected_system=1; state.diag.selected_component=1;
    state.diag.device_n=1; state.diag.devices[0].sysid=1; state.diag.devices[0].compid=1;
    state.diag.have_heartbeat=1; state.diag.heartbeat_received_ms=at;
    format_session(&state);
    (void)snprintf(previous_session,sizeof(previous_session),"%s",state.active_session);

    /* An unselected probe/disconnect must keep the old position available as
     * history; only an actual new session below invalidates that identity. */
    memset(&report,0,sizeof(report));
    (void)snprintf(report.status,sizeof(report.status),"DISCONNECTED");
    input_status(&state,&report);
    snapshot(&state,&copy);
    CHECK(sr_position_known(&copy));
    CHECK(copy.latitude_deg==37.125 && copy.navigation_received_ms==at);
    CHECK(copy.device_n==1 && copy.selected_system==1);

    memset(&reset,0,sizeof(reset)); reset.kind=MI_RESET; reset.epoch=epoch;
    lock(&state.mutex);
    next_session(&state,&reset);
    snapshot_locked(&state,&copy,at);
    CHECK(sr_encode(&copy,&state.config,"test",state.active_session,state.route_generation,
                    1,json,sizeof(json))>0);
    unlock(&state.mutex);
    CHECK(strcmp(previous_session,state.active_session)!=0 && state.route_generation==1);
    CHECK(!copy.have_position && !copy.have_geodetic && !copy.have_last_geodetic);
    CHECK(!copy.have_fix_age && !copy.have_position_error && !copy.navigation_invalid);
    CHECK(copy.position_kind==TM_POSITION_UNKNOWN && !copy.navigation_received_ms);
    CHECK(strstr(json,"\"position\":null")!=NULL);
    CHECK(strstr(json,"37.125")==NULL);
    /* A decoder reboot retains same-source device receipt history. The
     * existing new-transport policy still clears devices only for epoch zero. */
    CHECK(copy.device_n==(epoch ? 1u : 0u));
    CHECK(copy.selected_system==(epoch ? 1u : 0u));

    /* A later accepted point belongs to the new session and is visible again. */
    report.source_selected=1;
    (void)snprintf(report.status,sizeof(report.status),"LOCKED");
    report.mav.have_navigation=report.mav.have_last_geodetic=1;
    report.mav.position_kind=report.mav.last_position_kind=WEATHER_POSITION_ESTGPS;
    report.mav.navigation_received_ms=now_ms();
    report.mav.lat=38.25; report.mav.lon=128.5; report.mav.altitude_msl_m=200;
    input_status(&state,&report);
    snapshot(&state,&copy);
    CHECK(sr_position_known(&copy) && copy.latitude_deg==38.25);
    CHECK(copy.position_kind==TM_POSITION_ESTGPS);
    mutex_destroy(&state.mutex);
}

static void auth_restart_transaction(void)
{
    static const char old_token[]="TEST_ONLY_original_auth";
    static const char new_token[]="TEST_ONLY_replacement_auth";
    tb_config next,original;
    char path[128],absent[160],error[192];
#ifdef _WIN32
    unsigned process=(unsigned)GetCurrentProcessId();
#else
    unsigned process=(unsigned)getpid();
#endif
    (void)snprintf(path,sizeof(path),"sender-auth-test-%u.ini",process);
    (void)snprintf(absent,sizeof(absent),"sender-auth-absent-%u/settings.ini",process);
    memset(&state,0,sizeof(state));
    cfg_defaults(&state.config);
    strcpy(state.config.url,"http://127.0.0.1:21000/ingest");
    strcpy(state.config.stream,"auth-test"); strcpy(state.config.session,"auth-test");
    strcpy(state.config.token,old_token);
    state.config.spatial_enabled=0;
    state.config_path=path; state.live=1;
    state.auth=resolve_http_auth(&state.config);
    state.queue.count=3; state.current.n=2;
    CHECK(mutex_init(&state.mutex));
    original=state.config; next=original; strcpy(next.token,new_token);

    /* A rejected save cannot expose a private draft to in-flight sends. */
    state.config_path=absent;
    CHECK(!request_restart(&state,&next,error,sizeof(error)));
    CHECK(!state.restart_requested && !strcmp(state.auth.token,old_token));
    CHECK(state.queue.count==3 && state.current.n==2);
    state.config_path=path;

    /* Updating the same endpoint repairs authentication before drain, while
     * queued/partial measurements and all active receiver settings stay put. */
    CHECK(request_restart(&state,&next,error,sizeof(error)));
    CHECK(state.restart_requested && state.restart_attempt==1);
    CHECK(state.auth.valid && !strcmp(state.auth.token,new_token));
    CHECK(!memcmp(&state.config,&original,sizeof(original)));
    CHECK(!memcmp(&state.restart_config,&next,sizeof(next)));
    CHECK(state.queue.count==3 && state.current.n==2);
    CHECK(!request_restart(&state,&original,error,sizeof(error)));
    CHECK(!strcmp(state.auth.token,new_token));

    /* Exercise the production failure-resume branch. The partials have sealed
     * already, but the unsent FIFO remains intact when old settings resume. */
    state.current.n=0; state.restart_blocked=1;
    CHECK(!input_stop(&state));
    CHECK(!state.restart_requested && !state.restart_blocked && !state.diag.restarting);
    CHECK(state.auth.valid && !strcmp(state.auth.token,old_token));
    CHECK(state.queue.count==3 && !memcmp(&state.config,&original,sizeof(original)));

    /* New-destination credentials must never be used to drain the old URL,
     * including same-host changes to path or scheme. Exact URL is the guard. */
    strcpy(next.url,"http://127.0.0.1:21000/different-ingest");
    CHECK(request_restart(&state,&next,error,sizeof(error)));
    CHECK(!strcmp(state.auth.token,old_token));
    CHECK(!strcmp(state.restart_config.token,new_token));
    CHECK(!strcmp(state.restart_config.url,next.url));
    CHECK(state.queue.count==3 && !memcmp(&state.config,&original,sizeof(original)));
    state.restart_blocked=1;
    CHECK(!input_stop(&state));
    CHECK(!strcmp(state.auth.token,old_token) && state.queue.count==3);
    CHECK(remove(path)==0);
    mutex_destroy(&state.mutex);
}

static int set_auth_test_environment(const char *name,const char *value)
{
#ifdef _WIN32
    return _putenv_s(name,value ? value : "")==0;
#else
    return (value ? setenv(name,value,1) : unsetenv(name))==0;
#endif
}
static void invalid_environment_auth(void)
{
    static const char name[]="MTR_TEST_SENDER_AUTH";
    static const char injected[]="TEST_ONLY_invalid\r\nInjected:1";
    char overlong[CFG_AUTH_TOKEN_BYTES+1];
    const char *invalid[]={injected,overlong};
    size_t i;
    memset(overlong,'x',sizeof(overlong)-1); overlong[sizeof(overlong)-1]='\0';
    memset(&state,0,sizeof(state)); cfg_defaults(&state.config);
    strcpy(state.config.token_env,name);
    CHECK(mutex_init(&state.mutex));
    for(i=0;i<sizeof(invalid)/sizeof(invalid[0]);++i) {
        long status=204;
        unsigned error=0;
        CHECK(set_auth_test_environment(name,invalid[i]));
        state.auth=resolve_http_auth(&state.config);
        CHECK(!state.auth.valid && !state.auth.token[0]);
        /* NULL HTTP handle proves invalid credentials cannot reach a request. */
        CHECK(!http_post_snapshot(&state,NULL,"{}",2,&status,&error));
        CHECK(status==0 && error==EINVAL);
    }
    CHECK(set_auth_test_environment(name,NULL));
    mutex_destroy(&state.mutex);
}

static void http_pacing_contract(void)
{
    static const unsigned delays[]={1000,2000,4000,8000,16000,30000,30000};
    static const long denied[]={401,403,429};
    unsigned i;
    http_pacing pacing={0};
    uint64_t completed=100;
    for(i=0;i<sizeof(delays)/sizeof(delays[0]);++i) {
        http_pacing_complete(&pacing,500,denied[i%(sizeof(denied)/sizeof(denied[0]))],completed);
        CHECK(pacing.completed_ms==completed && pacing.next_ms==completed+delays[i]);
        CHECK(pacing.backoff_ms==delays[i]);
        completed=pacing.next_ms+100;
    }
    /* A generic failure cannot bypass an existing auth/policy backoff. */
    http_pacing_complete(&pacing,500,0,completed);
    CHECK(pacing.next_ms==completed+30000);
    http_pacing_complete(&pacing,500,204,completed);
    CHECK(!pacing.backoff_ms && pacing.next_ms==completed+500);
    http_pacing_complete(&pacing,60000,403,completed);
    CHECK(pacing.backoff_ms==1000 && pacing.next_ms==completed+60000);

    memset(&state,0,sizeof(state)); cfg_defaults(&state.restart_config);
    state.pacing=pacing;
    apply_restart(&state);
    CHECK(state.pacing.completed_ms==completed && state.pacing.next_ms==completed+60000);
    CHECK(state.pacing.backoff_ms==1000);
    http_pacing_complete(&state.pacing,500,204,completed);
    state.restart_config=state.config; state.restart_config.http_min_interval_ms=2000;
    apply_restart(&state);
    CHECK(state.pacing.next_ms==completed+2000);
}

int main(void)
{
    test_workdir directory;
    if(!test_workdir_enter(&directory)) return EXIT_FAILURE;
    session_transition(0); /* a newly selected transport */
    session_transition(2); /* selected FC boot/session reset */
    auth_restart_transaction();
    invalid_environment_auth();
    http_pacing_contract();
    CHECK(test_workdir_leave(&directory));
    printf("sender session: %u checks, %u failures\n",checks,failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
