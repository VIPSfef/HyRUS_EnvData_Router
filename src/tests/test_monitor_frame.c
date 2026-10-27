/* Verify the terminal-visible result, not an implementation-specific sequence
 * of cursor writes. All output is captured in disposable test streams. */
#include "monitor_frame.h"
#include <stdio.h>
#include <string.h>

enum { TEST_ROWS=8, TEST_COLUMNS=80, CAPTURE_BYTES=MF_FRAME_BYTES+MF_MAX_ROWS*64 };
typedef struct {
    char cells[TEST_ROWS][TEST_COLUMNS];
    unsigned styles[TEST_ROWS][TEST_COLUMNS];
    unsigned row,column,rows,columns,style;
    int wrapped,scrolled;
} screen;
static mf_frame before,after;
static char captured[CAPTURE_BYTES];
static unsigned checks;
#define CHECK(condition) do { ++checks; if(!(condition)) { \
    fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#condition); return 0; \
} } while(0)

static void screen_reset(screen *s,unsigned rows,unsigned columns)
{
    memset(s,0,sizeof(*s));
    memset(s->cells,' ',sizeof(s->cells));
    s->rows=rows; s->columns=columns;
}
static void erase_line(screen *s,unsigned row,unsigned first)
{
    unsigned column;
    for(column=first;column<s->columns;++column) {
        s->cells[row][column]=' '; s->styles[row][column]=s->style;
    }
}
static int screen_feed(screen *s,const char *bytes,size_t count)
{
    size_t i=0;
    while(i<count) {
        unsigned char ch=(unsigned char)bytes[i++];
        if(ch==27) {
            unsigned argument[4]={0,0,0,0},n=0,row;
            int private_mode=0;
            CHECK(i<count && bytes[i++]=='[');
            if(i<count && bytes[i]=='?') { private_mode=1; ++i; }
            while(i<count && ((bytes[i]>='0' && bytes[i]<='9') || bytes[i]==';')) {
                if(bytes[i]==';') { CHECK(n+1<4); ++n; }
                else argument[n]=argument[n]*10u+(unsigned)(bytes[i]-'0');
                ++i;
            }
            CHECK(i<count);
            ch=(unsigned char)bytes[i++];
            if(private_mode) { CHECK(ch=='h' || ch=='l'); continue; }
            if(ch=='H' || ch=='f') {
                s->row=argument[0] ? argument[0]-1u : 0;
                s->column=argument[1] ? argument[1]-1u : 0;
                CHECK(s->row<s->rows && s->column<s->columns);
            } else if(ch=='m') {
                unsigned index;
                for(index=0;index<=n;++index) s->style=argument[index];
            } else if(ch=='K') {
                CHECK(argument[0]==0 || argument[0]==2);
                erase_line(s,s->row,argument[0]==2 ? 0 : s->column);
            } else if(ch=='J') {
                CHECK(argument[0]==0 || argument[0]==2);
                row=argument[0]==2 ? 0 : s->row;
                if(argument[0]==0) { erase_line(s,row,s->column); ++row; }
                for(;row<s->rows;++row) erase_line(s,row,0);
            } else CHECK(0);
        } else if(ch=='\r') s->column=0;
        else if(ch=='\n') {
            if(s->row+1>=s->rows) s->scrolled=1;
            else ++s->row;
        } else {
            CHECK(ch>=32 && ch<127);
            if(s->column>=s->columns) { s->wrapped=1; return 0; }
            s->cells[s->row][s->column]=(char)ch;
            s->styles[s->row][s->column]=s->style;
            ++s->column;
        }
    }
    CHECK(!s->wrapped && !s->scrolled);
    return 1;
}
static int line_is(const screen *s,unsigned row,const char *text)
{
    size_t n=strlen(text);
    unsigned column;
    if(row>=s->rows || n>s->columns || memcmp(s->cells[row],text,n)) return 0;
    for(column=(unsigned)n;column<s->columns;++column) if(s->cells[row][column]!=' ') return 0;
    return 1;
}
static int frame_set(mf_frame *frame,const char *text)
{
    mf_reset(frame);
    return mf_append(frame,text,strlen(text));
}
static int capture(const mf_frame *current,const mf_frame *previous,
                   unsigned rows,unsigned columns,int repaint,size_t *count)
{
    FILE *out=tmpfile();
    long length;
    int ok;
    CHECK(out!=NULL);
    ok=mf_present(out,current,previous,rows,columns,repaint);
    CHECK(fflush(out)==0);
    length=ftell(out);
    CHECK(length>=0 && (size_t)length<sizeof(captured));
    *count=(size_t)length;
    rewind(out);
    CHECK(fread(captured,1,*count,out)==*count);
    captured[*count]='\0';
    CHECK(fclose(out)==0);
    return ok;
}
static int check_dirty_rows(void)
{
    screen display;
    size_t count;
    screen_reset(&display,TEST_ROWS,TEST_COLUMNS);
    memset(display.cells,'X',sizeof(display.cells));
    CHECK(frame_set(&before,"fixed header\npackets=100000\nfixed footer\n"));
    CHECK(capture(&before,NULL,display.rows,display.columns,1,&count));
    CHECK(screen_feed(&display,captured,count));
    CHECK(line_is(&display,0,"fixed header"));
    CHECK(line_is(&display,1,"packets=100000"));
    CHECK(line_is(&display,2,"fixed footer"));
    CHECK(line_is(&display,3,""));
    CHECK(line_is(&display,TEST_ROWS-1,""));
    CHECK(capture(&before,&before,display.rows,display.columns,0,&count));
    CHECK(count==0);

    CHECK(frame_set(&after,"fixed header\npackets=2\nfixed footer\n"));
    CHECK(capture(&after,&before,display.rows,display.columns,0,&count));
    CHECK(count>0);
    CHECK(strstr(captured,"fixed header")==NULL && strstr(captured,"fixed footer")==NULL);
    CHECK(strstr(captured,"\033[2J")==NULL && strchr(captured,'\n')==NULL);
    CHECK(screen_feed(&display,captured,count));
    CHECK(line_is(&display,0,"fixed header"));
    CHECK(line_is(&display,1,"packets=2"));
    CHECK(line_is(&display,2,"fixed footer"));
    CHECK(before.used==strlen("fixed header\npackets=100000\nfixed footer\n"));
    CHECK(memcmp(before.text,"fixed header\npackets=100000\nfixed footer\n",before.used)==0);

    before=after;
    CHECK(frame_set(&after,"fixed header\n"));
    CHECK(capture(&after,&before,display.rows,display.columns,0,&count));
    CHECK(screen_feed(&display,captured,count));
    CHECK(line_is(&display,0,"fixed header"));
    CHECK(line_is(&display,1,"") && line_is(&display,2,""));
    return 1;
}
static int check_colours_and_repaint(void)
{
    screen display;
    size_t count;
    unsigned row;
    screen_reset(&display,TEST_ROWS,TEST_COLUMNS);
    CHECK(frame_set(&before,"source=\033[33mWAIT\033[0m\nfixed\n"));
    CHECK(capture(&before,NULL,display.rows,display.columns,1,&count));
    CHECK(screen_feed(&display,captured,count));
    CHECK(line_is(&display,0,"source=WAIT"));
    CHECK(display.styles[0][7]==33 && display.styles[1][0]==0);
    CHECK(frame_set(&after,"source=\033[32mWAIT\033[0m\nfixed\n"));
    CHECK(capture(&after,&before,display.rows,display.columns,0,&count));
    CHECK(count>0 && strstr(captured,"\033[32mWAIT\033[0m")!=NULL);
    CHECK(strstr(captured,"fixed")==NULL);
    CHECK(screen_feed(&display,captured,count));
    CHECK(display.styles[0][7]==32 && display.styles[1][0]==0);

    /* A resized viewport can contain arbitrary stale text despite matching
     * previous line contents. Explicit repaint must repair the whole view. */
    display.rows=5; display.columns=18;
    memset(display.cells,'X',sizeof(display.cells));
    CHECK(capture(&after,&after,display.rows,display.columns,1,&count));
    CHECK(count>0 && strchr(captured,'\n')==NULL);
    CHECK(screen_feed(&display,captured,count));
    CHECK(line_is(&display,0,"source=WAIT") && line_is(&display,1,"fixed"));
    for(row=2;row<display.rows;++row) CHECK(line_is(&display,row,""));

    /* Filling every row must never emit a scrolling newline at the bottom. */
    CHECK(frame_set(&before,"a\nb\nc\nd\ne\n"));
    CHECK(capture(&before,&after,display.rows,display.columns,0,&count));
    CHECK(strchr(captured,'\n')==NULL);
    CHECK(screen_feed(&display,captured,count));
    CHECK(line_is(&display,4,"e"));
    CHECK(display.row<display.rows);
    CHECK(frame_set(&after,"final line without newline"));
    display.columns=TEST_COLUMNS;
    CHECK(capture(&after,&before,display.rows,display.columns,1,&count));
    CHECK(screen_feed(&display,captured,count));
    CHECK(line_is(&display,0,"final line without newline"));
    for(row=1;row<display.rows;++row) CHECK(line_is(&display,row,""));
    return 1;
}
static int check_capacity(void)
{
    static char fill[MF_FRAME_BYTES];
    size_t count;
    memset(fill,'x',sizeof(fill));
    mf_reset(&after);
    CHECK(after.used==0 && !after.overflow);
    CHECK(mf_append(&after,fill,sizeof(fill)-1));
    CHECK(after.used==sizeof(fill)-1 && !after.overflow);
    CHECK(mf_append(&after,"x",1));
    CHECK(after.used==sizeof(fill) && !after.overflow);
    CHECK(!mf_append(&after,"overflow",8));
    CHECK(after.used==sizeof(fill) && after.overflow);
    CHECK(memcmp(after.text,fill,sizeof(fill))==0);
    CHECK(!capture(&after,NULL,TEST_ROWS,TEST_COLUMNS,1,&count));
    CHECK(count==0);
    mf_reset(&after);
    CHECK(after.used==0 && !after.overflow);
    CHECK(mf_append(&after,"safe",4));
    CHECK(!mf_append(&after,fill,sizeof(fill)));
    CHECK(after.used==4 && after.overflow && memcmp(after.text,"safe",4)==0);
    return 1;
}
static int check_failed_output(void)
{
#ifdef _WIN32
    const char *null_device="NUL";
#else
    const char *null_device="/dev/null";
#endif
    FILE *out=fopen(null_device,"r");
    CHECK(out!=NULL);
    CHECK(frame_set(&after,"cannot write to a read-only stream\n"));
    CHECK(!mf_present(out,&after,NULL,TEST_ROWS,TEST_COLUMNS,1));
    CHECK(ferror(out));
    CHECK(fclose(out)==0);
    return 1;
}
int main(void)
{
    if(!check_dirty_rows() || !check_colours_and_repaint() || !check_capacity() || !check_failed_output()) return 1;
    printf("PASS terminal frame: %u checks; dirty rows, stale cells, colours, resize, bounds\n",checks);
    return 0;
}
