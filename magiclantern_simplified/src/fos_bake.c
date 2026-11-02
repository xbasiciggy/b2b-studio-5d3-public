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
    struct fos_preview preview;
};
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
    for(int y=r->top;y<=r->bottom;y++) for(int x=r->left;x<=r->right;x++,src+=3) {
        uint8_t *dst=b->strip+((y-b->strip_y)*b->width+x)*3;
        unsigned key=src[0]|src[1]<<8|src[2]<<16;
        if(key!=last){fos_lut_pixel(b->lut,src,lastout);last=key;}
        dst[0]=lastout[0];dst[1]=lastout[1];dst[2]=lastout[2];
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
    /* Preserve captured resolution for every Canon Small and Medium JPEG.
     * Large retains test6's half-size output to limit processing cost. */
    b.scale=(unsigned)jd.width*jd.height>3840u*2560u ? 1 : 0;
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
    fos_preview_free(&b.preview);
    fos_port_close(b.input);
    fos_port_free(pool); fos_port_free(b.strip); fos_port_free(b.lut); fos_port_free(exif);
    return result;
}
