/* patch_bytecode.c -- inject a getCurrentLocale override into Angry Birds'
 * gamelogic.lua bytecode (Lua 5.1, 32-bit int/size_t/instr, 4-byte number),
 * forcing res.useLocale(<locale>). Reproduces the Python reference exactly.
 *
 * Only the top-level (main) function is modified: append one proto (the override
 * function body), append the 'getCurrentLocale' constant, inject CLOSURE +
 * SETGLOBAL before the main chunk's final RETURN, and bump maxstacksize. Nested
 * protos are copied verbatim (we only need to skip past them to find the append
 * point).
 */
#include <string.h>
#include <stdint.h>
#include <stddef.h>

/* ---- little-endian cursor over the input ---- */
typedef struct { const uint8_t *p, *end; } RD;
static uint32_t r_u32(RD *r){ uint32_t v=r->p[0]|(r->p[1]<<8)|(r->p[2]<<16)|((uint32_t)r->p[3]<<24); r->p+=4; return v; }
static uint8_t  r_u8 (RD *r){ return *r->p++; }
static void     r_skip(RD *r, size_t n){ r->p+=n; }

/* ---- output buffer ---- */
typedef struct { uint8_t *p; uint8_t *base; } WR;
static void w_u32(WR *w, uint32_t v){ w->p[0]=v; w->p[1]=v>>8; w->p[2]=v>>16; w->p[3]=v>>24; w->p+=4; }
static void w_u8 (WR *w, uint8_t v){ *w->p++=v; }
static void w_mem(WR *w, const void *s, size_t n){ memcpy(w->p,s,n); w->p+=n; }

/* header sizes for this build */
#define SZ_INT 4
#define SZ_ST  4
#define SZ_INS 4

/* skip a length-prefixed string (size_t len incl. trailing NUL; 0 => none) */
static void skip_string(RD *r){ uint32_t n=r_u32(r); r_skip(r,n); }

/* recursively skip a function prototype to advance past it */
static void skip_function(RD *r){
  skip_string(r);            /* source */
  r_skip(r, SZ_INT*2);       /* line, lastline */
  r_skip(r, 4);              /* nups,npar,vararg,maxstack */
  uint32_t ncode=r_u32(r); r_skip(r, (size_t)ncode*SZ_INS);
  uint32_t nk=r_u32(r);
  for(uint32_t i=0;i<nk;i++){
    uint8_t t=r_u8(r);
    if(t==1) r_skip(r,1);            /* bool  */
    else if(t==3) r_skip(r,4);       /* number (4-byte) */
    else if(t==4) skip_string(r);    /* string */
    /* t==0 nil: nothing */
  }
  uint32_t np=r_u32(r);
  for(uint32_t i=0;i<np;i++) skip_function(r);
  uint32_t nli=r_u32(r); r_skip(r, (size_t)nli*SZ_INT);   /* lineinfo */
  uint32_t nlo=r_u32(r);
  for(uint32_t i=0;i<nlo;i++){ skip_string(r); r_skip(r, SZ_INT*2); }
  uint32_t nup=r_u32(r);
  for(uint32_t i=0;i<nup;i++) skip_string(r);
}

/* Lua 5.1 instruction encoders */
static uint32_t iABC(int op,int A,int B,int C){ return (uint32_t)op|(A<<6)|(C<<14)|((uint32_t)B<<23); }
static uint32_t iABx(int op,int A,int Bx){ return (uint32_t)op|(A<<6)|((uint32_t)Bx<<14); }
enum { OP_LOADK=1, OP_GETGLOBAL=5, OP_GETTABLE=6, OP_SETGLOBAL=7, OP_CALL=28, OP_RETURN=30, OP_CLOSURE=36 };

/* write a string constant (type 4): u8(4) + u32(len+1) + bytes + NUL */
static void w_strconst(WR *w, const char *s){
  uint32_t n=(uint32_t)strlen(s)+1;
  w_u8(w,4); w_u32(w,n); w_mem(w,s,n-1); w_u8(w,0);
}

/* Build the override proto: function() _G.res.useLocale(LOCALE); return LOCALE end
 * consts: 0=_G 1=res 2=useLocale 3=LOCALE  */
static void write_override_proto(WR *w, const char *locale){
  w_u32(w,0);                       /* source: empty */
  w_u32(w,0); w_u32(w,0);           /* line,lastline */
  w_u8(w,0); w_u8(w,0); w_u8(w,0);  /* nups,npar,vararg */
  w_u8(w,2);                        /* maxstack */
  /* code (7) */
  w_u32(w,7);
  w_u32(w, iABx(OP_GETGLOBAL,0,0));
  w_u32(w, iABC(OP_GETTABLE,0,0,256+1));
  w_u32(w, iABC(OP_GETTABLE,0,0,256+2));
  w_u32(w, iABx(OP_LOADK,1,3));
  w_u32(w, iABC(OP_CALL,0,2,1));
  w_u32(w, iABx(OP_LOADK,0,3));
  w_u32(w, iABC(OP_RETURN,0,2,0));
  /* consts (4) */
  w_u32(w,4);
  w_strconst(w,"_G"); w_strconst(w,"res"); w_strconst(w,"useLocale"); w_strconst(w,locale);
  /* protos (0) */
  w_u32(w,0);
  /* lineinfo (7 zeros) */
  w_u32(w,7); for(int i=0;i<7;i++) w_u32(w,0);
  /* locals (0), upvals (0) */
  w_u32(w,0); w_u32(w,0);
}

/* Patch the whole chunk. Returns output length, or 0 on failure. */
size_t patch_gamelogic_bytecode(const uint8_t *in, size_t in_len,
                                const char *locale, uint8_t *out, size_t out_cap){
  if(in_len<12 || memcmp(in,"\x1bLua",4)!=0) return 0;
  RD r={in+12, in+in_len};
  WR w={out+12, out};
  memcpy(out,in,12);                    /* copy header */

  /* --- main function fixed prefix --- */
  const uint8_t *src_start=r.p;
  skip_string(&r);                      /* source */
  r_skip(&r, SZ_INT*2);                 /* line,lastline */
  uint8_t nups=r_u8(&r), npar=r_u8(&r), vararg=r_u8(&r), maxstack=r_u8(&r);
  w_mem(&w, src_start, (size_t)(r.p - src_start) - 1);  /* copy up to (not incl) maxstack */
  uint8_t newmax = maxstack;            /* reg used = old maxstack; need +1 */
  int R = maxstack;
  if(newmax < R+1) newmax = R+1;
  w_u8(&w, newmax);

  /* --- code: inject CLOSURE R,pidx ; SETGLOBAL R,i_gcl before final RETURN --- */
  uint32_t ncode=r_u32(&r);
  const uint8_t *code=r.p; r_skip(&r,(size_t)ncode*SZ_INS);
  /* pidx and i_gcl computed below need proto/const counts; but we can compute
     const count now by peeking is hard -- instead we defer writing code count.
     We know pidx = (proto count) and i_gcl = (const count) or existing index.
     Parse consts first (they come before protos), then we can know i_gcl and
     proto count. So buffer: we must write code AFTER knowing i_gcl and pidx.
     Approach: scan consts + protos region first to compute indices, then emit. */
  /* scan constants */
  RD rc={r.p, r.end};
  uint32_t nk=r_u32(&rc);
  int i_gcl=-1;
  for(uint32_t i=0;i<nk;i++){
    uint8_t t=r_u8(&rc);
    if(t==1) r_skip(&rc,1);
    else if(t==3) r_skip(&rc,4);
    else if(t==4){ uint32_t n=r_u32(&rc);
      if(i_gcl<0 && n==17 && memcmp(rc.p,"getCurrentLocale",16)==0) i_gcl=(int)i;
      r_skip(&rc,n);
    }
  }
  int appended_k = (i_gcl<0);
  if(i_gcl<0) i_gcl=(int)nk;            /* will append */
  const uint8_t *consts_end=rc.p;
  uint32_t nproto=r_u32(&rc);
  int pidx=(int)nproto;

  /* now emit code with injection */
  w_u32(&w, ncode+2);
  w_mem(&w, code, (size_t)(ncode-1)*SZ_INS);     /* all but final RETURN */
  w_u32(&w, iABx(OP_CLOSURE,R,pidx));
  w_u32(&w, iABC(OP_SETGLOBAL,R,0,i_gcl));
  w_mem(&w, code+(size_t)(ncode-1)*SZ_INS, SZ_INS); /* original RETURN */

  /* emit consts (append getCurrentLocale if needed) */
  w_u32(&w, nk + (appended_k?1:0));
  w_mem(&w, r.p+4, (size_t)(consts_end - (r.p+4)));   /* existing consts bytes */
  if(appended_k) w_strconst(&w,"getCurrentLocale");

  /* emit protos: existing + our override */
  w_u32(&w, nproto+1);
  const uint8_t *protos_start=consts_end+4;
  RD rp={protos_start, r.end};
  for(uint32_t i=0;i<nproto;i++) skip_function(&rp);
  const uint8_t *protos_end=rp.p;
  w_mem(&w, protos_start, (size_t)(protos_end-protos_start)); /* existing protos verbatim */
  write_override_proto(&w, locale);                          /* our new proto */

  /* copy the rest (main-function debug: lineinfo, locals, upvals) verbatim */
  w_mem(&w, protos_end, (size_t)(r.end - protos_end));

  (void)nups;(void)npar;(void)vararg;
  return (size_t)(w.p - w.base);
}
