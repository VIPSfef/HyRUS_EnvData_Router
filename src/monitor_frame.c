#include "monitor_frame.h"

#include <string.h>

enum { MF_ROW_CONTROL_BYTES=32, MF_CONTROL_BYTES=64,
       MF_UPDATE_BYTES=MF_FRAME_BYTES+MF_MAX_ROWS*MF_ROW_CONTROL_BYTES+MF_CONTROL_BYTES };
/* Kept off the receiver/monitor thread stacks. Only the monitor presents. */
static char update[MF_UPDATE_BYTES];
size_t mf_storage_bytes(void) { return sizeof(update); }

void mf_reset(mf_frame *frame)
{ frame->used=0; frame->overflow=0; }

int mf_append(mf_frame *frame,const char *text,size_t length)
{
    if(frame->overflow || length>sizeof(frame->text)-frame->used) {
        frame->overflow=1; return 0;
    }
    memcpy(frame->text+frame->used,text,length);
    frame->used+=length;
    return 1;
}

typedef struct { const char *text; size_t length; } row;

static row next_row(const mf_frame *frame,size_t *offset)
{
    row result={"",0};
    size_t first=*offset;
    if(!frame || first>=frame->used) return result;
    while(*offset<frame->used && frame->text[*offset]!='\n') ++*offset;
    result.text=frame->text+first; result.length=*offset-first;
    if(*offset<frame->used) ++*offset;
    return result;
}

static int append_update(size_t *used,const char *text,size_t length)
{
    if(length>sizeof(update)-*used) return 0;
    memcpy(update+*used,text,length); *used+=length;
    return 1;
}

static int move_to(size_t *used,unsigned line)
{
    char control[MF_ROW_CONTROL_BYTES];
    int length=snprintf(control,sizeof(control),"\033[%u;1H",line);
    return length>0 && (size_t)length<sizeof(control) && append_update(used,control,(size_t)length);
}

int mf_present(FILE *out,const mf_frame *current,const mf_frame *previous,
               unsigned rows,unsigned columns,int repaint)
{
    size_t used=0,offset=0,old_offset=0;
    unsigned line=0,current_lines=0;
    int all=repaint || !previous;
    (void)columns; /* Width clipping is performed by the shared text writer. */
    if(!out || !current || current->overflow || (previous && previous->overflow) || !rows) return 0;
    if(rows>MF_MAX_ROWS) rows=MF_MAX_ROWS;
    while(line<rows && (offset<current->used || (previous && old_offset<previous->used))) {
        int have_current=offset<current->used;
        row now=next_row(current,&offset),old=next_row(previous,&old_offset);
        ++line;
        if(have_current) current_lines=line;
        if(!all && now.length==old.length && !memcmp(now.text,old.text,now.length)) continue;
        if(!used && !append_update(&used,"\033[?25l",6)) return 0;
        /* Overwrite first, then clear the unused suffix. Clearing the row
         * before writing it would reintroduce the visible blank interval. */
        if(!move_to(&used,line) || !append_update(&used,now.text,now.length) ||
           !append_update(&used,"\033[0m\033[K",7)) return 0;
    }
    if(all || used) {
        unsigned below=current_lines<rows ? current_lines+1 : rows;
        if(!used && !append_update(&used,"\033[?25l",6)) return 0;
        if(!append_update(&used,"\033[0m",4) || !move_to(&used,below)) return 0;
        if(all && current_lines<rows && !append_update(&used,"\033[J",3)) return 0;
    }
    return !used || (fwrite(update,1,used,out)==used && fflush(out)==0);
}
