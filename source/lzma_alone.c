#include <string.h>
#include "LzmaDec.h"
#include "LzmaEnc.h"
#include "Alloc.h"

/* devkitPro's newlib declares posix_memalign() but doesn't provide it; the LZMA
 * SDK's aligned allocator references it. Implement it via memalign(). */
#include <malloc.h>
#include <errno.h>
int posix_memalign(void **memptr, size_t alignment, size_t size) {
  void *p = memalign(alignment, size);
  if (!p) return ENOMEM;
  *memptr = p;
  return 0;
}

static const unsigned char MAGIC[9]={0x89,'L','Z','M','A','\r','\n',0x1a,'\n'};
// Decode game "alone" stream (starts with 9-byte magic). Returns out length, or 0.
size_t lzma_alone_decode(const unsigned char*in,size_t in_len,unsigned char*out,size_t out_cap){
  if(in_len<22||memcmp(in,MAGIC,9)!=0) return 0;
  const unsigned char*props=in+9;            // 5 bytes: propsByte + dictSize
  unsigned long long usize=0; for(int i=0;i<8;i++) usize|=(unsigned long long)in[14+i]<<(8*i);
  if(usize>out_cap) return 0;
  const unsigned char*data=in+22; SizeT srcLen=in_len-22, destLen=usize;
  ELzmaStatus st;
  SRes r=LzmaDecode(out,&destLen,data,&srcLen,props,5,LZMA_FINISH_ANY,&st,&g_Alloc);
  if(r!=0) return 0;
  return destLen;
}
// Encode to game "alone" format (magic + props + size + data w/ end mark).
size_t lzma_alone_encode(const unsigned char*in,size_t in_len,unsigned char*out,size_t out_cap){
  CLzmaEncProps p; LzmaEncProps_Init(&p);
  p.level=6; p.dictSize=1<<16; p.lc=3; p.lp=0; p.pb=2; p.writeEndMark=1;
  Byte propsEnc[5]; SizeT propsSize=5;
  if(out_cap<22) return 0;
  SizeT destLen=out_cap-22;
  SRes r=LzmaEncode(out+22,&destLen,in,in_len,&p,propsEnc,&propsSize,1,NULL,&g_Alloc,&g_Alloc);
  if(r!=0||propsSize!=5) return 0;
  memcpy(out,MAGIC,9);
  memcpy(out+9,propsEnc,5);
  for(int i=0;i<8;i++) out[14+i]=(unsigned char)((unsigned long long)in_len>>(8*i));
  return 22+destLen;
}
