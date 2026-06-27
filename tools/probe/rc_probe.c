/* rc_probe.c — standalone adaptive range-coder harness for the cell plane.
 *
 * Reads a raw cell-plane dump (TVID_CELLDUMP output) and reports the compressed
 * size under several adaptive context models, vs our shipped coder and xz. NO
 * codec changes — this answers "can context+arithmetic beat our static Huffman
 * and reach xz?" before any format work, per the realprobe.c discipline.
 *
 * Models:
 *   order0  — single 256-symbol adaptive model (sanity floor)
 *   order1  — 256 contexts keyed on the previous byte
 *   o1mix   — order1 with a small fallback so unseen contexts don't stall
 *
 * Range coder: Subbotin-style 32-bit carryless range coder, byte renormalized.
 * Frequencies are adaptive (encoder/decoder update in lockstep -> no table to
 * serialize, which is exactly the cost that killed static order-1 Huffman).
 *
 * Build:  cc -O2 -o /tmp/rc_probe /tmp/rc_probe.c
 * Run:    /tmp/rc_probe /tmp/cell_bif.bin
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

/* ---- range coder (encode-only; we only need the size, and we verify by also
 *      decoding to prove the model is real, not a lower bound) ---- */
typedef struct {
    uint32_t low, range;
    uint8_t *out; long pos, cap;
} RC;
static void rc_init(RC *r, uint8_t *out, long cap){ r->low=0; r->range=0xFFFFFFFFu; r->out=out; r->pos=0; r->cap=cap; }
static void rc_emit(RC *r, uint8_t b){ if(r->pos<r->cap) r->out[r->pos]=b; r->pos++; }
static void rc_encode(RC *r, uint32_t cum, uint32_t freq, uint32_t tot){
    r->range /= tot;
    r->low  += cum * r->range;
    r->range *= freq;
    while ((r->low ^ (r->low + r->range)) < (1u<<24) ||
           (r->range < (1u<<16) && ((r->range = -r->low & ((1u<<16)-1)), 1))) {
        rc_emit(r, (uint8_t)(r->low>>24)); r->low<<=8; r->range<<=8;
    }
}
static void rc_flush(RC *r){ for(int i=0;i<4;i++){ rc_emit(r,(uint8_t)(r->low>>24)); r->low<<=8; } }

/* ---- adaptive frequency model: 256 symbols, periodic rescale ---- */
typedef struct { uint16_t f[256]; uint32_t tot; } Model;
static void m_init(Model *m){ for(int i=0;i<256;i++) m->f[i]=1; m->tot=256; }
static void m_encode(RC *r, Model *m, int sym){
    uint32_t cum=0; for(int i=0;i<sym;i++) cum+=m->f[i];
    rc_encode(r, cum, m->f[sym], m->tot);
    m->f[sym]+=32; m->tot+=32;
    if(m->tot >= (1u<<16)){ uint32_t t=0; for(int i=0;i<256;i++){ m->f[i]=(uint16_t)((m->f[i]>>1)|1); t+=m->f[i]; } m->tot=t; }
}

static long sz_order0(const uint8_t *d, long n){
    uint8_t *out=malloc(n+1024); RC r; rc_init(&r,out,n+1024);
    Model m; m_init(&m);
    for(long i=0;i<n;i++) m_encode(&r,&m,d[i]);
    rc_flush(&r); long s=r.pos; free(out); return s;
}
static long sz_order1(const uint8_t *d, long n){
    uint8_t *out=malloc(n+1024); RC r; rc_init(&r,out,n+1024);
    Model *m=malloc(sizeof(Model)*256); for(int i=0;i<256;i++) m_init(&m[i]);
    int ctx=0;
    for(long i=0;i<n;i++){ m_encode(&r,&m[ctx],d[i]); ctx=d[i]; }
    rc_flush(&r); long s=r.pos; free(out); free(m); return s;
}
/* order-1 keyed on the high nibble only (the [luma|glyph] or [color|glyph]
 * split byte: high bits carry the more predictive field). 16 contexts -> far
 * more samples each, less adaptation lag than 256 contexts on a small stream. */
static long sz_o1hi(const uint8_t *d, long n){
    uint8_t *out=malloc(n+1024); RC r; rc_init(&r,out,n+1024);
    Model *m=malloc(sizeof(Model)*16); for(int i=0;i<16;i++) m_init(&m[i]);
    int ctx=0;
    for(long i=0;i<n;i++){ m_encode(&r,&m[ctx],d[i]); ctx=d[i]>>4; }
    rc_flush(&r); long s=r.pos; free(out); free(m); return s;
}

/* ---- tunable model: adaptation increment + rescale cap as parameters, so we
 *      can find the precision/lag sweet spot for this stream ---- */
typedef struct { uint16_t f[256]; uint32_t tot; } TModel;
static void tm_init(TModel *m){ for(int i=0;i<256;i++) m->f[i]=1; m->tot=256; }
static void tm_encode(RC *r, TModel *m, int sym, int inc, uint32_t cap){
    uint32_t cum=0; for(int i=0;i<sym;i++) cum+=m->f[i];
    rc_encode(r, cum, m->f[sym], m->tot);
    m->f[sym]+=inc; m->tot+=inc;
    if(m->tot>=cap){ uint32_t t=0; for(int i=0;i<256;i++){ m->f[i]=(uint16_t)((m->f[i]>>1)|1); t+=m->f[i]; } m->tot=t; }
}
/* order-1, parameterized */
static long sz_o1_tuned(const uint8_t *d, long n, int inc, uint32_t cap){
    uint8_t *out=malloc(n+1024); RC r; rc_init(&r,out,n+1024);
    TModel *m=malloc(sizeof(TModel)*256); for(int i=0;i<256;i++) tm_init(&m[i]);
    int ctx=0;
    for(long i=0;i<n;i++){ tm_encode(&r,&m[ctx],d[i],inc,cap); ctx=d[i]; }
    rc_flush(&r); long s=r.pos; free(out); free(m); return s;
}
/* order-2: prev two bytes -> 65536 contexts. Lazy-alloc to keep memory sane and
 * to test whether sparsity (only ~12 K of 64 K contexts seen) defeats it. */
static long sz_order2(const uint8_t *d, long n, int inc, uint32_t cap){
    uint8_t *out=malloc(n+1024); RC r; rc_init(&r,out,n+1024);
    TModel *m=calloc(65536,sizeof(TModel)); /* 0-init; init lazily on first use */
    uint8_t *seen=calloc(65536,1);
    int ctx=0;
    for(long i=0;i<n;i++){
        if(!seen[ctx]){ tm_init(&m[ctx]); seen[ctx]=1; }
        tm_encode(&r,&m[ctx],d[i],inc,cap);
        ctx=((ctx<<8)|d[i])&0xFFFF;
    }
    rc_flush(&r); long s=r.pos; free(out); free(m); free(seen); return s;
}
/* order-2 with a reduced context: prev byte (8b) + prev-prev high nibble (4b)
 * = 4096 contexts. Adds some second-order info without 64 K-context sparsity. */
static long sz_o2red(const uint8_t *d, long n, int inc, uint32_t cap){
    uint8_t *out=malloc(n+1024); RC r; rc_init(&r,out,n+1024);
    TModel *m=malloc(sizeof(TModel)*4096); for(int i=0;i<4096;i++) tm_init(&m[i]);
    int p1=0,p2=0;
    for(long i=0;i<n;i++){ int ctx=(p1<<4)|(p2>>4); tm_encode(&r,&m[ctx&4095],d[i],inc,cap); p2=p1; p1=d[i]; }
    rc_flush(&r); long s=r.pos; free(out); free(m); return s;
}

/* ---- decoder side: prove the order-1 model is a real codec, not a lower bound.
 *      Mirror of rc_encode / tm_encode with a range *decoder*. ---- */
typedef struct { const uint8_t *in; long pos, len; uint32_t low, range, code; } RD;
static uint8_t rd_get(RD *d){ return d->pos<d->len ? d->in[d->pos++] : 0; }
static void rd_init(RD *d, const uint8_t *in, long len){
    d->in=in; d->pos=0; d->len=len; d->low=0; d->range=0xFFFFFFFFu; d->code=0;
    for(int i=0;i<4;i++) d->code=(d->code<<8)|rd_get(d);
}
static uint32_t rd_getfreq(RD *d, uint32_t tot){
    d->range/=tot; return (d->code - d->low)/d->range;
}
static void rd_decode(RD *d, uint32_t cum, uint32_t freq){
    d->low += cum*d->range; d->range*=freq;
    while ((d->low ^ (d->low + d->range)) < (1u<<24) ||
           (d->range < (1u<<16) && ((d->range = -d->low & ((1u<<16)-1)), 1))) {
        d->code=(d->code<<8)|rd_get(d); d->low<<=8; d->range<<=8;
    }
}
static int tm_decode(RD *d, TModel *m, int inc, uint32_t cap){
    uint32_t f=rd_getfreq(d,m->tot), cum=0; int sym=0;
    while(cum+m->f[sym]<=f){ cum+=m->f[sym]; sym++; }
    rd_decode(d,cum,m->f[sym]);
    m->f[sym]+=inc; m->tot+=inc;
    if(m->tot>=cap){ uint32_t t=0; for(int i=0;i<256;i++){ m->f[i]=(uint16_t)((m->f[i]>>1)|1); t+=m->f[i]; } m->tot=t; }
    return sym;
}
/* encode then decode order-1, return 0 if byte-exact roundtrip, else 1. */
static int roundtrip_o1(const uint8_t *d, long n, int inc, uint32_t cap, long *enc_size){
    uint8_t *out=malloc(n+1024); RC r; rc_init(&r,out,n+1024);
    TModel *m=malloc(sizeof(TModel)*256); for(int i=0;i<256;i++) tm_init(&m[i]);
    int ctx=0;
    for(long i=0;i<n;i++){ tm_encode(&r,&m[ctx],d[i],inc,cap); ctx=d[i]; }
    rc_flush(&r); *enc_size=r.pos;
    /* decode */
    RD dd; rd_init(&dd,out,r.pos);
    TModel *m2=malloc(sizeof(TModel)*256); for(int i=0;i<256;i++) tm_init(&m2[i]);
    int ctx2=0, bad=0;
    for(long i=0;i<n;i++){ int s=tm_decode(&dd,&m2[ctx2],inc,cap); if((uint8_t)s!=d[i]){ bad=1; break; } ctx2=s; }
    free(out); free(m); free(m2); return bad;
}

int main(int argc, char **argv){
    if(argc<2){ fprintf(stderr,"usage: %s cellplane.bin\n",argv[0]); return 1; }
    FILE *f=fopen(argv[1],"rb"); if(!f){ perror("open"); return 1; }
    fseek(f,0,SEEK_END); long n=ftell(f); fseek(f,0,SEEK_SET);
    uint8_t *d=malloc(n); if(fread(d,1,n,f)!=(size_t)n){ return 1; } fclose(f);
    printf("plane=%ld bytes\n", n);
    printf("  order0          = %ld\n", sz_order0(d,n));
    printf("  order1(256)     = %ld\n", sz_order1(d,n));
    printf("  order1(hi4)     = %ld\n", sz_o1hi(d,n));
    printf("  -- order1 adaptation tuning (inc, cap) --\n");
    int incs[]={8,16,24,32,48,64};
    uint32_t caps[]={1u<<14,1u<<15,1u<<16};
    long best=1e18; int bi=0; uint32_t bc=0;
    for(unsigned ci=0;ci<sizeof(caps)/sizeof(caps[0]);ci++)
      for(unsigned ii=0;ii<sizeof(incs)/sizeof(incs[0]);ii++){
        long s=sz_o1_tuned(d,n,incs[ii],caps[ci]);
        printf("    o1 inc=%-3d cap=%-6u = %ld\n", incs[ii], caps[ci], s);
        if(s<best){ best=s; bi=incs[ii]; bc=caps[ci]; }
      }
    printf("  best order1     = %ld (inc=%d cap=%u)\n", best, bi, bc);
    printf("  order2(64K) inc=%d   = %ld\n", bi, sz_order2(d,n,bi,bc));
    printf("  order2-reduced(4K)  = %ld\n", sz_o2red(d,n,bi,bc));
    long es=0; int bad=roundtrip_o1(d,n,bi,bc,&es);
    printf("  ROUNDTRIP order1 inc=%d cap=%u: enc=%ld  %s\n",
           bi, bc, es, bad ? "*** DECODE MISMATCH ***" : "byte-exact OK");
    free(d); return bad;
}
