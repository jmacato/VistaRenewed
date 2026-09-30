/* SPDX-License-Identifier: MIT
 * Error/ordering contracts, not a real socket or GPU test. */
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef int32_t HRESULT;
#define NPT_E_INVALIDARG (-1)
#define NPT_E_NOTIMPL (-2)
#define NPT_E_FAIL (-3)
#define NPT_CMD_FLAG_REPLY 1
#define NPT_TRANSPORT_SUBGROUP_RESOURCE 3
#define NPT_TRANSPORT_RESOURCE_COPY_COLOR 4
#define NPT_TRANSPORT_CMD_TYPE(s,m) (((s)<<16)|(m))
#define NPT_OBJECT_TYPE_ID3D11DEVICECONTEXT 1
#define NPT_OBJECT_TYPE_ID3D11RENDERTARGETVIEW 2
#define NPT_OBJECT_TYPE_ID3D11SHADERRESOURCEVIEW 3
#define NPT_BACKEND_DXVK 1
// WIRE
_Static_assert(sizeof(struct npt_cmd_resource_copy_color)==48,"wire ABI");
static unsigned checks, backend_calls, submits, frees;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr,"FAIL transport %u: %s\n",__LINE__,#x); exit(1); } } while (0)
struct npt_cs_decoder { uint8_t *cur,*end; bool fatal; };
struct npt_cs_encoder { uint8_t *cur,*end; };
struct npt_context { struct npt_cs_decoder decoder; } host;
struct npt_dispatch_context { struct npt_cs_decoder *decoder; };
struct npt_ring { unsigned tag; } dc_ring, tls_ring;
struct npt_device { unsigned tag; } device, foreign;
struct wrapper { uint64_t id; struct npt_device *dev; struct npt_ring *ring; } context, rtv, srv;
static struct npt_ring *npt_com_self_ring(void*p){return ((struct wrapper*)p)->ring;}
static struct npt_device *npt_com_self_device(void*p){return ((struct wrapper*)p)->dev;}
static uint64_t npt_com_self_id(void*p){return ((struct wrapper*)p)->id;}
static bool npt_dispatch_is_ring_dispatch(struct npt_context*c,struct npt_dispatch_context*d){return d->decoder!=&c->decoder;}
static void npt_cs_decoder_set_fatal(struct npt_cs_decoder*d){d->fatal=true;}
static bool npt_cs_decoder_get_fatal(struct npt_cs_decoder*d){return d->fatal;}
static void npt_cs_decoder_read(struct npt_cs_decoder*d,size_t size,void*out,size_t cap){
 if(size>cap || size>(size_t)(d->end-d->cur)){d->fatal=true;return;}memcpy(out,d->cur,size);d->cur+=size;
}
static bool encoder_fail;
static bool npt_cs_encoder_acquire(struct npt_cs_encoder*e){return !encoder_fail;}
static void npt_cs_encoder_release(struct npt_cs_encoder*e){(void)e;}
static void npt_cs_encoder_write(struct npt_cs_encoder*e,size_t size,const void*data,size_t cap){CHECK(size==cap&&size<=(size_t)(e->end-e->cur));memcpy(e->cur,data,size);e->cur+=size;}
static unsigned missing_type;
static void *npt_context_lookup_object(struct npt_context*c,void*d,uint64_t id,unsigned type){
 if(type==missing_type)return NULL;
 if(id==101&&type==1)return &context;
 if(id==102&&type==2)return &rtv;
 if(id==103&&type==3)return &srv;
 return NULL;
}
static HRESULT backend_status;
static HRESULT backend(void*c,void*d,void*s){CHECK(c==&context&&d==&rtv&&s==&srv);++backend_calls;return backend_status;}
struct npt_d3d_library { unsigned backend; HRESULT(*pfn_copy_color)(void*,void*,void*); } lib;
static struct npt_d3d_library*npt_renderer_get_library(void){return &lib;}
// HOST
struct npt_ring_submit_command { void*cmd; size_t size; struct npt_cs_encoder enc; };
static uint8_t reply_bytes[64];
static struct npt_cs_decoder reply;
static bool init_fail, bad_reply, short_reply, absent_reply;
static struct npt_cs_encoder*npt_ring_submit_command_init(struct npt_ring*r,struct npt_ring_submit_command*s,void*cmd,size_t size,size_t response){
 CHECK(r==&dc_ring&&size==48&&response==sizeof(struct npt_cmd_resource_copy_color_reply));
 if(init_fail)return NULL;s->cmd=cmd;s->size=size;s->enc=(struct npt_cs_encoder){cmd,(uint8_t*)cmd+size};return &s->enc;
}
static void npt_ring_submit_command(struct npt_ring*r,struct npt_ring_submit_command*s){
 reply=(struct npt_cs_decoder){0};if(!s->cmd)return;++submits;CHECK(s->enc.cur==s->enc.end);
 struct npt_cmd_resource_copy_color*cmd=s->cmd;
 CHECK(cmd->header.cmd_type==NPT_TRANSPORT_CMD_TYPE(3,4)&&cmd->header.cmd_flags==1&&cmd->header.cmd_size==48);
 CHECK(cmd->context_id==101&&cmd->dst_rtv_id==102&&cmd->src_srv_id==103);
 struct npt_cs_decoder dec={(uint8_t*)s->cmd+sizeof(cmd->header),(uint8_t*)s->cmd+s->size,false};
 struct npt_cs_encoder enc={reply_bytes,reply_bytes+sizeof(reply_bytes)};
 struct npt_dispatch_context dispatch={&dec};host_copy_color(&host,&dispatch,&dec,&enc,&cmd->header);CHECK(!dec.fatal);
 reply.cur=reply_bytes;reply.end=enc.cur;
 if(bad_reply)((struct npt_reply_header*)reply_bytes)->cmd_type^=1;
 if(short_reply)reply.end=reply.cur;
}
static struct npt_cs_decoder*npt_ring_get_command_reply(struct npt_ring*r,struct npt_ring_submit_command*s){return init_fail||absent_reply?NULL:&reply;}
static void npt_ring_free_command_reply(struct npt_ring*r,struct npt_ring_submit_command*s){++frees;}
// GUEST
static void reset(void){
 context=(struct wrapper){101,&device,&dc_ring};rtv=(struct wrapper){102,&device,&tls_ring};srv=(struct wrapper){103,&device,&tls_ring};
 lib=(struct npt_d3d_library){NPT_BACKEND_DXVK,backend};backend_status=0;backend_calls=submits=frees=missing_type=0;
 encoder_fail=init_fail=bad_reply=short_reply=absent_reply=false;
}
static void host_invalid(unsigned which){
 reset();struct npt_cmd_resource_copy_color cmd={0};cmd.header.cmd_size=sizeof(cmd);cmd.context_id=101;cmd.dst_rtv_id=102;cmd.src_srv_id=103;
 struct npt_cs_decoder d={(uint8_t*)&cmd.context_id,(uint8_t*)&cmd+sizeof(cmd),false};struct npt_cs_encoder e={reply_bytes,reply_bytes+64};struct npt_dispatch_context dispatch={&d};
 if(which==0)cmd.header.cmd_size--;
 if(which==1)d.end--;
 if(which==2)dispatch.decoder=&host.decoder;
 if(which==3)cmd.src_srv_id=0;
 host_copy_color(&host,&dispatch,&d,&e,&cmd.header);CHECK(backend_calls==0);
 if(which<2)CHECK(d.fatal&&e.cur==reply_bytes);else CHECK(!d.fatal&&((struct npt_reply_header*)reply_bytes)->cmd_return==(uint32_t)NPT_E_INVALIDARG);
}
int main(void){
 reset();CHECK(npt_dispatch_resource_copy_color(&context,&rtv,&srv)==0);CHECK(backend_calls==1&&submits==1&&frees==1);
 reset();backend_status=-77;CHECK(npt_dispatch_resource_copy_color(&context,&rtv,&srv)==-77&&backend_calls==1);
 for(unsigned t=1;t<=3;++t){reset();missing_type=t;CHECK(npt_dispatch_resource_copy_color(&context,&rtv,&srv)==NPT_E_INVALIDARG&&backend_calls==0);}
 reset();lib.pfn_copy_color=NULL;CHECK(npt_dispatch_resource_copy_color(&context,&rtv,&srv)==NPT_E_NOTIMPL&&backend_calls==0);
 reset();lib.backend=0;CHECK(npt_dispatch_resource_copy_color(&context,&rtv,&srv)==NPT_E_NOTIMPL&&backend_calls==0);
 for(unsigned f=0;f<5;++f){reset();if(f==0)init_fail=true;if(f==1)bad_reply=true;if(f==2)short_reply=true;if(f==3)absent_reply=true;if(f==4)encoder_fail=true;CHECK(npt_dispatch_resource_copy_color(&context,&rtv,&srv)==NPT_E_FAIL);}
 reset();srv.dev=&foreign;CHECK(npt_dispatch_resource_copy_color(&context,&rtv,&srv)==NPT_E_INVALIDARG&&submits==0);
 reset();rtv.id=0;CHECK(npt_dispatch_resource_copy_color(&context,&rtv,&srv)==NPT_E_INVALIDARG&&submits==0);
 reset();context.ring=NULL;CHECK(npt_dispatch_resource_copy_color(&context,&rtv,&srv)==NPT_E_INVALIDARG&&submits==0);
 reset();CHECK(npt_dispatch_resource_copy_color(NULL,&rtv,&srv)==NPT_E_INVALIDARG&&submits==0);
 for(unsigned i=0;i<4;++i)host_invalid(i);
 printf("Color copy paired transport contracts %u checks passed\n",checks);return 0;
}
