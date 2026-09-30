/* SPDX-License-Identifier: MIT
 * Execute production pending-export ownership with real POSIX descriptors. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <threads.h>
#include <errno.h>
#include <string.h>

enum virgl_resource_fd_type { VIRGL_RESOURCE_FD_DMABUF=1 };
// PRODUCTION_PENDING_TYPES
struct hash_entry { const uint64_t *key; void *data; struct hash_entry *next; };
struct hash_table { struct hash_entry *first; bool fail_insert; };
struct npt_context { mtx_t pending_blob_mutex; struct hash_table *pending_blob_table; };
static unsigned blocks;
static bool fail_calloc;
static void *test_calloc(size_t count,size_t size)
{
   if(fail_calloc)return NULL;
   void *p=calloc(count,size);if(p)++blocks;return p;
}
static void test_free(void *p) { if(p){assert(blocks);--blocks;}free(p); }
static struct hash_entry *_mesa_hash_table_search(struct hash_table *h,const uint64_t *key)
{
   for(struct hash_entry *p=h->first;p;p=p->next)if(*p->key==*key)return p;
   return NULL;
}
static struct hash_entry *_mesa_hash_table_insert(struct hash_table *h,const uint64_t *key,void *data)
{
   if(h->fail_insert)return NULL;
   struct hash_entry *p=_mesa_hash_table_search(h,key);
   if(p){p->key=key;p->data=data;return p;}
   p=calloc(1,sizeof(*p));assert(p);p->key=key;p->data=data;p->next=h->first;h->first=p;return p;
}
static void _mesa_hash_table_remove(struct hash_table *h,struct hash_entry *entry)
{
   struct hash_entry **p=&h->first;while(*p!=entry){assert(*p);p=&(*p)->next;}
   *p=entry->next;free(entry);
}
#define calloc test_calloc
#define free test_free
// PRODUCTION_PENDING_FUNCTIONS
#undef calloc
#undef free
static bool valid(int fd) { return fcntl(fd,F_GETFD)>=0; }
static int export_fd(void) { int fd=open("/dev/null",O_RDONLY);assert(fd>=0);return fd; }
static bool stage(struct npt_context *ctx,uint64_t id,int fd)
{
   return npt_context_register_pending_blob(ctx,id,VIRGL_RESOURCE_FD_DMABUF,fd,4096,42,NULL);
}
static int consume(struct npt_context *ctx,uint64_t id)
{
   mtx_lock(&ctx->pending_blob_mutex);
   struct hash_entry *entry=_mesa_hash_table_search(ctx->pending_blob_table,&id);
   assert(entry);struct npt_pending_blob *pb=entry->data;int fd=pb->fd;
   _mesa_hash_table_remove(ctx->pending_blob_table,entry);
   mtx_unlock(&ctx->pending_blob_mutex);test_free(pb);return fd;
}
int main(void)
{
   struct hash_table table={0};struct npt_context ctx={.pending_blob_table=&table};
   assert(mtx_init(&ctx.pending_blob_mutex,mtx_plain)==thrd_success);
   int first=export_fd();assert(stage(&ctx,7,first) && valid(first) && blocks==1);
   int duplicate=export_fd();assert(!stage(&ctx,7,duplicate));
   assert(valid(first) && valid(duplicate) && blocks==1);close(duplicate);
   npt_context_cancel_pending_blob(&ctx,7);
   assert(!valid(first) && errno==EBADF && !blocks && !table.first);
   npt_context_cancel_pending_blob(&ctx,7);assert(!blocks);
   first=export_fd();assert(stage(&ctx,7,first));assert(consume(&ctx,7)==first);
   npt_context_cancel_pending_blob(&ctx,7);assert(valid(first));close(first);
   first=export_fd();fail_calloc=true;assert(!stage(&ctx,1,first));fail_calloc=false;
   assert(valid(first) && !blocks && !table.first);close(first);
   first=export_fd();table.fail_insert=true;assert(!stage(&ctx,1,first));table.fail_insert=false;
   assert(valid(first) && !blocks && !table.first);close(first);
   struct virgl_attachment_layout layout = {
      .width=17, .height=9, .fourcc=0x34325241, .plane_count=1,
      .modifier=UINT64_C(0x0300000000606015), .strides={256}, .offsets={128},
   };
   first=export_fd();
   assert(npt_context_register_pending_blob(&ctx,55,VIRGL_RESOURCE_FD_DMABUF,
                                            first,4096,42,&layout));
   const uint64_t image_id=55;
   struct npt_pending_blob *image=_mesa_hash_table_search(&table,&image_id)->data;
   assert(image->layout.width==17 && image->layout.height==9);
   assert(image->layout.fourcc==0x34325241 && image->layout.plane_count==1);
   assert(image->layout.modifier==UINT64_C(0x0300000000606015));
   assert(image->layout.strides[0]==256 && image->layout.offsets[0]==128);
   memset(&layout,0,sizeof(layout));
   assert(image->layout.modifier==UINT64_C(0x0300000000606015));
   npt_context_cancel_pending_blob(&ctx,55);assert(!valid(first) && !blocks);
   first=export_fd();assert(stage(&ctx,55,first));
   image=_mesa_hash_table_search(&table,&image_id)->data;
   assert(!memcmp(&image->layout,&layout,sizeof(layout)));
   npt_context_cancel_pending_blob(&ctx,55);assert(!valid(first) && !blocks);
   for(unsigned i=0;i<64;++i)assert(stage(&ctx,i+100,export_fd()));
   assert(blocks==64);
   for(unsigned i=0;i<64;++i)npt_context_cancel_pending_blob(&ctx,i+100);
   assert(!blocks && !table.first);
   mtx_destroy(&ctx.pending_blob_mutex);
   puts("Pending export fd ownership passed");
}
