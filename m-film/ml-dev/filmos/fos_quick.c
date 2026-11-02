/* LUT review from the actual captured JPEG, never the EXIF thumbnail.
 * Decode to at least LCD resolution before making a fitted review cache.
 * fos_plain_preview does the same for a finished JPEG without a LUT. */
#include "fos.h"
#include "fos_preview.h"
#include "tjpgd.h"
__attribute__((weak)) void *fos_port_alloc_hot(int size){return fos_port_alloc(size);}
struct quick {
 void *fp;unsigned position;
 int w,h,row,rows,read_ms,colour_ms,rows_ms,live;uint8_t *strip,*lut;struct fos_preview preview;
};
static size_t quick_read(JDEC *jd,uint8_t *dst,size_t n){
 struct quick *q=jd->device;
 if(!dst){if(fos_port_seek(q->fp,q->position+n)<0)return 0;q->position+=n;return n;}
 int t=fos_port_ms();int got=fos_port_read(q->fp,dst,n);q->read_ms+=fos_port_ms()-t;if(got<=0)return 0;q->position+=got;return got;
}
static int quick_output(JDEC *jd,void *pixels,JRECT *r){
 struct quick *q=jd->device;uint8_t *src=pixels;(void)jd;
 if(fos_port_background_abort())return 0;
 if(r->top/16*16!=q->row)return 0;
 int t0=fos_port_ms();unsigned last=0xFFFFFFFFu;uint8_t lastout[3]={0,0,0};
 for(int y=r->top;y<=r->bottom;y++)for(int x=r->left;x<=r->right;x++,src+=3){
  uint8_t *dst=q->strip+((y-q->row)*q->w+x)*3;
  if(q->lut){unsigned key=(unsigned)src[0]<<16|(unsigned)src[1]<<8|src[2];if(key!=last){fos_lut_pixel(q->lut,src,lastout);last=key;}dst[0]=lastout[0];dst[1]=lastout[1];dst[2]=lastout[2];}
  else{dst[0]=src[0];dst[1]=src[1];dst[2]=src[2];}
 }
 q->colour_ms+=fos_port_ms()-t0;
 q->rows=r->bottom-q->row+1;
 if(r->right==q->w-1&&(q->rows==16||r->bottom==q->h-1)){
  int t1=fos_port_ms();fos_preview_rows(&q->preview,q->strip,q->row,q->rows);if(q->live)fos_live_progress(&q->preview);q->rows_ms+=fos_port_ms()-t1;q->row+=q->rows;q->rows=0;
 }return 1;
}
static unsigned q16(const uint8_t *p,int le){return le?(unsigned)p[0]|(unsigned)p[1]<<8:(unsigned)p[1]|(unsigned)p[0]<<8;}
static unsigned q32(const uint8_t *p,int le){return le?q16(p,1)|(q16(p+2,1)<<16):q16(p+2,0)|(q16(p,0)<<16);}
static int make_preview(const char *source,const char *dest,int film,int use_lut,int early){
 struct quick q;memset(&q,0,sizeof(q));void *pool=0;uint8_t *exif=0;int ok=0,orientation=1;uint32_t taken=0;
 q.fp=fos_port_open(source,0);if(!q.fp)return 0;
 uint8_t h[4];int offset=2;
 if(fos_port_read(q.fp,h,2)!=2||h[0]!=255||h[1]!=216)goto done;
 while(offset<1048576&&fos_port_read(q.fp,h,4)==4){
  if(h[0]!=255||h[1]==218)break;
  int n=(h[2]<<8|h[3])-2;if(n<0)break;
  if(h[1]==225&&n>=14){
   exif=fos_port_alloc(n);if(!exif)goto done;
   if(fos_port_read(q.fp,exif,n)==n&&!memcmp(exif,"Exif\0\0",6)){
    taken=fos_exif_taken(exif,n);
    uint8_t *t=exif+6;unsigned len=n-6;int le=t[0]=='I';unsigned off=q32(t+4,le);
    if(off<=len-2){
     unsigned count=q16(t+off,le);
     if(count<=(len-off-2)/12){
      for(unsigned i=0;i<count;i++){uint8_t *e=t+off+2+i*12;
       if(q16(e,le)==274&&q16(e+2,le)==3&&q32(e+4,le)==1)orientation=q16(e+8,le);
      }

     }
    }
   }break;
  }
  offset+=n+4;if(fos_port_seek(q.fp,offset)<0)goto done;
 }
 if(fos_port_seek(q.fp,0)<0)goto done;
 pool=fos_port_alloc_hot(65536);if(!pool)goto done;
 if(use_lut){
  q.lut=fos_port_alloc_hot(33*33*33*3);if(!q.lut)goto done;
  char path[512];snprintf(path,sizeof(path),"%sML/DARKROOM/PLOOK%02d.LUT",fos_port_card(),film);
  void *fp=fos_port_open(path,0);if(!fp)goto done;
  uint8_t header[8];int valid=fos_port_read(fp,header,8)==8&&!memcmp(header,"FOPV",4);
  valid=valid&&fos_port_read(fp,q.lut,33*33*33*3)==33*33*33*3;fos_port_close(fp);if(!valid)goto done;
 }
 JDEC jd;if(jd_prepare(&jd,quick_read,pool,65536,&q)!=JDR_OK)goto done;
 int tw=jd.width,th=jd.height;
 const int TW=720,TH=480;
 if(tw>TW){th=th*TW/tw;tw=TW;}
 if(th>TH){tw=tw*TH/th;th=TH;}
 int scale=0;
 while(scale<3 && (jd.width>>(scale+1))>=tw && (jd.height>>(scale+1))>=th)scale++;
 q.w=jd.width>>scale;q.h=jd.height>>scale;
 if(!fos_preview_init(&q.preview,q.w,q.h))goto done;
 q.preview.taken=taken;q.preview.early=0;
 if(early){const char *slash=strrchr(dest,'/');fos_live_begin(&q.preview,orientation,slash?slash+1:dest);q.live=1;}
 q.strip=fos_port_alloc_hot(q.w*16*3);if(!q.strip)goto done;
 if(jd_decomp(&jd,quick_output,scale)!=JDR_OK)goto done;
 if(q.live)fos_live_end(&q.preview);
 int ts=fos_port_ms();ok=fos_preview_save(dest,&q.preview,orientation);
 {char m[160];snprintf(m,sizeof(m),"quick preview stages: read=%d colour=%d palette/average=%d save=%d ms (%dx%d from %dx%d, scale 1/%d)",q.read_ms,q.colour_ms,q.rows_ms,fos_port_ms()-ts,q.w,q.h,jd.width,jd.height,1<<scale);fos_port_log(m);}
done:if(q.live){if(fos_live.active){fos_live.finishing=1;fos_live.active=0;while(fos_live.reading)fos_port_sleep(2);}fos_live.result=ok?1:2;}
 fos_port_close(q.fp);fos_preview_free(&q.preview);fos_port_free(q.strip);fos_port_free(q.lut);fos_port_free(pool);fos_port_free(exif);return ok;
}
int fos_quick_preview(const char *source,const char *dest,int film){return make_preview(source,dest,film,1,1);}
int fos_plain_preview(const char *jpeg){return make_preview(jpeg,jpeg,0,0,0);}

/* ---------------------------------------------------------------- zoom */
struct zdec {void *fp;unsigned position;struct fos_zoom *z;int w,h,orientation,last_row;int (*progress)(int);};
static size_t zread(JDEC *jd,uint8_t *dst,size_t n){
 struct zdec *d=jd->device;
 if(!dst){if(fos_port_seek(d->fp,d->position+n)<0)return 0;d->position+=n;return n;}
 int got=fos_port_read(d->fp,dst,n);if(got<=0)return 0;d->position+=got;return got;
}
static int zout(JDEC *jd,void *bitmap,JRECT *r){
 struct zdec *d=jd->device;struct fos_zoom *z=d->z;const uint8_t *p=bitmap;
 if(d->progress&&r->top!=d->last_row){d->last_row=r->top;if(d->progress(r->top*100/d->h))return 0;}
 for(int y=r->top;y<=r->bottom;y++)for(int x=r->left;x<=r->right;x++,p+=3){
  int u=x,v=y;
  if(d->orientation==3){u=d->w-1-x;v=d->h-1-y;}
  else if(d->orientation==6){u=d->h-1-y;v=x;}
  else if(d->orientation==8){u=y;v=d->w-1-x;}
  int ix=u-z->wx,iy=v-z->wy;
  if((unsigned)ix<(unsigned)z->ww&&(unsigned)iy<(unsigned)z->wh)z->px[iy*z->ww+ix]=(uint16_t)((p[0]>>3)<<11|(p[1]>>2)<<5|(p[2]>>3));
 }
 return 1;
}
#define ZWIN_W 1440
#define ZWIN_H 960
int fos_zoom_open(struct fos_zoom *z,const char *jpeg,int scale,int orientation,int cx,int cy,int (*progress)(int)){
 memset(z,0,sizeof(*z));struct zdec d;memset(&d,0,sizeof(d));void *pool=0;int ok=0;JDEC jd;
 d.fp=fos_port_open(jpeg,0);if(!d.fp)return 0;
 pool=fos_port_alloc_hot(65536);if(!pool)goto done;
 if(jd_prepare(&jd,zread,pool,65536,&d)!=JDR_OK)goto done;
 d.w=jd.width>>scale;d.h=jd.height>>scale;d.orientation=orientation;d.progress=progress;d.last_row=-1;d.z=z;
 z->dw=d.w;z->dh=d.h;if(orientation==6||orientation==8){z->dw=d.h;z->dh=d.w;}
 z->ww=z->dw<ZWIN_W?z->dw:ZWIN_W;z->wh=z->dh<ZWIN_H?z->dh:ZWIN_H;
 if(cx<0)cx=z->dw/2;
 if(cy<0)cy=z->dh/2;
 z->wx=cx-z->ww/2;
 if(z->wx<0)z->wx=0;
 if(z->wx>z->dw-z->ww)z->wx=z->dw-z->ww;
 z->wy=cy-z->wh/2;
 if(z->wy<0)z->wy=0;
 if(z->wy>z->dh-z->wh)z->wy=z->dh-z->wh;
 z->px=fos_port_alloc_hot(z->ww*z->wh*2);if(!z->px)goto done;
 memset(z->px,0,z->ww*z->wh*2);
 ok=jd_decomp(&jd,zout,scale)==JDR_OK;
done:fos_port_close(d.fp);fos_port_free(pool);
 if(!ok){fos_port_free(z->px);memset(z,0,sizeof(*z));}
 return ok;
}
int fos_zoom_holds(const struct fos_zoom *z,int vx,int vy){
 int w=z->dw<720?z->dw:720,h=z->dh<480?z->dh:480;
 return vx>=z->wx&&vy>=z->wy&&vx+w<=z->wx+z->ww&&vy+h<=z->wy+z->wh;
}
void fos_zoom_view(const struct fos_zoom *z,int vx,int vy,uint8_t *rgb){
 memset(rgb,0,720*480*3);
 int ox=z->dw<720?(720-z->dw)/2:0,oy=z->dh<480?(480-z->dh)/2:0;       /* a picture smaller than the screen is centred */
 for(int y=0;y<480;y++){
  int gy=vy+y-oy;if(gy<z->wy||gy>=z->wy+z->wh||gy<0||gy>=z->dh)continue;
  const uint16_t *src=z->px+(gy-z->wy)*z->ww;
  for(int x=0;x<720;x++){
   int gx=vx+x-ox;if(gx<z->wx||gx>=z->wx+z->ww||gx<0||gx>=z->dw)continue;
   uint16_t c=src[gx-z->wx];uint8_t *o=rgb+(y*720+x)*3;
   o[0]=(uint8_t)((c>>11)<<3|(c>>13));o[1]=(uint8_t)(((c>>5)&63)<<2|((c>>9)&3));o[2]=(uint8_t)((c&31)<<3|((c>>2)&7));
  }
 }
}
void fos_zoom_free(struct fos_zoom *z){fos_port_free(z->px);memset(z,0,sizeof(*z));}

