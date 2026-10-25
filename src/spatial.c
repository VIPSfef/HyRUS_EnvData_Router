#include "spatial.h"

#include <float.h>
#include <inttypes.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static void count_up(uint64_t *v) { if (*v != UINT64_MAX) ++*v; }
void sp_options_default(sp_options *o)
{
    if (!o) return;
    memset(o,0,sizeof(*o)); o->step_m=10.0; o->gap_us=UINT64_C(2000000);
    o->chord_tolerance_m=.5; o->chord_relative_tolerance=.25; o->adaptive=1;
}
int sp_options_valid(const sp_options *o)
{
    return o && isfinite(o->step_m) && o->step_m>=1e-6 && o->step_m<=SP_MAX_WIDTH_M &&
        o->gap_us>0 && o->gap_us<=UINT64_C(60000000) &&
        isfinite(o->chord_tolerance_m) && o->chord_tolerance_m>=0 && o->chord_tolerance_m<=1e6 &&
        isfinite(o->chord_relative_tolerance) && o->chord_relative_tolerance>=0 && o->chord_relative_tolerance<=10 &&
        (o->adaptive==0 || o->adaptive==1);
}
void sp_init(sp_state *s,const sp_options *o)
{
    if (!s) return;
    memset(s,0,sizeof(*s)); sp_options_default(&s->options);
    if (sp_options_valid(o)) s->options=*o;
}
static void report_path(sp_state *s)
{
    s->report.path_id=s->path_id; s->report.bin_index=s->bin_index;
    s->report.s_m=s->s_m; s->report.effective_width_m=s->width_m;
    s->report.open_n=s->open_n; s->report.completed_n=s->completed_n;
}
static void enqueue(sp_state *s,const sp_record *r)
{
    count_up(&s->report.completed_records);
    if (s->completed_n==SP_COMPLETED_CAP) {
        count_up(&s->report.dropped_records); s->capacity_pressure=1;
        s->report.flags|=SP_FLAG_RATE_LIMITED; return;
    }
    s->completed[(s->completed_head+s->completed_n)%SP_COMPLETED_CAP]=*r;
    ++s->completed_n;
}
static void close_open(sp_state *s,uint8_t status,uint32_t reason)
{
    unsigned i;
    for(i=0;i<s->open_n;++i) {
        sp_record r=s->open[i].record;
        r.status=status; r.flags|=reason;
        if(r.revision!=UINT64_MAX) ++r.revision;
        enqueue(s,&r);
    }
    s->open_n=0;
}
void sp_break(sp_state *s,uint32_t reason)
{
    if(!s) return;
    close_open(s,SP_INTERRUPTED,reason);
    s->report.flags|=reason; s->have_step=s->current_step_valid=0;
    s->report.path_known=0; report_path(s);
}
int sp_set_options(sp_state *s,const sp_options *o)
{
    if(!s || !sp_options_valid(o)) return 0;
    if(s->options.step_m!=o->step_m || s->options.gap_us!=o->gap_us || s->options.adaptive!=o->adaptive ||
       s->options.chord_tolerance_m!=o->chord_tolerance_m ||
       s->options.chord_relative_tolerance!=o->chord_relative_tolerance) {
        sp_break(s,SP_FLAG_CONFIG_CHANGE); s->options=*o; s->shrink_credit=0;
    }
    return 1;
}
static double select_width(sp_state *s,double observed_step)
{
    double width=s->options.step_m;
    if(s->options.adaptive) {
        if(observed_step>width) width=observed_step;
        if(!isfinite(width) || width>SP_MAX_WIDTH_M) width=SP_MAX_WIDTH_M;
        if(s->capacity_pressure) {
            double pressure_width=s->width_m>0 ? s->width_m*2 : width*2;
            if(pressure_width>width) width=pressure_width;
            if(width>SP_MAX_WIDTH_M) width=SP_MAX_WIDTH_M;
        }
        if(s->have_step && width<s->width_m) {
            /* Two consecutive bin entries must support a finer resolution. */
            if(++s->shrink_credit<2) width=s->width_m;
            else s->shrink_credit=0;
        } else s->shrink_credit=0;
    }
    return width;
}
static int start_path(sp_state *s,const weather_sample *w)
{
    if(s->path_id==UINT64_MAX) {
        s->report.flags|=SP_FLAG_COUNT_OVERFLOW; s->report.path_known=0; return 0;
    }
    ++s->path_id; count_up(&s->report.paths_started); s->bin_index=0; s->s_m=s->s_compensation=0;
    s->interval_start_m=0; s->entry_step_m=0;
    s->width_m=select_width(s,0);
    s->anchor_p[0]=s->last_p[0]=w->p[0]; s->anchor_p[1]=s->last_p[1]=w->p[1];
    s->predicted_displacement[0]=s->predicted_displacement[1]=0;
    s->last_v[0]=w->v[0]; s->last_v[1]=w->v[1]; s->last_t_us=w->t_us;
    s->source_kind=w->position_kind; s->have_step=s->current_step_valid=1;
    s->path_flags=SP_FLAG_PATH_ESTIMATED;
    s->bin_flags=s->path_flags|(s->capacity_pressure ? SP_FLAG_RATE_LIMITED : 0);
    s->report.path_known=1; report_path(s); return 1;
}
static int representative_better(const weather_sample *a,const weather_sample *b)
{
    int ak=isfinite(a->pos_error_m),bk=isfinite(b->pos_error_m);
    if(ak!=bk) return ak>bk;
    if(ak && a->pos_error_m!=b->pos_error_m) return a->pos_error_m<b->pos_error_m;
    ak=isfinite(a->measurement_variance); bk=isfinite(b->measurement_variance);
    if(ak!=bk) return ak>bk;
    if(ak && a->measurement_variance!=b->measurement_variance) return a->measurement_variance<b->measurement_variance;
    if(a->time_uncert_us!=b->time_uncert_us) return a->time_uncert_us<b->time_uncert_us;
    if(a->t_us!=b->t_us) return a->t_us<b->t_us;
    return a->seq<b->seq;
}
static int add_group(sp_state *s,const weather_sample *w)
{
    unsigned i; sp_record *r; double delta;
    for(i=0;i<s->open_n;++i) {
        r=&s->open[i].record;
        if(r->metric==w->metric && r->message_id==w->message_id && r->sensor_id==w->sensor_id &&
           r->sensor_instance==w->sensor_instance && r->source_kind==w->position_kind) break;
    }
    if(i==s->open_n) {
        if(s->open_n==SP_GROUP_CAP) { count_up(&s->report.rejected_samples); return 0; }
        memset(&s->open[i],0,sizeof(s->open[i])); ++s->open_n; r=&s->open[i].record;
        r->continuity_key=s->continuity_key; r->path_id=s->path_id; r->bin_index=s->bin_index;
        r->message_id=w->message_id; r->sensor_id=w->sensor_id; r->metric=w->metric;
        r->sensor_instance=w->sensor_instance; r->source_kind=w->position_kind;
        r->t_start_us=r->t_end_us=w->t_us; r->min=r->max=w->value; r->representative=*w;
        r->s_start_m=r->s_end_m=s->s_m; r->width_m=s->width_m;
        r->interval_start_m=s->interval_start_m; r->interval_end_m=s->interval_start_m+s->width_m;
        r->entry_step_m=s->entry_step_m;
    }
    r=&s->open[i].record;
    if(r->n==UINT32_MAX || r->revision==UINT64_MAX) {
        r->flags|=SP_FLAG_COUNT_OVERFLOW; s->report.flags|=SP_FLAG_COUNT_OVERFLOW;
        count_up(&s->report.rejected_samples); return 0;
    }
    if(representative_better(w,&r->representative)) r->representative=*w;
    ++r->n; ++r->revision; delta=w->value-r->mean;
    r->mean+=delta/r->n; r->m2+=delta*(w->value-r->mean);
    if(w->value<r->min) r->min=w->value;
    if(w->value>r->max) r->max=w->value;
    r->t_end_us=w->t_us; r->s_end_m=s->s_m;
    r->flags|=s->bin_flags; r->quality|=w->quality;
    count_up(&s->report.spatial_samples); report_path(s); return 1;
}
int sp_add(sp_state *s,const weather_sample *w,uint64_t context,int velocity_valid)
{
    if(!s || !w) return 0;
    count_up(&s->report.samples_seen);
    if(!weather_sample_valid(w)) { count_up(&s->report.rejected_samples); return 0; }
    if(!s->have_context || context!=s->continuity_key) {
        if(s->have_context) sp_break(s,SP_FLAG_CONTEXT_CHANGE);
        s->have_context=1; s->continuity_key=context;
    }
    if(!w->has_time || !w->has_position || w->position_kind==WEATHER_POSITION_UNKNOWN || !velocity_valid || !w->has_velocity) {
        const uint32_t reason=(!velocity_valid || !w->has_velocity) ? SP_FLAG_VELOCITY_UNKNOWN : SP_FLAG_NAV_UNKNOWN;
        count_up(&s->report.unknown_samples); s->report.path_known=0;
        if(s->have_step) sp_break(s,reason); else s->report.flags|=reason;
        return 0;
    }
    if(s->have_step && w->t_us<s->last_t_us) {
        count_up(&s->report.late_samples); s->report.flags|=SP_FLAG_OUT_OF_ORDER; return 0;
    }
    if(s->have_step && w->position_kind!=s->source_kind) sp_break(s,SP_FLAG_SOURCE_CHANGE);
    if(s->have_step && w->t_us-s->last_t_us>s->options.gap_us) sp_break(s,SP_FLAG_TIME_GAP);
    if(!s->have_step) { if(!start_path(s,w)) return 0; }
    else if(w->t_us==s->last_t_us) {
        if(!s->current_step_valid || fabs(w->p[0]-s->last_p[0])>1e-6 || fabs(w->p[1]-s->last_p[1])>1e-6 ||
           fabs((double)w->v[0]-s->last_v[0])>1e-6 || fabs((double)w->v[1]-s->last_v[1])>1e-6) {
            count_up(&s->report.unknown_samples); sp_break(s,SP_FLAG_MOTION_UNKNOWN); return 0;
        }
    } else {
        const double dt=(double)(w->t_us-s->last_t_us)*1e-6;
        const double ds=.5*(hypot(s->last_v[0],s->last_v[1])+hypot((double)w->v[0],w->v[1]))*dt;
        const double px=s->predicted_displacement[0]+.5*(s->last_v[0]+w->v[0])*dt;
        const double py=s->predicted_displacement[1]+.5*(s->last_v[1]+w->v[1])*dt;
        const double compensated_step=ds-s->s_compensation;
        const double next_s=s->s_m+compensated_step;
        const double next_compensation=(next_s-s->s_m)-compensated_step;
        const double residual=hypot(w->p[0]-s->anchor_p[0]-px,w->p[1]-s->anchor_p[1]-py);
        const double tolerance=s->options.chord_tolerance_m+s->options.chord_relative_tolerance*next_s;
        if(!isfinite(ds) || !isfinite(next_s) || next_s>1e15 || !isfinite(residual) || residual>tolerance) {
            count_up(&s->report.inconsistent_steps); count_up(&s->report.unknown_samples);
            sp_break(s,SP_FLAG_MOTION_UNKNOWN);
            if(start_path(s,w)) { s->current_step_valid=0; s->report.path_known=0; }
            return 0;
        }
        s->predicted_displacement[0]=px; s->predicted_displacement[1]=py;
        s->s_m=next_s; s->s_compensation=next_compensation; s->last_t_us=w->t_us;
        s->last_p[0]=w->p[0]; s->last_p[1]=w->p[1]; s->last_v[0]=w->v[0]; s->last_v[1]=w->v[1];
        s->current_step_valid=1; s->report.path_known=1;
        if(ds>s->options.step_m) { count_up(&s->report.undersampled_steps); s->bin_flags|=SP_FLAG_UNDERSAMPLED; }
        if(next_s-s->interval_start_m>=s->width_m) {
            double next_start=s->interval_start_m+s->width_m, width=select_width(s,ds), skipped=0;
            close_open(s,SP_COMPLETE,s->bin_flags);
            /* Capacity pressure may only change the width of the NEXT bin. */
            if(s->capacity_pressure && s->options.adaptive) width=select_width(s,ds);
            if(next_s-next_start>=width) skipped=floor((next_s-next_start)/width);
            if(!isfinite(skipped) || skipped>=9007199254740991.0 ||
               (uint64_t)skipped>=UINT64_MAX-s->bin_index) {
                sp_break(s,SP_FLAG_COUNT_OVERFLOW); count_up(&s->report.unknown_samples); return 0;
            }
            s->bin_index+=1+(uint64_t)skipped;
            s->interval_start_m=next_start+skipped*width; s->width_m=width; s->entry_step_m=ds;
            s->bin_flags=s->path_flags|(ds>s->options.step_m ? SP_FLAG_UNDERSAMPLED : 0)|
                (s->capacity_pressure ? SP_FLAG_RATE_LIMITED : 0);
        }
    }
    return add_group(s,w);
}
void sp_take_snapshot(sp_state *s,sp_batch *b)
{
    unsigned i;
    if(!s || !b) return;
    memset(b,0,sizeof(*b)); report_path(s); b->options=s->options; b->report=s->report;
    while(s->completed_n) {
        b->record[b->n++]=s->completed[s->completed_head];
        s->completed_head=(s->completed_head+1)%SP_COMPLETED_CAP; --s->completed_n;
    }
    for(i=0;i<s->open_n;++i) if(s->open[i].record.revision!=s->open[i].emitted_revision) {
        b->record[b->n++]=s->open[i].record;
        s->open[i].emitted_revision=s->open[i].record.revision;
    }
    s->capacity_pressure=0; report_path(s);
}

typedef struct { char *out; size_t pos,cap; int failed; } sp_writer;
static void json_put(sp_writer *w,const char *format,...)
{
    int n; va_list args;
    if(w->failed) return;
    va_start(args,format); n=vsnprintf(w->out+w->pos,w->cap-w->pos,format,args); va_end(args);
    if(n<0 || (size_t)n>=w->cap-w->pos) { w->failed=1; return; }
    w->pos+=(size_t)n;
}
static int record_valid(const sp_record *r)
{
    return r->n && r->revision>=r->n && r->status<=SP_INTERRUPTED &&
        (r->metric==WEATHER_TEMPERATURE || r->metric==WEATHER_HUMIDITY) &&
        (r->source_kind==WEATHER_POSITION_GPS || r->source_kind==WEATHER_POSITION_ESTGPS) &&
        isfinite(r->mean) && isfinite(r->m2) && r->m2>=0 && isfinite(r->min) && isfinite(r->max) &&
        r->min<=r->max && r->mean>=r->min && r->mean<=r->max &&
        isfinite(r->width_m) && r->width_m>0 && r->width_m<=SP_MAX_WIDTH_M &&
        isfinite(r->s_start_m) && isfinite(r->s_end_m) && r->s_start_m<=r->s_end_m &&
        isfinite(r->interval_start_m) && isfinite(r->interval_end_m) && r->interval_start_m<r->interval_end_m &&
        isfinite(r->entry_step_m) && r->entry_step_m>=0 && r->t_start_us<=r->t_end_us &&
        weather_sample_valid(&r->representative) && r->representative.has_position && r->representative.has_time &&
        r->representative.position_kind==r->source_kind && r->representative.metric==r->metric &&
        r->representative.message_id==r->message_id && r->representative.sensor_id==r->sensor_id &&
        r->representative.sensor_instance==r->sensor_instance;
}
int sp_batch_json(const sp_batch *b,char *out,size_t capacity)
{
    sp_writer w; unsigned i;
    if(out && capacity) out[0]='\0';
    if(!b || !out || !capacity || !sp_options_valid(&b->options) || b->n>SP_BATCH_CAP ||
       !isfinite(b->report.s_m) || !isfinite(b->report.effective_width_m)) return -1;
    for(i=0;i<b->n;++i) if(!record_valid(&b->record[i])) return -1;
    w.out=out; w.pos=0; w.cap=capacity; w.failed=0;
    json_put(&w,"{\"schema\":\"mavlink_spatial_v1\",\"si_registry\":%u,\"target_step_m\":%.17g,\"adaptive\":%s,\"gap_us\":\"%" PRIu64 "\","
        "\"path\":{\"known\":%s,\"path_id\":\"%" PRIu64 "\",\"bin_index\":\"%" PRIu64 "\",\"s_m\":%.17g,\"effective_width_m\":%.17g},"
        "\"counters\":{\"samples_seen\":\"%" PRIu64 "\",\"spatial_samples\":\"%" PRIu64 "\",\"unknown_samples\":\"%" PRIu64 "\","
        "\"rejected_samples\":\"%" PRIu64 "\",\"late_samples\":\"%" PRIu64 "\",\"paths_started\":\"%" PRIu64 "\","
        "\"completed_records\":\"%" PRIu64 "\",\"dropped_records\":\"%" PRIu64 "\",\"undersampled_steps\":\"%" PRIu64 "\","
        "\"inconsistent_steps\":\"%" PRIu64 "\",\"flags\":%" PRIu32 "},\"bins\":[",
        WEATHER_SI_REGISTRY,b->options.step_m,b->options.adaptive ? "true" : "false",b->options.gap_us,
        b->report.path_known ? "true" : "false",b->report.path_id,b->report.bin_index,b->report.s_m,b->report.effective_width_m,
        b->report.samples_seen,b->report.spatial_samples,b->report.unknown_samples,b->report.rejected_samples,
        b->report.late_samples,b->report.paths_started,b->report.completed_records,b->report.dropped_records,
        b->report.undersampled_steps,b->report.inconsistent_steps,b->report.flags);
    for(i=0;i<b->n && !w.failed;++i) {
        const sp_record *r=&b->record[i]; char representative[2048];
        const int sample_length=weather_sample_json(&r->representative,representative,sizeof(representative));
        if(sample_length<0) { w.failed=1; break; }
        json_put(&w,"%s{\"continuity_key\":\"%" PRIu64 "\",\"path_id\":\"%" PRIu64 "\",\"bin_index\":\"%" PRIu64 "\",\"revision\":\"%" PRIu64 "\","
            "\"state\":\"%s\",\"metric\":\"%s\",\"message_id\":%" PRIu32 ",\"sensor_id\":%" PRIu32 ",\"sensor_instance\":%u,\"source_kind\":\"%s\","
            "\"flags\":%" PRIu32 ",\"quality\":%" PRIu32 ",\"width_m\":%.17g,\"interval_start_m\":%.17g,\"interval_end_m\":%.17g,"
            "\"observed_start_m\":%.17g,\"observed_end_m\":%.17g,\"entry_step_m\":%.17g,\"t_start_us\":\"%" PRIu64 "\",\"t_end_us\":\"%" PRIu64 "\","
            "\"statistics\":{\"n\":%" PRIu32 ",\"mean\":%.17g,\"m2\":%.17g,\"variance\":",
            i ? "," : "",r->continuity_key,r->path_id,r->bin_index,r->revision,
            r->status==SP_PARTIAL ? "partial" : r->status==SP_COMPLETE ? "complete" : "interrupted",
            r->metric==WEATHER_TEMPERATURE ? "temperature" : "humidity",r->message_id,r->sensor_id,(unsigned)r->sensor_instance,
            r->source_kind==WEATHER_POSITION_GPS ? "GPS" : "ESTGPS",r->flags,r->quality,r->width_m,
            r->interval_start_m,r->interval_end_m,r->s_start_m,r->s_end_m,r->entry_step_m,r->t_start_us,r->t_end_us,
            r->n,r->mean,r->m2);
        if(r->n>1) json_put(&w,"%.17g",r->m2/(r->n-1)); else json_put(&w,"null");
        json_put(&w,",\"min\":%.17g,\"max\":%.17g},\"representative\":%s}",r->min,r->max,representative);
    }
    json_put(&w,"]}");
    if(w.failed || w.pos>2147483647u) { out[0]='\0'; return -1; }
    return (int)w.pos;
}
