/* Small, optional display previews. The primary JPEG remains authoritative.
 *
 * BVP3 cache: "BVP3", width, height (u16), orientation, palette size, 2 spare bytes,
 * then the palette (RGB triples) and w*h indices 1..palette size. The palette is
 * made for each picture (median cut over a 5-5-5 histogram, the way the darkroom's
 * prints are) and the indices carry Floyd-Steinberg error diffusion, which hides
 * far more than the fixed 6x6x6 cube and 4x4 ordered dither of BVP2. */
#include "fos.h"
#include "fos_preview.h"
#pragma GCC optimize ("O2")   /* the pixel loops: the camera build is -Os, which left them at about 240 cycles a pixel */
/* working memory that is read and written per pixel comes from the ordinary cached heap (the port's fos_port_alloc_hot) */
__attribute__((weak)) void *fos_port_alloc_hot(int size){return fos_port_alloc(size);}
static void path_for(char *out,int size,const char *jpeg){
 snprintf(out,size,"%s",jpeg);int n=strlen(out);if(n>=4)memcpy(out+n-4,".BVP",5);
}

/* ---------------------------------------------------------------- palette */
struct box {int lo[3],hi[3];long count;};
struct quant {
 uint32_t *hist;uint8_t *inv;int16_t *err;
 struct box boxes[FOS_PREVIEW_COLOURS];
 uint8_t by_green[FOS_PREVIEW_COLOURS];
 uint8_t pal[FOS_PREVIEW_COLOURS][3];int npal;
};
static void box_shrink(const uint32_t *hist,struct box *b){
 int lo[3]={31,31,31},hi[3]={0,0,0};long count=0;
 for(int r=b->lo[0];r<=b->hi[0];r++)for(int g=b->lo[1];g<=b->hi[1];g++)for(int bl=b->lo[2];bl<=b->hi[2];bl++){
  uint32_t n=hist[r<<10|g<<5|bl];if(!n)continue;count+=n;
  if(r<lo[0])lo[0]=r;
  if(r>hi[0])hi[0]=r;
  if(g<lo[1])lo[1]=g;
  if(g>hi[1])hi[1]=g;
  if(bl<lo[2])lo[2]=bl;
  if(bl>hi[2])hi[2]=bl;
 }
 if(count){memcpy(b->lo,lo,sizeof(lo));memcpy(b->hi,hi,sizeof(hi));}
 b->count=count;
}
static void median_cut(struct quant *q){
 struct box *boxes=q->boxes;int n=1;
 boxes[0].lo[0]=boxes[0].lo[1]=boxes[0].lo[2]=0;boxes[0].hi[0]=boxes[0].hi[1]=boxes[0].hi[2]=31;
 box_shrink(q->hist,&boxes[0]);
 while(n<FOS_PREVIEW_COLOURS){
  /* the box with the most pixels that can still be split, weighted by its size */
  int best=-1;long best_score=0;
  for(int i=0;i<n;i++){
   struct box *b=&boxes[i];int span=0;
   for(int c=0;c<3;c++)if(b->hi[c]-b->lo[c]>span)span=b->hi[c]-b->lo[c];
   if(!span)continue;
   long score=b->count*(span+1);if(score>best_score){best_score=score;best=i;}
  }
  if(best<0)break;
  struct box *b=&boxes[best];int axis=0,span=-1;
  for(int c=0;c<3;c++)if(b->hi[c]-b->lo[c]>span){span=b->hi[c]-b->lo[c];axis=c;}
  long half=b->count/2,acc=0;int cut=b->lo[axis];
  for(int v=b->lo[axis];v<b->hi[axis];v++){
   int lo[3]={b->lo[0],b->lo[1],b->lo[2]},hi[3]={b->hi[0],b->hi[1],b->hi[2]};
   lo[axis]=hi[axis]=v;
   for(int r=lo[0];r<=hi[0];r++)for(int g=lo[1];g<=hi[1];g++)for(int bl=lo[2];bl<=hi[2];bl++)acc+=q->hist[r<<10|g<<5|bl];
   cut=v;if(acc>=half)break;
  }
  struct box nb=*b;b->hi[axis]=cut;nb.lo[axis]=cut+1;
  box_shrink(q->hist,b);box_shrink(q->hist,&nb);boxes[n++]=nb;
 }
 q->npal=n;
 for(int i=0;i<n;i++){
  const struct box *b=&boxes[i];long s[3]={0,0,0},cnt=0;
  for(int r=b->lo[0];r<=b->hi[0];r++)for(int g=b->lo[1];g<=b->hi[1];g++)for(int bl=b->lo[2];bl<=b->hi[2];bl++){
   long k=q->hist[r<<10|g<<5|bl];s[0]+=k*(r*8+4);s[1]+=k*(g*8+4);s[2]+=k*(bl*8+4);cnt+=k;
  }
  for(int c=0;c<3;c++)q->pal[i][c]=(uint8_t)(cnt?(s[c]+cnt/2)/cnt:0);
 }
}
/* the palette in order of green: searched outwards from the nearest green */
static void sort_palette(struct quant *q){
 for(int i=0;i<q->npal;i++){
  int j=i;for(;j>0&&q->pal[q->by_green[j-1]][1]>q->pal[i][1];j--)q->by_green[j]=q->by_green[j-1];
  q->by_green[j]=(uint8_t)i;
 }
}
static int nearest(struct quant *q,int cell){
 int r=(cell>>10)*8+4,g=(cell>>5&31)*8+4,b=(cell&31)*8+4;
 int lo=0,hi=q->npal;
 while(lo<hi){int mid=(lo+hi)>>1;if(q->pal[q->by_green[mid]][1]<g)lo=mid+1;else hi=mid;}
 int best=q->by_green[lo<q->npal?lo:q->npal-1],bd=1<<30;
 for(int up=lo,down=lo-1;up<q->npal||down>=0;){
  int done=1;
  if(up<q->npal){
   const uint8_t *p=q->pal[q->by_green[up]];int dg=p[1]-g;
   if(4*dg*dg<bd){int dr=r-p[0],db=b-p[2],dd=3*dr*dr+4*dg*dg+2*db*db;if(dd<bd){bd=dd;best=q->by_green[up];}up++;done=0;}
   else up=q->npal;
  }
  if(down>=0){
   const uint8_t *p=q->pal[q->by_green[down]];int dg=g-p[1];
   if(4*dg*dg<bd){int dr=r-p[0],db=b-p[2],dd=3*dr*dr+4*dg*dg+2*db*db;if(dd<bd){bd=dd;best=q->by_green[down];}down--;done=0;}
   else down=-1;
  }
  if(done)break;
 }
 q->inv[cell]=(uint8_t)(best+1);return best;
}
/* reduce p->rgb to p->pixels with a palette of its own; 0 when memory is short */
static int quantise(struct fos_preview *p){
 const int W=p->w,H=p->h;
 struct quant *q=fos_port_alloc_hot(sizeof(*q));
 if(!q)return 0;
 memset(q,0,sizeof(*q));
 q->hist=fos_port_alloc_hot(32768*4);q->inv=fos_port_alloc_hot(32768);q->err=fos_port_alloc_hot((W+2)*3*2*2);
 if(!q->hist||!q->inv||!q->err){fos_port_free(q->hist);fos_port_free(q->inv);fos_port_free(q->err);fos_port_free(q);return 0;}
 memset(q->hist,0,32768*4);memset(q->inv,0,32768);memset(q->err,0,(W+2)*3*2*2);
 for(int i=0;i<W*H;i++){const uint8_t *c=p->rgb+3*i;q->hist[(c[0]>>3)<<10|(c[1]>>3)<<5|c[2]>>3]++;}
 median_cut(q);sort_palette(q);
 for(int y=0;y<H;y++){
  /* serpentine: every other row right to left */
  const int dir=y&1?-1:1,st=3*dir;
  int16_t *cur=q->err+((y&1)?(W+2)*3:0),*nxt=q->err+((y&1)?0:(W+2)*3);
  memset(nxt,0,(W+2)*3*2);
  const int x=dir>0?0:W-1;
  const uint8_t *s=p->rgb+3*(y*W+x);uint8_t *o=p->pixels+y*W+x;
  int16_t *c=cur+(x+1)*3,*n=nxt+(x+1)*3;
  for(int k=0;k<W;k++,s+=st,o+=dir,c+=st,n+=st){
   int r=s[0]+(c[0]>>4),g=s[1]+(c[1]>>4),b=s[2]+(c[2]>>4);
   r=r<0?0:r>255?255:r;g=g<0?0:g>255?255:g;b=b<0?0:b>255?255:b;
   int cell=(r>>3)<<10|(g>>3)<<5|b>>3;
   int i=q->inv[cell]?q->inv[cell]-1:nearest(q,cell);
   *o=(uint8_t)(i+1);
   const uint8_t *e0=q->pal[i];
   int e=r-e0[0];
   c[st]=(int16_t)(c[st]+e*7);n[-st]=(int16_t)(n[-st]+e*3);n[0]=(int16_t)(n[0]+e*5);n[st]=(int16_t)(n[st]+e);
   e=g-e0[1];
   c[st+1]=(int16_t)(c[st+1]+e*7);n[1-st]=(int16_t)(n[1-st]+e*3);n[1]=(int16_t)(n[1]+e*5);n[st+1]=(int16_t)(n[st+1]+e);
   e=b-e0[2];
   c[st+2]=(int16_t)(c[st+2]+e*7);n[2-st]=(int16_t)(n[2-st]+e*3);n[2]=(int16_t)(n[2]+e*5);n[st+2]=(int16_t)(n[st+2]+e);
  }
 }
 p->npal=q->npal;memcpy(p->pal,q->pal,sizeof(p->pal));
 fos_port_free(q->hist);fos_port_free(q->inv);fos_port_free(q->err);fos_port_free(q);
 return 1;
}

/* ---------------------------------------------------------------- the cube, for when memory is short */
static void cube_palette(struct fos_preview *p){
 p->npal=216;
 for(int i=0;i<216;i++){p->pal[i][0]=i/36*51;p->pal[i][1]=i/6%6*51;p->pal[i][2]=i%6*51;}
}
static void cube_row(struct fos_preview *p,const uint8_t *rgb,int y,int span){
 for(int x=0;x<p->w;x++){
  const uint8_t *c=rgb+(x*span/p->w)*3;
  int d=fos_bayer[y&3][x&3]*51/16;
  p->pixels[y*p->w+x]=1+((c[0]+d)/51)*36+((c[1]+d)/51)*6+(c[2]+d)/51;
 }
}


/* ---------------------------------------------------------------- the review while it is built */
__attribute__((weak)) void fos_port_sleep(int ms){(void)ms;}
__attribute__((weak)) void fos_port_live_started(void){}
struct fos_live fos_live;
void fos_live_begin(const struct fos_preview *p,int orientation,const char *name){
 fos_live.active=0;fos_live.finishing=0;fos_live.acked=0;fos_live.rows=0;fos_live.result=0;
 fos_live.w=p->w;fos_live.h=p->h;fos_live.orientation=orientation;fos_live.rgb=p->rgb;
 snprintf(fos_live.name,sizeof(fos_live.name),"%s",name);
 fos_live.serial++;fos_live.active=p->rgb!=0;
 fos_port_live_started();
}
void fos_live_progress(const struct fos_preview *p){
 if(fos_live.active&&p->cur>0)fos_live.rows=p->cur;      /* rows below the one being summed are written */
}
static void flush_row(struct fos_preview *p);
void fos_live_end(struct fos_preview *p){
 if(!fos_live.active)return;
 flush_row(p);fos_live.rows=p->h;fos_live.finishing=1;
 for(int t=0;t<600&&!fos_live.acked;t+=10)fos_port_sleep(10);     /* the screen takes the last rows */
 fos_live.active=0;
 while(fos_live.reading)fos_port_sleep(2);
}
void fos_live_cube(uint8_t pal[216][3]){
 for(int i=0;i<216;i++){pal[i][0]=i/36*51;pal[i][1]=i/6%6*51;pal[i][2]=i%6*51;}
}
void fos_live_paint(uint8_t *dst,int from,int to){
 const int sw=fos_live.w,sh=fos_live.h,o=fos_live.orientation;
 if(!fos_live.rgb||sw<1||sh<1)return;
 if(to>sh)to=sh;
 if(from<0)from=0;
 if(to<=from)return;
 int ow=sw,oh=sh;if(o==6||o==8){ow=sh;oh=sw;}
 int dw=720,dh=oh*720/ow;if(dh>480){dw=dw*480/dh;dh=480;}
 const int left=(720-dw)/2,top=(480-dh)/2;
 /* the destination rows or columns whose source row lies in from..to-1 */
 int x0=0,x1=dw,y0=0,y1=dh;
 if(o==6||o==8){
  int a=o==6?sh-to:from,b=o==6?sh-from:to;       /* source rows as u, the display column index */
  x0=(a*dw+ow-1)/ow;x1=(b*dw+ow-1)/ow;
 }else{
  int a=o==3?sh-to:from,b=o==3?sh-from:to;
  y0=(a*dh+oh-1)/oh;y1=(b*dh+oh-1)/oh;
 }
 if(x1>dw)x1=dw;
 if(y1>dh)y1=dh;
 static uint16_t ux[720];
 for(int x=x0;x<x1;x++)ux[x]=(uint16_t)(x*ow/dw);
 for(int y=y0;y<y1;y++){
  int v=y*oh/dh;
  uint8_t *row=dst+(top+y)*720+left;
  for(int x=x0;x<x1;x++){
   int u=ux[x],sx=u,sy=v;
   if(o==3){sx=sw-1-u;sy=sh-1-v;}
   else if(o==6){sx=v;sy=sh-1-u;}
   else if(o==8){sx=sw-1-v;sy=u;}
   const uint8_t *c=fos_live.rgb+(sy*sw+sx)*3;
   int d=fos_bayer[y&3][x&3]*51/16;
   row[x]=1+((c[0]+d)/51)*36+((c[1]+d)/51)*6+(c[2]+d)/51;
  }
 }
}

/* ---------------------------------------------------------------- building a preview */
void fos_preview_free(struct fos_preview *p){
 fos_port_free(p->pixels);fos_port_free(p->rgb);fos_port_free(p->acc);fos_port_free(p->colmap);fos_port_free(p->colcnt);
 p->pixels=0;p->rgb=0;p->acc=0;p->colmap=0;p->colcnt=0;
}
int fos_preview_init(struct fos_preview *p,int w,int h){
 memset(p,0,sizeof(*p));if(w<1||h<1||w>8192||h>8192)return 0;
 p->sw=w;p->sh=h;p->w=w;p->h=h;
 if(p->w>720){p->h=p->h*720/p->w;p->w=720;}
 if(p->h>480){p->w=p->w*480/p->h;p->h=480;}
 if(p->w<1||p->h<1)return 0;
 p->cur=-1;
 p->pixels=fos_port_alloc_hot(p->w*p->h);if(!p->pixels)return 0;
 p->rgb=fos_port_alloc_hot(p->w*p->h*3);p->acc=fos_port_alloc_hot(p->w*3*4);
 p->colmap=fos_port_alloc_hot(p->sw*2);p->colcnt=fos_port_alloc_hot(p->w*2);
 if(p->rgb&&p->acc&&p->colmap&&p->colcnt){
  memset(p->acc,0,p->w*3*4);memset(p->colcnt,0,p->w*2);
  for(int x=0;x<p->sw;x++){p->colmap[x]=(uint16_t)(x*p->w/p->sw);p->colcnt[p->colmap[x]]++;}
 }else{
  fos_port_free(p->rgb);fos_port_free(p->acc);fos_port_free(p->colmap);fos_port_free(p->colcnt);
  p->rgb=0;p->acc=0;p->colmap=0;p->colcnt=0;cube_palette(p);
 }
 return 1;
}
/* write the finished output row from its sums */
static void flush_row(struct fos_preview *p){
 if(p->cur<0||!p->nrows)return;
 uint8_t *o=p->rgb+p->cur*p->w*3;uint32_t *a=p->acc;
 for(int x=0;x<p->w;x++,o+=3,a+=3){
  unsigned n=(unsigned)p->colcnt[x]*(unsigned)p->nrows;if(!n)n=1;
  unsigned inv=(65536+n/2)/n;
  for(int c=0;c<3;c++){unsigned v=(a[c]*inv+32768)>>16;o[c]=v>255?255:v;a[c]=0;}
 }
 p->nrows=0;
}
void fos_preview_rows(struct fos_preview *p,const uint8_t *rgb,int y0,int rows){
 if(!p->pixels)return;
 if(!p->rgb){
  /* no work memory: sample one source pixel per output pixel */
  int begin=(y0*p->h+p->sh-1)/p->sh;
  for(int y=begin;y<p->h;y++){
   int sy=y*p->sh/p->h;if(sy>=y0+rows)break;
   cube_row(p,rgb+(sy-y0)*p->sw*3,y,p->sw);
  }
  return;
 }
 /* area average: every source pixel counts toward the output pixel it falls in */
 for(int r=0;r<rows;r++){
  int oy=(int)((unsigned)(y0+r)*(unsigned)p->h/(unsigned)p->sh);
  if(oy>=p->h)oy=p->h-1;
  if(oy!=p->cur){flush_row(p);p->cur=oy;}
  const uint8_t *s=rgb+(size_t)r*p->sw*3;const uint16_t *map=p->colmap;uint32_t *a=p->acc;
  for(int x=0;x<p->sw;x++,s+=3){uint32_t *q=a+map[x]*3;q[0]+=s[0];q[1]+=s[1];q[2]+=s[2];}
  p->nrows++;
 }
}
/* the last row, then the palette; leaves p->pixels and p->pal ready to save */
static int finish(struct fos_preview *p){
 if(!p->rgb)return 1;
 flush_row(p);
 if(!quantise(p)){
  /* short of memory for the palette: the cube, from the averaged picture */
  cube_palette(p);
  for(int y=0;y<p->h;y++)cube_row(p,p->rgb+y*p->w*3,y,p->w);
 }
 fos_port_free(p->rgb);fos_port_free(p->acc);fos_port_free(p->colmap);fos_port_free(p->colcnt);
 p->rgb=0;p->acc=0;p->colmap=0;p->colcnt=0;
 return 1;
}
/* DateTimeOriginal (else DateTime) from an EXIF block as one sortable number:
 * seconds counted over 31-day months from 2000, so later pictures always compare larger. */
static unsigned xr16(const uint8_t *p,int le){return le?(unsigned)p[0]|(unsigned)p[1]<<8:(unsigned)p[1]|(unsigned)p[0]<<8;}
static unsigned xr32(const uint8_t *p,int le){return le?xr16(p,1)|(xr16(p+2,1)<<16):xr16(p+2,0)|(xr16(p,0)<<16);}
static uint32_t xtime(const uint8_t *s){
 for(int i=0;i<19;i++){int digit=s[i]>='0'&&s[i]<='9';if(i==4||i==7||i==10||i==13||i==16){if(s[i]==0)return 0;}else if(!digit)return 0;}
 #define D2(i) ((s[i]-'0')*10+(s[i+1]-'0'))
 int y=(s[0]-'0')*1000+(s[1]-'0')*100+D2(2),mo=D2(5),d=D2(8),h=D2(11),mi=D2(14),se=D2(17);
 if(y<2000||y>2099||mo<1||mo>12||d<1||d>31)return 0;
 return (uint32_t)((((y-2000)*12+(mo-1))*31+(d-1))*86400u+h*3600u+mi*60u+se);
}
static uint32_t find_time(const uint8_t *t,unsigned len,int le,unsigned ifd,unsigned want,unsigned *sub){
 if(ifd>len-2)return 0;
 unsigned count=xr16(t+ifd,le);if(count>(len-ifd-2)/12)return 0;
 for(unsigned i=0;i<count;i++){
  const uint8_t *e=t+ifd+2+i*12;unsigned tag=xr16(e,le);
  if(sub&&tag==0x8769&&xr16(e+2,le)==4)*sub=xr32(e+8,le);
  if(tag==want&&xr16(e+2,le)==2&&xr32(e+4,le)>=19){unsigned o=xr32(e+8,le);if(o<=len-20)return xtime(t+o);}
 }
 return 0;
}
uint32_t fos_exif_taken(const uint8_t *exif,int size){
 if(!exif||size<14||memcmp(exif,"Exif\0\0",6))return 0;
 const uint8_t *t=exif+6;unsigned len=size-6;int le=t[0]=='I';unsigned ifd0=xr32(t+4,le),sub=0;
 uint32_t v=find_time(t,len,le,ifd0,0x0132,&sub);        /* DateTime, and where the Exif IFD is */
 if(sub){uint32_t o=find_time(t,len,le,sub,0x9003,0);if(o)v=o;}   /* DateTimeOriginal wins */
 return v;
}
uint32_t fos_preview_taken(const char *jpeg){
 char path[512];path_for(path,sizeof(path),jpeg);
 void *fp=fos_port_open(path,0);if(!fp)return 0;
 uint8_t h[12];uint32_t v=0;
 if(fos_port_read(fp,h,12)==12&&!memcmp(h,"BVP3",4)&&h[10]==1){
  int w=h[4]|h[5]<<8,hh=h[6]|h[7]<<8,n=h[9];uint8_t t[4];
  if(fos_port_seek(fp,12+n*3+w*hh)==0&&fos_port_read(fp,t,4)==4)v=(uint32_t)t[0]|(uint32_t)t[1]<<8|(uint32_t)t[2]<<16|(uint32_t)t[3]<<24;
 }
 fos_port_close(fp);return v;
}
int fos_preview_save(const char *jpeg,struct fos_preview *p,int orientation){
 if(!p->pixels)return 0;
 if(!finish(p))return 0;
 if(p->npal<1||p->npal>FOS_PREVIEW_COLOURS)return 0;
 char path[512];path_for(path,sizeof(path),jpeg);void *fp=fos_port_open(path,1);if(!fp)return 0;
 uint8_t h[12]={'B','V','P','3',p->w&255,p->w>>8,p->h&255,p->h>>8,orientation,p->npal,p->taken?1:0,p->early?1:0};   /* h[10]=1: 4-byte capture time follows the pixels; h[11]=1: early small review */
 int ok=fos_port_write(fp,h,12)==12 && fos_port_write(fp,p->pal,p->npal*3)==p->npal*3
     && fos_port_write(fp,p->pixels,p->w*p->h)==p->w*p->h;
 if(ok&&p->taken){uint8_t t[4]={p->taken&255,(p->taken>>8)&255,(p->taken>>16)&255,p->taken>>24};ok=fos_port_write(fp,t,4)==4;}
 fos_port_close(fp);return ok;
}
int fos_quantise_rgb(const uint8_t *rgb,int w,int h,struct fos_preview *out){
 memset(out,0,sizeof(*out));out->w=w;out->h=h;
 out->pixels=fos_port_alloc_hot(w*h);if(!out->pixels)return 0;
 out->rgb=(uint8_t *)rgb;                       /* only read by quantise; not ours to free */
 int ok=quantise(out);out->rgb=0;
 if(!ok)fos_preview_free(out);
 return ok;
}
int fos_preview_final(const char *jpeg){
 char path[512];path_for(path,sizeof(path),jpeg);
 void *fp=fos_port_open(path,0);if(!fp)return 0;
 uint8_t h[12];int ok=fos_port_read(fp,h,12)==12 && !memcmp(h,"BVP3",4) && h[11]==0;
 fos_port_close(fp);return ok;
}
int fos_preview_exists(const char *jpeg){
 char path[512];path_for(path,sizeof(path),jpeg);
 void *fp=fos_port_open(path,0);if(!fp)return 0;
 uint8_t h[12];int ok=fos_port_read(fp,h,12)==12 && !memcmp(h,"BVP3",4);
 fos_port_close(fp);return ok;
}
int fos_preview_load(const char *jpeg,struct fos_preview *p,int *orientation){
 memset(p,0,sizeof(*p));char path[512];path_for(path,sizeof(path),jpeg);
 void *fp=fos_port_open(path,0);if(!fp)return 0;
 uint8_t h[12];int ok=0;
 if(fos_port_read(fp,h,12)!=12||memcmp(h,"BVP3",4))goto done;
 p->w=h[4]|h[5]<<8;p->h=h[6]|h[7]<<8;p->npal=h[9];
 if(p->w<1||p->h<1||p->w>720||p->h>480||h[8]<1||h[8]>8||p->npal<1||p->npal>FOS_PREVIEW_COLOURS)goto done;
 if(fos_port_read(fp,p->pal,p->npal*3)!=p->npal*3)goto done;
 p->pixels=fos_port_alloc(p->w*p->h);if(!p->pixels)goto done;
 if(fos_port_read(fp,p->pixels,p->w*p->h)!=p->w*p->h)goto done;
 for(int i=0;i<p->w*p->h;i++)if(p->pixels[i]<1||p->pixels[i]>p->npal)goto done;
 *orientation=h[8];ok=1;
done:fos_port_close(fp);if(!ok)fos_preview_free(p);return ok;
}

/* x/255 for 0 <= x < 65535 without a divide (exact) */
#define DIV255(x) (((x)+1+((x)>>8))>>8)
#define LERP255(a,b,t) ({int _x=(a)*(255-(t))+(b)*(t)+127;DIV255(_x);})

/* Reference: trilinear interpolation (21 blends per pixel). Kept for tests; the camera uses
 * the tetrahedral version below, which reads 4 table entries per channel instead of 8. */
void fos_lut_pixel_trilinear(const uint8_t *lut, const uint8_t *p, uint8_t *out)
{
    int ix[3], t[3];
    for(int c=0;c<3;c++) { int v=p[c]*32; ix[c]=DIV255(v); t[c]=v-ix[c]*255; if(ix[c]==32){ix[c]=31;t[c]=255;} }
    const uint8_t *q=lut+(ix[2]*1089+ix[1]*33+ix[0])*3;
    for(int c=0;c<3;c++,q++) {
        int v0=LERP255(q[0],q[3],t[0]),v1=LERP255(q[99],q[102],t[0]);
        int v2=LERP255(q[3267],q[3270],t[0]),v3=LERP255(q[3366],q[3369],t[0]);
        int lo=LERP255(v0,v1,t[1]),hi=LERP255(v2,v3,t[1]);
        out[c]=LERP255(lo,hi,t[2]);
    }
}

/* Tetrahedral interpolation: the cell is split into six tetrahedra by the order of the three
 * fractions, and the result is the base corner plus three weighted steps along edges.
 * Per channel: 4 table reads, 3 multiplies, 1 divide-free rounding. Differs from trilinear
 * by a level or two at most on smooth tables (see tests/lutcheck.py). */
/* Per 8-bit value: which table cell it falls in (0..31) and how far through it (0..255), packed ix << 8 | t. The same numbers the
 * lookup used to work out for every channel of every pixel (three DIV255s each); the camera spends about 240 cycles a pixel here. */
static uint16_t lut_cell[256];
static volatile int lut_cell_ready;
static void lut_cell_init(void)
{
    for(int v=0;v<256;v++){
        int x=v*32,ix=DIV255(x),t=x-ix*255;
        if(ix==32){ix=31;t=255;}
        lut_cell[v]=(uint16_t)(ix<<8|t);
    }
    lut_cell_ready=1;
}
void fos_lut_pixel(const uint8_t *lut, const uint8_t *p, uint8_t *out)
{
    if(!lut_cell_ready)lut_cell_init();
    const unsigned e0=lut_cell[p[0]],e1=lut_cell[p[1]],e2=lut_cell[p[2]];
    const int fr=e0&255,fg=e1&255,fb=e2&255;
    const uint8_t *q=lut+((e2>>8)*1089+(e1>>8)*33+(e0>>8))*3;
    /* corner offsets (bytes) for +1 in r, g, b */
    const int R=3,G=99,B=3267;
    int o1,o2,w1,w2,w3;                 /* path: base -> +o1 -> +o2 -> all three */
    if(fr>=fg){
        if(fg>=fb)      {o1=R;o2=R+G;w1=fr;w2=fg;w3=fb;}        /* r >= g >= b */
        else if(fr>=fb) {o1=R;o2=R+B;w1=fr;w2=fb;w3=fg;}        /* r >= b >  g */
        else            {o1=B;o2=B+R;w1=fb;w2=fr;w3=fg;}        /* b >  r >= g */
    }else{
        if(fr>=fb)      {o1=G;o2=G+R;w1=fg;w2=fr;w3=fb;}        /* g >  r >= b */
        else if(fg>=fb) {o1=G;o2=G+B;w1=fg;w2=fb;w3=fr;}        /* g >= b >  r */
        else            {o1=B;o2=B+G;w1=fb;w2=fg;w3=fr;}        /* b >  g >  r */
    }
    const int o3=R+G+B;
    for(int c=0;c<3;c++) {
        int c0=q[c],c1=q[o1+c],c2=q[o2+c],c3=q[o3+c];
        int x=c0*255+w1*(c1-c0)+w2*(c2-c1)+w3*(c3-c2)+127;
        out[c]=DIV255(x);
    }
}
