/* Streaming JPEG-to-JPEG look application. Canon originals are opened read-only.
 * One 16-row band is retained, rather than a full 22MP RGB image. */
#include "fos.h"
#include "fos_bake.h"
#include "fos_jpeg.h"
#include "tjpgd.h"
#include "fos_preview.h"
static const char *error_text="";
static char detail[96];
static int read_ms,colour_ms,encode_ms,bench_ms;
/* Memory the colour loop works in. On the camera this prefers the ordinary cached heap;
 * the 2026-11-01 log showed about 25 us per pixel in this step, far above the arithmetic. */
__attribute__((weak)) void *fos_port_alloc_hot(int size){return fos_port_alloc(size);}
const char *fos_bake_error(void){return error_text;}
struct bake {
    void *input, *writer;
    uint8_t *strip, *lut;
    int width, height, scale, strip_y, rows, failed;
    int grain;                          /* peak grain amplitude in levels, 0 = off */
    struct glow *glow;                  /* halation, or 0 */
    struct fos_preview preview;
};

/* ---------------------------------------------------------------- grain and halation
 * Both are optional and off by default. Grain is mono noise strongest in the mid tones.
 * Halation is a warm glow around bright areas; it is measured from the review picture
 * the quick worker already made (so it follows the graded highlights) as a small blurred
 * map, then added to every pixel with bilinear sampling while the bake streams past. */
#define GLOW_MAX 96
struct glow {
    int gw, gh, strength, top;
    uint8_t *cell;                      /* gw x gh, 0..255 */
    uint16_t *gx;                       /* per output column: cell << 8 | fraction */
    int line[16][GLOW_MAX + 2];         /* the current strip's rows, blended vertically */
};
static const int grain_peak[4]={0,4,8,14};
static const int glow_strength[4]={0,80,150,220};   /* x 1/256 of the glow, which is added to red and, less, to green and blue */
static void glow_free(struct glow *g){if(g){fos_port_free(g->cell);fos_port_free(g->gx);fos_port_free(g);}}
static void box_blur(int *v,int *tmp,int n,int step,int count,int stride,int radius){
    /* blur `count` lines of `n` samples; sample i of line k is v[k*stride+i*step]; outside is 0 */
    for(int k=0;k<count;k++){
        int *p=v+k*stride;int sum=0;
        for(int i=0;i<=radius&&i<n;i++)sum+=p[i*step];
        for(int i=0;i<n;i++){
            tmp[i]=sum;
            int add=i+radius+1,sub=i-radius;
            if(add<n)sum+=p[add*step];
            if(sub>=0)sum-=p[sub*step];
        }
        for(int i=0;i<n;i++)p[i*step]=tmp[i]/(2*radius+1);
    }
}
static struct glow *glow_build(const struct fos_preview *pv,int level,int width){
    struct glow *g=fos_port_alloc_hot(sizeof(*g));if(!g)return 0;
    memset(g,0,sizeof(*g));g->top=-1;g->strength=glow_strength[level];
    int cs=pv->w/GLOW_MAX+1;if(cs<4)cs=4;
    g->gw=(pv->w+cs-1)/cs;g->gh=(pv->h+cs-1)/cs;
    int cells=g->gw*g->gh,lw=g->gw>g->gh?g->gw:g->gh;
    int *acc=fos_port_alloc_hot((cells+lw)*sizeof(int));
    g->cell=fos_port_alloc_hot(cells);g->gx=fos_port_alloc_hot(width*sizeof(uint16_t));
    if(!acc||!g->cell||!g->gx){fos_port_free(acc);glow_free(g);return 0;}
    memset(acc,0,cells*sizeof(int));
    /* each cell starts as the brightest highlight in it (so a small light counts as fully as a large one) */
    for(int y=0;y<pv->h;y++)for(int x=0;x<pv->w;x++){
        const uint8_t *c=pv->pal[pv->pixels[y*pv->w+x]-1];
        int l=(c[0]*77+c[1]*150+c[2]*29)>>8;
        if(l>160){int h=((l-160)*655)>>8;if(h>255)h=255;int *a=&acc[(y/cs)*g->gw+x/cs];if(h*256>*a)*a=h*256;}
    }
    /* then it falls away with distance: a chamfer sweep takes the strongest highlight in reach, less a fixed step per cell
     * (1.41 per diagonal step), so every light gets the same halo, round and fading to nothing */
    const int reach=g->gw/7<4?4:g->gw/7;
    const int step=255*256/reach,diag=step*362/256;
    const int gw=g->gw,gh=g->gh;
    for(int y=0;y<gh;y++)for(int x=0;x<gw;x++){
        int i=y*gw+x,v=acc[i],t;
        if(x>0&&(t=acc[i-1]-step)>v)v=t;
        if(y>0){
            if((t=acc[i-gw]-step)>v)v=t;
            if(x>0&&(t=acc[i-gw-1]-diag)>v)v=t;
            if(x<gw-1&&(t=acc[i-gw+1]-diag)>v)v=t;
        }
        acc[i]=v;
    }
    for(int y=gh-1;y>=0;y--)for(int x=gw-1;x>=0;x--){
        int i=y*gw+x,v=acc[i],t;
        if(x<gw-1&&(t=acc[i+1]-step)>v)v=t;
        if(y<gh-1){
            if((t=acc[i+gw]-step)>v)v=t;
            if(x<gw-1&&(t=acc[i+gw+1]-diag)>v)v=t;
            if(x>0&&(t=acc[i+gw-1]-diag)>v)v=t;
        }
        acc[i]=v;
    }
    box_blur(acc,acc+cells,gw,1,gh,gw,1);                 /* a little smoothing so the cells do not show */
    box_blur(acc,acc+cells,gh,gw,gw,1,1);
    for(int i=0;i<cells;i++){int v=acc[i]>>8;if(v<0)v=0;if(v>255)v=255;g->cell[i]=(uint8_t)(v*(v+255)/510);}   /* between straight and squared: a firm halo that still eases to nothing */
    fos_port_free(acc);
    for(int x=0;x<width;x++){
        int pos=((2*x+1)*g->gw*128)/width-128,cx,f;
        if(pos<0)pos=0;
        cx=pos>>8;f=pos&255;
        if(cx>=g->gw-1){cx=g->gw-1;f=0;}
        g->gx[x]=(uint16_t)(cx<<8|f);
    }
    return g;
}
static void glow_lines(struct glow *g,int top,int height){
    for(int k=0;k<16&&top+k<height;k++){
        int y=top+k,pos=((2*y+1)*g->gh*128)/height-128,cy,f;
        if(pos<0)pos=0;
        cy=pos>>8;f=pos&255;
        if(cy>=g->gh-1){cy=g->gh-1;f=0;}
        const uint8_t *a=g->cell+cy*g->gw,*c=f?a+g->gw:a;
        for(int x=0;x<g->gw;x++)g->line[k][x]=(a[x]*(256-f)+c[x]*f)>>8;
        g->line[k][g->gw]=g->line[k][g->gw-1];
    }
    g->top=top;
}
static inline int clip255(int v){return v<0?0:v>255?255:v;}
static size_t input(JDEC *jd, uint8_t *buf, size_t n)
{
    struct bake *b=jd->device;
    uint8_t discard[256];
    if (buf) {int start=fos_port_ms();int got=fos_port_read(b->input,buf,n);read_ms+=fos_port_ms()-start;return got>0?(size_t)got:0;}
    size_t done=0;
    while(done<n) {
        int k=n-done>sizeof(discard)?sizeof(discard):n-done;
        int got=fos_port_read(b->input,discard,k);
        if(got<=0) break;
        done+=got;
    }
    return done;
}
static int output(JDEC *jd, void *pixels, JRECT *r)
{
    struct bake *b=jd->device;
    if(fos_port_bake_yield()) {b->failed=2;return 0;}
    int colour_start=fos_port_ms();
    if(r->top/16*16!=b->strip_y) { b->failed=1; return 0; }
    const uint8_t *src=pixels;
    /* Neighbouring pixels are often identical (sky, shadows): reuse the last result. */
    unsigned last=0xFFFFFFFFu;uint8_t lastout[3]={0,0,0};
    struct glow *g=b->glow;
    if(g && g->top!=b->strip_y)glow_lines(g,b->strip_y,b->height);
    unsigned noise=((unsigned)r->top*131u+(unsigned)r->left*7919u+1u)*2654435761u;
    for(int y=r->top;y<=r->bottom;y++) for(int x=r->left;x<=r->right;x++,src+=3) {
        uint8_t *dst=b->strip+((y-b->strip_y)*b->width+x)*3;
        unsigned key=src[0]|src[1]<<8|src[2]<<16;
        if(key!=last){fos_lut_pixel(b->lut,src,lastout);last=key;}
        int o0=lastout[0],o1=lastout[1],o2=lastout[2];
        if(g){
            const int *line=g->line[y-b->strip_y];unsigned gx=g->gx[x];
            int add=((line[gx>>8]*(256-(gx&255))+line[(gx>>8)+1]*(gx&255))>>8)*g->strength>>8;
            if(add){o0=clip255(o0+add);o1=clip255(o1+(add*88>>8));o2=clip255(o2+(add*26>>8));}
        }
        if(b->grain){
            noise=noise*1664525u+1013904223u;
            int t=(int)((noise>>16)&255)+(int)((noise>>24)&255)-255;
            int l=(o0+o1*2+o2)>>2,d=l<128?128-l:l-128,w=256-(d<<1);
            if(w<64)w=64;
            int n=((t*b->grain>>8)*w)>>8;
            o0=clip255(o0+n);o1=clip255(o1+n);o2=clip255(o2+n);
        }
        dst[0]=o0;dst[1]=o1;dst[2]=o2;
    }
    colour_ms+=fos_port_ms()-colour_start;
    b->rows=r->bottom-b->strip_y+1;
    /* 4:2:0 has 16-row MCUs; 4:2:2/4:4:4 have 8-row MCUs. */
    if(r->right==b->width-1 && (b->rows==16 || r->bottom==b->height-1)) {
        fos_preview_rows(&b->preview,b->strip,b->strip_y,b->rows);
        int encode_start=fos_port_ms();
        if(!fos_jpeg_rows(b->writer,b->strip,b->width*3,b->rows)) {b->failed=1;return 0;}
        encode_ms+=fos_port_ms()-encode_start;
        b->strip_y+=b->rows; b->rows=0;
        fos_port_bake_progress(b->strip_y,b->height);
    }
    return 1;
}
static unsigned ex16(const uint8_t *p,int le){return le?(unsigned)p[0]|(unsigned)p[1]<<8:(unsigned)p[1]|(unsigned)p[0]<<8;}
static unsigned ex32(const uint8_t *p,int le){return le?ex16(p,1)|(ex16(p+2,1)<<16):ex16(p+2,0)|(ex16(p,0)<<16);}
static void exwrite(uint8_t *p,unsigned value,int bytes,int le){for(int i=0;i<bytes;i++)p[le?i:bytes-1-i]=(value>>(i*8))&255;}
static void resize_exif(uint8_t *exif,int size,int w,int h){
 if(!exif||size<14)return;
 uint8_t *t=exif+6;unsigned len=size-6;int le=t[0]=='I';unsigned offsets[2]={ex32(t+4,le),0};
 for(int which=0;which<2;which++){
  unsigned off=offsets[which];if(!off||off>len-2)continue;
  unsigned count=ex16(t+off,le);if(count>(len-off-2)/12)continue;
  for(unsigned i=0;i<count;i++){
   uint8_t *e=t+off+2+i*12;unsigned tag=ex16(e,le),type=ex16(e+2,le),n=ex32(e+4,le);
   if(tag==0x8769 && type==4 && n==1)offsets[1]=ex32(e+8,le);
   if(n!=1||(type!=3&&type!=4))continue;
   if(tag==256||tag==257||tag==0xA002||tag==0xA003){
    unsigned value=(tag==256||tag==0xA002)?w:h;
    memset(e+8,0,4);exwrite(e+8,value,type==3?2:4,le);
   }
  }
  /* The source thumbnail shows the unfiltered original. Do not advertise it. */
  unsigned next=off+2+count*12;
  if(which==0&&next<=len-4)memset(t+next,0,4);
 }
}

int fos_bake(const char *source, const char *dest, int film)
{
    int total_start=fos_port_ms();
    read_ms=colour_ms=encode_ms=0;
    error_text="JPEG source missing";
    struct bake b; memset(&b,0,sizeof(b));
    JDEC jd;
    void *pool=0;
    uint8_t *exif=0; int exif_len=0;
    int result=0;
    b.input=fos_port_open(source,0);
    if(!b.input) return 0;
    error_text="Invalid JPEG header";
    /* Preserve the source EXIF segment, including orientation and capture data. */
    uint8_t marker[4];
    if(fos_port_read(b.input,marker,2)!=2 || marker[0]!=255 || marker[1]!=216) goto done;
    int offset=2;
    while(fos_port_read(b.input,marker,4)==4) {
        if(marker[0]!=255 || marker[1]==218 || marker[1]==217)break;
        int len=(marker[2]<<8|marker[3])-2;
        if(len<0 || offset>1048576)break;
        if(marker[1]==225 && len>=6) {
            exif=fos_port_alloc(len);
            if(!exif)goto done;
            if(fos_port_read(b.input,exif,len)==len && !memcmp(exif,"Exif\0\0",6)) {exif_len=len;break;}
            fos_port_free(exif);exif=0;
        }
        offset+=len+4;
        if(fos_port_seek(b.input,offset)<0)goto done;
    }
    if(fos_port_seek(b.input,0)<0)goto done;
    error_text="Not enough decode memory";
    pool=fos_port_alloc_hot(65536);
    if(!pool) goto done;
    error_text="JPEG decode header unsupported";
    if(jd_prepare(&jd,input,pool,65536,&b)!=JDR_OK) goto done;
    error_text="Unsupported JPEG dimensions";
    if(jd.width<64 || jd.height<64 || jd.width>8192 || jd.height>8192) goto done;
    /* Every picture is baked at the size it was shot: Large stays Large, Medium Medium, Small Small. (Large used to be halved to
     * limit the processing time; it now costs about four times a Medium bake.) */
    b.scale=0;
    b.width=jd.width>>b.scale;b.height=jd.height>>b.scale;
    /* The review worker already made this picture's cache from the same source and look:
     * rebuilding it here would only repeat that work. */
    /* the review worker's early small cache is replaced by the full-size one built from these full-resolution rows */
    if(!fos_preview_final(dest)){fos_preview_init(&b.preview,b.width,b.height);b.preview.taken=fos_exif_taken(exif,exif_len);}
    error_text="Not enough image memory";
    b.strip=fos_port_alloc_hot(b.width*16*3);
    b.lut=fos_port_alloc_hot(33*33*33*3);
    if(!b.strip || !b.lut) goto done;
    char path[512];
    snprintf(path,sizeof(path),"%sML/DARKROOM/PLOOK%02d.LUT",fos_port_card(),film);
    error_text="Look file missing";
    void *fp=fos_port_open(path,0);
    if(!fp) goto done;
    uint8_t header[8];
    int valid=fos_port_read(fp,header,8)==8 && !memcmp(header,"FOPV",4);
    valid=valid && fos_port_read(fp,b.lut,33*33*33*3)==33*33*33*3;
    fos_port_close(fp);
    error_text="Look file incomplete/invalid";
    if(!valid) goto done;
    {   /* log how fast the table lookup runs here, so the log shows it directly */
        uint8_t px[3],o[3];int t0=fos_port_ms();unsigned v=12345u;
        for(int i=0;i<4096;i++){v=v*1664525u+1013904223u;px[0]=v>>8;px[1]=v>>16;px[2]=v>>24;fos_lut_pixel(b.lut,px,o);}
        bench_ms=fos_port_ms()-t0;
    }
    {   /* optional looks: the levels are read when the bake starts */
        int gl=fos_port_get(FS_GRAIN),hl=fos_port_get(FS_HALATION);
        if(gl<0)gl=0;
        if(gl>3)gl=3;
        if(hl<0)hl=0;
        if(hl>3)hl=3;
        b.grain=grain_peak[gl];
        /* the review cache is rebuilt from the baked rows whenever grain or halation is on: the one the review worker made came from
         * the source and the look only, so the review of a baked picture never showed them */
        if((gl||hl)&&!b.preview.pixels){fos_preview_init(&b.preview,b.width,b.height);b.preview.taken=fos_exif_taken(exif,exif_len);}
        const char *note="no review picture";
        if(hl){
            struct fos_preview pv;int orient=0;
            if(fos_preview_load(dest,&pv,&orient)){b.glow=glow_build(&pv,hl,b.width);fos_preview_free(&pv);note=b.glow?"ok":"out of memory";}
        }
        if(gl||hl){char m[96];snprintf(m,sizeof(m),"bake looks: grain %d halation %d (%s)",gl,hl,hl?note:"off");fos_port_log(m);}
    }
    resize_exif(exif,exif_len,b.width,b.height);
    struct fos_jpeg_meta meta={0}; meta.exif_blob=exif; meta.exif_length=exif_len;
    error_text="Cannot create output JPEG";
    b.writer=fos_jpeg_begin(dest,b.width,b.height,92,exif_len ? &meta : 0);
    if(!b.writer) goto done;
    error_text="Decode/write stopped or cancelled";
    int status=jd_decomp(&jd,output,b.scale);
    int ok=status==JDR_OK && !b.failed;
    int written=fos_jpeg_end(b.writer);
    result=written && ok;
    if(b.failed==2)error_text="Save cancelled by shutter/shutdown";
    else if(b.failed==1)error_text="JPEG card write failed";
    else if(status!=JDR_OK){snprintf(detail,sizeof(detail),"JPEG decode error %d",status);error_text=detail;}
    else if(!written)error_text="JPEG incomplete or card write failed";
    else error_text="Saved";
    b.writer=0;
    if(!result) fos_bake_remove(dest);
    else {
        int orientation=1;
        if(exif && exif_len>=14) {
            const uint8_t *t=exif+6;int len=exif_len-6,le=t[0]=='I';
#define READ16(p) (le?((unsigned)(p)[0]|(unsigned)(p)[1]<<8):((unsigned)(p)[1]|(unsigned)(p)[0]<<8))
#define READ32(p) (le?(READ16(p)|(READ16((p)+2)<<16)):(READ16((p)+2)|(READ16(p)<<16)))
            unsigned offset=READ32(t+4);
            if(offset<=(unsigned)len-2) {
                unsigned count=READ16(t+offset);
                for(unsigned i=0;i<count && offset+2+i*12+12<=(unsigned)len;i++) {
                    const uint8_t *entry=t+offset+2+i*12;
                    if(READ16(entry)==274 && READ16(entry+2)==3 && READ32(entry+4)==1)orientation=READ16(entry+8);
                }
            }
#undef READ16
#undef READ32
        }
        /* Best effort: preview failure never invalidates a completed JPEG. */
        fos_preview_save(dest,&b.preview,orientation);
    }
done:;
    char timing[260];
    snprintf(timing,sizeof(timing),"bake timing: total=%d read=%d colour=%d encode/write=%d other(decode, pauses)=%d ms result=%d; 4096-pixel LUT bench %d ms, lut@%08x strip@%08x pool@%08x enc@%08x",fos_port_ms()-total_start,read_ms,colour_ms,encode_ms,fos_port_ms()-total_start-read_ms-colour_ms-encode_ms,result,bench_ms,(unsigned)(uintptr_t)b.lut,(unsigned)(uintptr_t)b.strip,(unsigned)(uintptr_t)pool,(unsigned)fos_jpeg_state_addr);
    fos_port_log(timing);
    fos_preview_free(&b.preview);glow_free(b.glow);
    fos_port_close(b.input);
    fos_port_free(pool); fos_port_free(b.strip); fos_port_free(b.lut); fos_port_free(exif);
    return result;
}
