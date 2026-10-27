#ifndef _WIN32
#define _POSIX_C_SOURCE 200112L
#endif
#include "udp_backup.h"
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET ub_socket;
#define UB_INVALID INVALID_SOCKET
#define ub_close closesocket
static unsigned socket_error(void) { return (unsigned)WSAGetLastError(); }
static int would_block(unsigned error) { return error==WSAEWOULDBLOCK; }
static int nonblocking(ub_socket socket_fd) { u_long mode=1; return ioctlsocket(socket_fd,FIONBIO,&mode)==0; }
#else
#include <fcntl.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int ub_socket;
#define UB_INVALID (-1)
#define ub_close close
static unsigned socket_error(void) { return (unsigned)errno; }
static int would_block(unsigned error) { return error==EAGAIN || error==EWOULDBLOCK; }
static int nonblocking(ub_socket socket_fd) {
    int flags=fcntl(socket_fd,F_GETFL,0);
    return flags>=0 && fcntl(socket_fd,F_SETFL,flags|O_NONBLOCK)==0;
}
#endif

enum { UB_VERSION=1, UB_JSON_KIND=0,
       UB_FRAGMENT_CAP=(UB_JSON_CAP+UB_PAYLOAD_CAP-1)/UB_PAYLOAD_CAP,
       UB_BITMAP_BYTES=(UB_FRAGMENT_CAP+CHAR_BIT-1)/CHAR_BIT };
typedef struct {
    ub_peer peer;
    uint64_t nonce,sequence,started_ms;
    uint32_t total;
    unsigned count,received,complete;
    uint8_t bitmap[UB_BITMAP_BYTES];
    char *body;
} ub_assembly;
struct ub_sender {
    ub_socket socket_fd;
    struct sockaddr_storage destination;
    int destination_length,sockets_ready;
    uint64_t nonce,sequence;
    ub_report report;
};
struct ub_receiver {
    ub_socket socket_fd;
    int sockets_ready;
    ub_message_fn callback;
    void *context;
    ub_assembly slot[UB_ASSEMBLY_CAP];
    ub_report report;
};
static void write_number(uint8_t *out,uint64_t value,unsigned bytes) {
    while(bytes) { out[--bytes]=(uint8_t)value; value>>=CHAR_BIT; }
}
static uint64_t read_number(const uint8_t *in,unsigned bytes) {
    uint64_t value=0;
    while(bytes--) value=(value<<CHAR_BIT)|*in++;
    return value;
}
size_t ub_fragment(uint64_t nonce,uint64_t sequence,const char *json,size_t length,
                   unsigned index,uint8_t *out,size_t capacity) {
    size_t offset,payload; unsigned count;
    if(!json || !length || length>UB_JSON_CAP || !out) return 0;
    count=(unsigned)((length+UB_PAYLOAD_CAP-1u)/UB_PAYLOAD_CAP);
    if(index>=count) return 0;
    offset=(size_t)index*UB_PAYLOAD_CAP; payload=length-offset;
    if(payload>UB_PAYLOAD_CAP) payload=UB_PAYLOAD_CAP;
    if(capacity<UB_HEADER_BYTES+payload) return 0;
    memset(out,0,UB_HEADER_BYTES); memcpy(out,"MTRU",4);
    out[4]=UB_VERSION; out[5]=UB_JSON_KIND;
    write_number(out+6,UB_HEADER_BYTES,2); write_number(out+8,nonce,8);
    write_number(out+16,sequence,8); write_number(out+24,length,4);
    write_number(out+28,offset,4); write_number(out+32,index,2);
    write_number(out+34,count,2); write_number(out+36,payload,2);
    memcpy(out+UB_HEADER_BYTES,json+offset,payload);
    return UB_HEADER_BYTES+payload;
}
static void report_error(ub_report *report,unsigned error) {
    ++report->errors; report->last_error=error;
}
static int peer_equal(const ub_peer *a,const ub_peer *b) {
    return a->family==b->family && a->port==b->port &&
        (a->family==4 || a->scope_id==b->scope_id) &&
        !memcmp(a->address,b->address,a->family==4 ? 4u : 16u);
}
static void clear_slot(ub_receiver *r,ub_assembly *slot) {
    if(slot->body && !slot->complete) ++r->report.drops;
    free(slot->body); memset(slot,0,sizeof(*slot));
}
ub_receiver *ub_receiver_create(ub_message_fn callback,void *context) {
    ub_receiver *r;
    if(!callback) return NULL;
    r=calloc(1,sizeof(*r));
    if(r) { r->socket_fd=UB_INVALID; r->callback=callback; r->context=context; }
    return r;
}
void ub_receiver_expire(ub_receiver *r,uint64_t now) {
    unsigned i;
    if(!r) return;
    for(i=0;i<UB_ASSEMBLY_CAP;++i) {
        ub_assembly *s=&r->slot[i];
        if(s->body && (now<s->started_ms || now-s->started_ms>=UB_EXPIRE_MS)) clear_slot(r,s);
    }
}
/* Validate canonical, disjoint fragment geometry before allocation/copy. Each
 * transfer owns one bounded slot; only a complete bitmap reaches the callback.
 * Completed slots briefly retain their identity to suppress duplicate delivery. */
void ub_receiver_feed(ub_receiver *r,const ub_peer *peer,const uint8_t *data,size_t size,uint64_t now) {
    uint64_t nonce,sequence; uint32_t total,offset;
    unsigned index,count,payload,expected,i; ub_assembly *slot=NULL,*oldest=NULL;
    if(!r) return;
    ++r->report.datagrams; r->report.bytes+=size; ub_receiver_expire(r,now);
    if(!peer || (peer->family!=4 && peer->family!=6) || !data || size<UB_HEADER_BYTES ||
       size>UB_DATAGRAM_CAP || memcmp(data,"MTRU",4) || data[4]!=UB_VERSION || data[5]!=UB_JSON_KIND ||
       read_number(data+6,2)!=UB_HEADER_BYTES || read_number(data+38,2)) goto malformed;
    nonce=read_number(data+8,8); sequence=read_number(data+16,8);
    total=(uint32_t)read_number(data+24,4); offset=(uint32_t)read_number(data+28,4);
    index=(unsigned)read_number(data+32,2); count=(unsigned)read_number(data+34,2);
    payload=(unsigned)read_number(data+36,2);
    if(!total || total>UB_JSON_CAP || count!=(total+UB_PAYLOAD_CAP-1u)/UB_PAYLOAD_CAP || index>=count ||
       offset!=(uint32_t)index*UB_PAYLOAD_CAP || offset>=total) goto malformed;
    expected=total-offset; if(expected>UB_PAYLOAD_CAP) expected=UB_PAYLOAD_CAP;
    if(payload!=expected || size!=UB_HEADER_BYTES+payload) goto malformed;
    for(i=0;i<UB_ASSEMBLY_CAP;++i) {
        ub_assembly *s=&r->slot[i];
        if(s->body && s->nonce==nonce && s->sequence==sequence && peer_equal(&s->peer,peer)) { slot=s; break; }
    }
    if(slot && (slot->total!=total || slot->count!=count)) { clear_slot(r,slot); goto malformed; }
    if(!slot) {
        for(i=0;i<UB_ASSEMBLY_CAP;++i) {
            ub_assembly *s=&r->slot[i];
            if(!s->body) { slot=s; break; }
            if(!oldest || (s->complete && !oldest->complete) ||
               (s->complete==oldest->complete && s->started_ms<oldest->started_ms)) oldest=s;
        }
        if(!slot) { slot=oldest; clear_slot(r,slot); }
        slot->body=malloc((size_t)total+1u);
        if(!slot->body) { report_error(&r->report,ENOMEM); ++r->report.drops; return; }
        slot->peer=*peer; slot->nonce=nonce; slot->sequence=sequence; slot->started_ms=now;
        slot->total=total; slot->count=count; slot->body[total]='\0';
    }
    if(slot->bitmap[index/CHAR_BIT]&(1u<<(index%CHAR_BIT))) {
        if(memcmp(slot->body+offset,data+UB_HEADER_BYTES,payload)) { clear_slot(r,slot); goto malformed; }
        return;
    }
    memcpy(slot->body+offset,data+UB_HEADER_BYTES,payload);
    slot->bitmap[index/CHAR_BIT]|=(uint8_t)(1u<<(index%CHAR_BIT)); ++slot->received;
    if(slot->received==slot->count) {
        slot->complete=1; ++r->report.messages;
        r->callback(r->context,slot->body,slot->total);
    }
    return;
malformed:
    report_error(&r->report,EINVAL);
}
static int start_sockets(unsigned *error) {
#ifdef _WIN32
    WSADATA data; int result=WSAStartup(MAKEWORD(2,2),&data);
    if(result) { if(error) *error=(unsigned)result; return 0; }
#else
    (void)error;
#endif
    return 1;
}
static void stop_sockets(int ready) {
#ifdef _WIN32
    if(ready) (void)WSACleanup();
#else
    (void)ready;
#endif
}
static ub_socket open_socket(const char *host,unsigned port,int receiving,
                             struct sockaddr_storage *destination,int *destination_length,unsigned *error) {
    struct addrinfo hints,*list=NULL,*item; char service[6]; ub_socket result=UB_INVALID;
    int status;
    if(port>UINT16_MAX || (!receiving && (!host || !*host || !port))) { *error=EINVAL; return result; }
    memset(&hints,0,sizeof(hints)); hints.ai_family=AF_UNSPEC;
    hints.ai_socktype=SOCK_DGRAM; hints.ai_protocol=IPPROTO_UDP;
    if(receiving) hints.ai_flags=AI_PASSIVE;
    (void)snprintf(service,sizeof(service),"%u",port);
    status=getaddrinfo(host && *host ? host : NULL,service,&hints,&list);
    if(status) { *error=(unsigned)(status<0 ? -status : status); return result; }
    for(item=list;item;item=item->ai_next) {
        ub_socket candidate;
        if(item->ai_addrlen>sizeof(*destination)) continue;
        candidate=socket(item->ai_family,item->ai_socktype,item->ai_protocol);
        if(candidate==UB_INVALID) { *error=socket_error(); continue; }
        if(!nonblocking(candidate) || (receiving && bind(candidate,item->ai_addr,(int)item->ai_addrlen))) {
            *error=socket_error(); (void)ub_close(candidate); continue;
        }
        memcpy(destination,item->ai_addr,item->ai_addrlen); *destination_length=(int)item->ai_addrlen;
        result=candidate; *error=0; break;
    }
    freeaddrinfo(list); return result;
}
ub_sender *ub_sender_open(const char *host,unsigned port,uint64_t nonce,unsigned *error) {
    ub_sender *s=calloc(1,sizeof(*s)); unsigned failure=0;
    if(error) *error=0;
    if(!s) { if(error) *error=ENOMEM; return NULL; }
    s->socket_fd=UB_INVALID; s->nonce=nonce;
    if((s->sockets_ready=start_sockets(&failure))!=0)
        s->socket_fd=open_socket(host,port,0,&s->destination,&s->destination_length,&failure);
    if(s->socket_fd==UB_INVALID) { if(error) *error=failure; ub_sender_close(s); return NULL; }
    return s;
}
int ub_sender_send(ub_sender *s,const char *json,size_t length) {
    uint8_t datagram[UB_DATAGRAM_CAP]; unsigned index,count;
    if(!s) return 0;
    ++s->sequence;
    if(!json || !length || length>UB_JSON_CAP) { report_error(&s->report,EINVAL); ++s->report.drops; return 0; }
    count=(unsigned)((length+UB_PAYLOAD_CAP-1u)/UB_PAYLOAD_CAP);
    for(index=0;index<count;++index) {
        size_t size=ub_fragment(s->nonce,s->sequence,json,length,index,datagram,sizeof(datagram));
        int sent=(int)sendto(s->socket_fd,(const char *)datagram,(int)size,0,
                            (const struct sockaddr *)&s->destination,s->destination_length);
        if(sent!=(int)size) {
            report_error(&s->report,sent<0 ? socket_error() : EIO); ++s->report.drops; return 0;
        }
        ++s->report.datagrams; s->report.bytes+=size;
    }
    ++s->report.messages; return 1;
}
ub_receiver *ub_receiver_open(const char *host,unsigned port,ub_message_fn callback,void *context,unsigned *error) {
    ub_receiver *r=ub_receiver_create(callback,context);
    unsigned failure=0; struct sockaddr_storage address; int length=0;
    if(error) *error=0;
    if(!r) { if(error) *error=callback ? ENOMEM : EINVAL; return NULL; }
    if((r->sockets_ready=start_sockets(&failure))!=0)
        r->socket_fd=open_socket(host,port,1,&address,&length,&failure);
    if(r->socket_fd==UB_INVALID) { if(error) *error=failure; ub_receiver_close(r); return NULL; }
    return r;
}
void ub_receiver_poll(ub_receiver *r,uint64_t now) {
    unsigned i;
    if(!r) return;
    ub_receiver_expire(r,now);
    if(r->socket_fd==UB_INVALID) return;
    for(i=0;i<UB_POLL_CAP;++i) {
        uint8_t datagram[UB_DATAGRAM_CAP+1u]; struct sockaddr_storage address; ub_peer peer;
#ifdef _WIN32
        int address_length=(int)sizeof(address);
#else
        socklen_t address_length=(socklen_t)sizeof(address);
#endif
        int received=(int)recvfrom(r->socket_fd,(char *)datagram,(int)sizeof(datagram),0,
                                  (struct sockaddr *)&address,&address_length);
        if(received<0) {
            unsigned error=socket_error();
            if(would_block(error)) break;
            report_error(&r->report,error); continue;
        }
        memset(&peer,0,sizeof(peer));
        if(address.ss_family==AF_INET) {
            const struct sockaddr_in *a=(const struct sockaddr_in *)&address;
            peer.family=4; peer.port=ntohs(a->sin_port); memcpy(peer.address,&a->sin_addr,4);
        } else if(address.ss_family==AF_INET6) {
            const struct sockaddr_in6 *a=(const struct sockaddr_in6 *)&address;
            peer.family=6; peer.port=ntohs(a->sin6_port); peer.scope_id=a->sin6_scope_id;
            memcpy(peer.address,&a->sin6_addr,16);
        }
        ub_receiver_feed(r,&peer,datagram,(size_t)received,now);
    }
}
void ub_sender_get_report(const ub_sender *s,ub_report *out) {
    if(out) { if(s) *out=s->report; else memset(out,0,sizeof(*out)); }
}
void ub_receiver_get_report(const ub_receiver *r,ub_report *out) {
    if(out) { if(r) *out=r->report; else memset(out,0,sizeof(*out)); }
}
void ub_sender_close(ub_sender *s) {
    if(!s) return;
    if(s->socket_fd!=UB_INVALID) (void)ub_close(s->socket_fd);
    stop_sockets(s->sockets_ready); free(s);
}
void ub_receiver_close(ub_receiver *r) {
    unsigned i;
    if(!r) return;
    for(i=0;i<UB_ASSEMBLY_CAP;++i) free(r->slot[i].body);
    if(r->socket_fd!=UB_INVALID) (void)ub_close(r->socket_fd);
    stop_sockets(r->sockets_ready); free(r);
}
