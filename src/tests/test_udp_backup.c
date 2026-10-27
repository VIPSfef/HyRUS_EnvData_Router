#include "udp_backup.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define CHECK(test) do { ++checks; if(!(test)) { fprintf(stderr,"FAIL %s:%d %s\n",__FILE__,__LINE__,#test); return 0; } } while(0)
typedef struct { unsigned calls; size_t length; uint64_t hash; } capture;
static uint64_t hash_bytes(const char *data,size_t size) {
    uint64_t hash=UINT64_C(14695981039346656037); size_t i;
    for(i=0;i<size;++i) { hash^=(unsigned char)data[i]; hash*=UINT64_C(1099511628211); }
    return hash;
}
static void completed(void *context,const char *json,size_t length) {
    capture *out=context; ++out->calls; out->length=length; out->hash=hash_bytes(json,length);
}
static ub_peer peer(unsigned port) {
    ub_peer p; memset(&p,0,sizeof(p)); p.family=4; p.address[0]=127; p.address[3]=1; p.port=(uint16_t)port;
    return p;
}
static void make_json(char *out,size_t length) {
    size_t i; memcpy(out,"{\"v\":\"",6);
    for(i=6;i<length-2;++i) out[i]=(char)('a'+i%26);
    memcpy(out+length-2,"\"}",2);
}
static uint64_t number(const uint8_t *p,unsigned size) {
    uint64_t n=0; while(size--) n=n*256u+*p++; return n;
}
static int framing_and_reorder(void) {
    char json[UB_PAYLOAD_CAP*2+17]; uint8_t frame[UB_DATAGRAM_CAP];
    capture out={0}; ub_peer p=peer(3000); ub_receiver *r=ub_receiver_create(completed,&out);
    ub_report report; unsigned index; size_t bytes;
    CHECK(r!=NULL); make_json(json,sizeof(json));
    bytes=ub_fragment(UINT64_C(0x0102030405060708),UINT64_C(0x1011121314151617),json,sizeof(json),0,frame,sizeof(frame));
    CHECK(bytes==UB_DATAGRAM_CAP && !memcmp(frame,"MTRU",4));
    CHECK(frame[4]==1 && frame[5]==0 && number(frame+6,2)==40);
    CHECK(number(frame+8,8)==UINT64_C(0x0102030405060708));
    CHECK(number(frame+16,8)==UINT64_C(0x1011121314151617));
    CHECK(number(frame+24,4)==sizeof(json) && number(frame+28,4)==0);
    CHECK(number(frame+32,2)==0 && number(frame+34,2)==3 && number(frame+36,2)==1160 && number(frame+38,2)==0);
    for(index=3;index>0;--index) {
        bytes=ub_fragment(7,9,json,sizeof(json),index-1,frame,sizeof(frame));
        CHECK(bytes>UB_HEADER_BYTES && bytes<=UB_DATAGRAM_CAP);
        ub_receiver_feed(r,&p,frame,bytes,1000); ub_receiver_feed(r,&p,frame,bytes,1001);
        CHECK(out.calls==(index==1 ? 1u : 0u));
    }
    CHECK(out.length==sizeof(json) && out.hash==hash_bytes(json,sizeof(json)));
    ub_receiver_get_report(r,&report);
    CHECK(report.messages==1 && report.datagrams==6 && report.errors==0 && report.drops==0);
    CHECK(!ub_fragment(1,1,json,sizeof(json),3,frame,sizeof(frame)));
    CHECK(!ub_fragment(1,1,json,sizeof(json),0,frame,UB_HEADER_BYTES));
    CHECK(!ub_fragment(1,1,json,0,0,frame,sizeof(frame)));
    CHECK(!ub_fragment(1,1,json,UB_JSON_CAP+1u,0,frame,sizeof(frame)));
    ub_receiver_close(r); return 1;
}
static int malformed_and_conflicting_duplicates(void) {
    static const unsigned corrupt_offsets[]={0,4,5,7,24,28,32,34,36,38};
    char json[UB_PAYLOAD_CAP+20]; uint8_t frame[UB_DATAGRAM_CAP+1],copy[UB_DATAGRAM_CAP+1];
    ub_peer p=peer(3000),invalid=p; capture out={0}; ub_report report; unsigned i;
    ub_receiver *r=ub_receiver_create(completed,&out); size_t bytes;
    CHECK(r!=NULL); make_json(json,sizeof(json));
    bytes=ub_fragment(1,1,json,sizeof(json),0,frame,sizeof(frame));
    for(i=0;i<sizeof(corrupt_offsets)/sizeof(corrupt_offsets[0]);++i) {
        memcpy(copy,frame,bytes); copy[corrupt_offsets[i]]^=0x80;
        ub_receiver_feed(r,&p,copy,bytes,1000);
    }
    ub_receiver_feed(r,&p,frame,bytes-1,1000);
    ub_receiver_feed(r,&p,frame,UB_DATAGRAM_CAP+1u,1000);
    invalid.family=0; ub_receiver_feed(r,&invalid,frame,bytes,1000);
    ub_receiver_feed(r,NULL,frame,bytes,1000);
    ub_receiver_feed(r,&p,NULL,bytes,1000);
    CHECK(out.calls==0); ub_receiver_get_report(r,&report); CHECK(report.errors==15);
    ub_receiver_feed(r,&p,frame,bytes,1000);
    memcpy(copy,frame,bytes); copy[UB_HEADER_BYTES]^=1; ub_receiver_feed(r,&p,copy,bytes,1001);
    ub_receiver_get_report(r,&report); CHECK(report.errors==16 && report.drops==1 && out.calls==0);
    bytes=ub_fragment(1,1,json,sizeof(json),1,frame,sizeof(frame));
    ub_receiver_feed(r,&p,frame,bytes,1002); CHECK(out.calls==0); /* Bad first fragment cannot complete a record. */
    ub_receiver_close(r); return 1;
}
static int expiry_and_capacity(void) {
    char json[UB_PAYLOAD_CAP+20]; uint8_t frame[UB_DATAGRAM_CAP];
    ub_peer p=peer(3000); capture out={0}; ub_report report; unsigned i; size_t bytes;
    ub_receiver *r=ub_receiver_create(completed,&out);
    CHECK(r!=NULL); make_json(json,sizeof(json));
    bytes=ub_fragment(1,1,json,sizeof(json),0,frame,sizeof(frame));
    ub_receiver_feed(r,&p,frame,bytes,1000); ub_receiver_feed(r,&p,frame,bytes,2999);
    ub_receiver_expire(r,3000); ub_receiver_get_report(r,&report);
    CHECK(report.drops==1 && out.calls==0); /* Duplicate traffic does not extend the deadline. */
    for(i=0;i<UB_ASSEMBLY_CAP+1u;++i) {
        bytes=ub_fragment(1,10+i,json,sizeof(json),0,frame,sizeof(frame));
        ub_receiver_feed(r,&p,frame,bytes,4000+i);
    }
    ub_receiver_get_report(r,&report); CHECK(report.drops==2);
    for(i=1;i<UB_ASSEMBLY_CAP+1u;++i) {
        bytes=ub_fragment(1,10+i,json,sizeof(json),1,frame,sizeof(frame));
        ub_receiver_feed(r,&p,frame,bytes,4100);
    }
    CHECK(out.calls==UB_ASSEMBLY_CAP);
    ub_receiver_expire(r,7000); ub_receiver_get_report(r,&report); CHECK(report.drops==2);
    ub_receiver_close(r); return 1;
}
static int peers_do_not_mix(void) {
    char first[UB_PAYLOAD_CAP+20],second[UB_PAYLOAD_CAP+20]; uint8_t frame[UB_DATAGRAM_CAP];
    ub_peer a=peer(3000),b=peer(3001); capture out={0}; ub_receiver *r=ub_receiver_create(completed,&out); size_t bytes;
    CHECK(r!=NULL); make_json(first,sizeof(first)); memcpy(second,first,sizeof(first)); second[10]='!';
    bytes=ub_fragment(1,1,first,sizeof(first),0,frame,sizeof(frame)); ub_receiver_feed(r,&a,frame,bytes,1000);
    bytes=ub_fragment(1,1,second,sizeof(second),1,frame,sizeof(frame)); ub_receiver_feed(r,&b,frame,bytes,1000);
    CHECK(out.calls==0);
    bytes=ub_fragment(1,1,second,sizeof(second),0,frame,sizeof(frame)); ub_receiver_feed(r,&b,frame,bytes,1001);
    CHECK(out.calls==1 && out.hash==hash_bytes(second,sizeof(second)));
    bytes=ub_fragment(1,1,first,sizeof(first),1,frame,sizeof(frame)); ub_receiver_feed(r,&a,frame,bytes,1001);
    CHECK(out.calls==2 && out.hash==hash_bytes(first,sizeof(first)));
    a.family=b.family=6; a.port=b.port=3000; a.scope_id=1; b.scope_id=2;
    bytes=ub_fragment(2,1,first,sizeof(first),0,frame,sizeof(frame)); ub_receiver_feed(r,&a,frame,bytes,1100);
    bytes=ub_fragment(2,1,second,sizeof(second),1,frame,sizeof(frame)); ub_receiver_feed(r,&b,frame,bytes,1100);
    CHECK(out.calls==2); /* IPv6 scoped addresses on different interfaces are distinct peers. */
    ub_receiver_close(r); return 1;
}
static int largest_body_and_api_edges(void) {
    char *json=malloc(UB_JSON_CAP); uint8_t frame[UB_DATAGRAM_CAP];
    ub_peer p=peer(3000); capture out={0}; ub_receiver *r=ub_receiver_create(completed,&out);
    unsigned i,count=(UB_JSON_CAP+UB_PAYLOAD_CAP-1)/UB_PAYLOAD_CAP,error=0; ub_report report;
    CHECK(json!=NULL && r!=NULL); make_json(json,UB_JSON_CAP);
    for(i=count;i>0;--i) {
        size_t bytes=ub_fragment(UINT64_MAX,UINT64_MAX,json,UB_JSON_CAP,i-1,frame,sizeof(frame));
        CHECK(bytes>0); ub_receiver_feed(r,&p,frame,bytes,1000);
    }
    CHECK(out.calls==1 && out.length==UB_JSON_CAP && out.hash==hash_bytes(json,UB_JSON_CAP));
    ub_receiver_get_report(r,&report); CHECK(report.messages==1 && report.datagrams==count && report.errors==0);
    CHECK(ub_receiver_create(NULL,NULL)==NULL);
    CHECK(ub_sender_open("127.0.0.1",65536,1,&error)==NULL && error!=0);
    CHECK(!ub_sender_send(NULL,"{}",2));
    ub_sender_get_report(NULL,&report); CHECK(report.messages==0 && report.errors==0);
    ub_receiver_poll(NULL,0); ub_receiver_feed(NULL,&p,frame,0,0);
    ub_receiver_close(r); ub_sender_close(NULL); free(json); return 1;
}
int main(void) {
    if(!framing_and_reorder() || !malformed_and_conflicting_duplicates() || !expiry_and_capacity() ||
       !peers_do_not_mix() || !largest_body_and_api_edges()) return 1;
    printf("udp_backup: %u checks passed\n",checks); return 0;
}
