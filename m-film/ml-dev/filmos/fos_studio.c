#include "fos_ui.h"
#include "fos_dev.h"
#include "fos_studio.h"
#include "tjpgd.h"
#include "fos_preview.h"
#include <stdlib.h>
/* Use M-Film's original canister-and-list home layout. */
void studio_enter(int arg){home_enter(arg);}
void studio_key(int key){home_key(key);}
int studio_update(int dt){return home_update(dt);}
void studio_draw(void){home_draw();}

__attribute__((weak)) int fos_port_bake_pending(const char *name){(void)name;return 0;}
__attribute__((weak)) int fos_port_bake_revision(void){return 0;}
__attribute__((weak)) const char *fos_port_review_name(void){return "";}
__attribute__((weak)) void fos_port_bake_status(char *out,int size){if(size>0)out[0]=0;}
__attribute__((weak)) int fos_port_background_abort(void){return 0;}
#define LIMIT 512
static char (*names)[13];
static uint32_t *taken_at;     /* capture time of each listed picture, same order as names */
static int total,selection,viewing,dirty,have_picture,orientation;
static const char *message;
static int quick_review;
static uint8_t *picture;
static uint8_t photo_pal[FOS_PREVIEW_COLOURS][3];
static int photo_n;
static uint8_t *tiles;
static uint8_t tile_map[FOS_PREVIEW_COLOURS+1],tile_ready[6];
static int page_first=-1,next_tile,tile_due,status_due,review_revision;
#define TILE_W 192
#define TILE_H 108
/* newest capture first; with no capture time (an older cache), by name, newest number first */
static int comes_before(uint32_t ta,const char *a,uint32_t tb,const char *b){
 if(ta!=tb)return ta>tb;
 return strcmp(a,b)>0;
}
static int collect(void *ctx,const char *name,int is_dir){
 (void)ctx;int n=strlen(name);
 if(is_dir||n!=12||name[0]!='P'||(strcmp(name+8,".JPG")&&strcmp(name+8,".BVP")))return 1;
 char display[13];memcpy(display,name,13);memcpy(display+8,".JPG",5);
 for(int i=0;i<total;i++)if(!strcmp(names[i],display))return 1;
 if(!strcmp(name+8,".BVP")){
  char path[512];snprintf(path,sizeof(path),"%sPIXELJPG/%s",fos_port_card(),name);
  uint8_t magic[4];void *fp=fos_port_open(path,0);
  int valid=fp && fos_port_read(fp,magic,4)==4 && !memcmp(magic,"BVP3",4);
  if(fp)fos_port_close(fp);
  if(!valid)return 1;
 }
 char full[512];snprintf(full,sizeof(full),"%sPIXELJPG/%s",fos_port_card(),display);
 uint32_t key=fos_preview_taken(full);
 int at=0;while(at<total&&comes_before(taken_at[at],names[at],key,display))at++;
 if(at>=LIMIT)return 1;
 if(total<LIMIT)total++;
 for(int i=total-1;i>at;i--){memcpy(names[i],names[i-1],13);taken_at[i]=taken_at[i-1];}
 memcpy(names[at],display,13);taken_at[at]=key;return 1;
}
static void cached_picture(const struct fos_preview *p) {
 int w=p->w,h=p->h;if(orientation==6||orientation==8){w=p->h;h=p->w;}
 /* full screen, like Canon's playback: a 3:2 picture fills 720x480 exactly; others are letterboxed */
 int dw=720,dh=h*720/w;if(dh>480){dw=dw*480/dh;dh=480;}
 int left=(720-dw)/2,top=(480-dh)/2;
 memset(picture,PX_BLACK,720*480);
 for(int y=0;y<dh;y++)for(int x=0;x<dw;x++){
  int u=x*w/dw,v=y*h/dh,sx=u,sy=v;
  if(orientation==3){sx=p->w-1-u;sy=p->h-1-v;}
  else if(orientation==6){sx=v;sy=p->h-1-u;}
  else if(orientation==8){sx=p->w-1-v;sy=u;}
  picture[(top+y)*720+left+x]=p->pixels[sy*p->w+sx];
 }
}
static void picture_palette(const struct fos_preview *p){photo_n=p->npal;memcpy(photo_pal,p->pal,p->npal*3);}
static struct fos_zoom zoom;
static int zlevel,zvx,zvy,view_t0;     /* zoom level 0 fit, 1 half-size decode, 2 full size; viewport corner in that level's pixels */
static void zoom_off(void){fos_zoom_free(&zoom);zlevel=0;}
static void load_picture(void){
 if(zlevel)zoom_off();
 have_picture=0;quick_review=0;message="JPEG cannot be opened";view_t0=fos_port_ms();
 if(!picture)picture=fos_port_alloc(720*480);
 if(!picture){message="Not enough viewer memory";return;}
 memset(picture,PX_BLACK,720*480);
 char path[512];snprintf(path,sizeof(path),"%sPIXELJPG/%s",fos_port_card(),names[selection]);
 /* a finished JPEG with no current cache (older build, or a cache that was deleted):
  * make one from the file itself; its look is already in it, so no LUT is applied */
 if(!fos_preview_exists(path))fos_plain_preview(path);
 struct fos_preview cached;
 if(fos_preview_load(path,&cached,&orientation)){
  if(fos_port_get(FS_PORTRAIT)==1&&(orientation==6||orientation==8))orientation=1;         /* shot upright: shown turned, to fill the screen */
  cached_picture(&cached);picture_palette(&cached);fos_preview_free(&cached);have_picture=1;
  void *original=fos_port_open(path,0);quick_review=!original||fos_port_bake_pending(names[selection]);
  if(original)fos_port_close(original);
 }
}
/* ---- the review while it is still being built: rows arrive from the quick worker, are painted with a colour cube, and the
 * part not painted yet is a skeleton card with a light band sweeping over it. When the finished cache lands it replaces this. */
static int live_mode,live_serial,live_painted,live_last,skel_t;
static uint8_t live_cube[216][3];
static int live_enter(void){
 if(zlevel)zoom_off();
 if(!picture)picture=fos_port_alloc(720*480);
 if(!picture)return 0;
 const int sw=fos_live.w,sh=fos_live.h;
 int o=fos_live.orientation;
 if(fos_port_get(FS_PORTRAIT)==1&&(o==6||o==8))o=1;
 fos_live.paint_orient=o==fos_live.orientation?0:o;
 if(sw<1||sh<1)return 0;
 int ow=sw,oh=sh;if(o==6||o==8){ow=sh;oh=sw;}
 int dw=720,dh=oh*720/ow;if(dh>480){dw=dw*480/dh;dh=480;}
 memset(picture,PX_BLACK,720*480);
 for(int y=0;y<dh;y++)memset(picture+((480-dh)/2+y)*720+(720-dw)/2,PX_PANEL,dw);
 fos_live_cube(live_cube);photo_n=216;memcpy(photo_pal,live_cube,sizeof(live_cube));
 orientation=o;live_serial=fos_live.serial;live_painted=0;live_last=0;skel_t=0;live_mode=1;
 have_picture=1;quick_review=1;message="";view_t0=fos_port_ms();
 return 1;
}
static int live_step(int now){
 if(fos_live.serial!=live_serial){live_mode=0;load_picture();return 1;}
 int dirty_now=0;
 fos_live.reading=1;
 if(fos_live.active){
  int rows=fos_live.rows;
  if(rows>live_painted&&(now-live_last>=300||fos_live.finishing)){fos_live_paint(picture,live_painted,rows);live_painted=rows;live_last=now;dirty_now=1;}
  if(fos_live.finishing&&live_painted>=fos_live.h)fos_live.acked=1;
 }
 fos_live.reading=0;
 if(!fos_live.active){
  if(fos_live.result==2){live_mode=0;quick_review=1;return 1;}
  char path[512];snprintf(path,sizeof(path),"%sPIXELJPG/%s",fos_port_card(),names[selection]);
  if(fos_live.result==1&&fos_preview_exists(path)){live_mode=0;load_picture();return 1;}
 }
 return dirty_now;
}
/* the unpainted part of the picture (PX_PANEL) as a skeleton: a soft diagonal band moving across a dark card */
static void skeleton_over(uint8_t *px,int pitch,int now){
 int pos=(now/6)%(720+240+240)-240;
 for(int y=0;y<480;y++){
  uint8_t *r=px+y*pitch;const uint8_t *src=picture+y*720;
  for(int x=0;x<720;x++){
   if(src[x]!=PX_PANEL)continue;
   int d=x+y/2-pos;if(d<0)d=-d;
   r[x]=d<16?PX_G2:d<56?PX_G1:PX_PANEL;
  }
 }
}
void gallery_enter(int arg){total=selection=viewing=have_picture=0;dirty=1;message="No baked images yet";
 names=fos_port_alloc(LIMIT*13);taken_at=fos_port_alloc(LIMIT*4);
 if(!names||!taken_at){message="Not enough gallery memory";fos_port_free(names);names=0;fos_port_free(taken_at);taken_at=0;return;}
 char path[512];snprintf(path,sizeof(path),"%sPIXELJPG",fos_port_card());fos_port_list(path,collect,0);
 tiles=fos_port_alloc(6*TILE_W*TILE_H);memset(tile_ready,0,sizeof(tile_ready));page_first=-1;next_tile=0;tile_due=fos_port_ms()+500;
 if(arg==-2){
  const char *review=fos_port_review_name();
  int at=-1;for(int i=0;i<total;i++)if(!strcmp(names[i],review)){at=i;break;}
  int building=review[0]&&fos_live.serial&&!fos_live.result&&!strcmp(fos_live.name,review);
  if(at<0&&building&&total<LIMIT){                  /* its cache does not exist yet: list it first */
   for(int i=total;i>0;i--){memcpy(names[i],names[i-1],13);taken_at[i]=taken_at[i-1];}
   memcpy(names[0],review,13);taken_at[0]=0xFFFFFFFFu;total++;at=0;
  }
  if(at>=0){
   selection=at;viewing=1;
   if(building&&live_enter())return;
   load_picture();
  }
 }
}
void gallery_leave(void){live_mode=0;fos_port_free(tiles);tiles=0;fos_port_free(names);names=0;fos_port_free(taken_at);taken_at=0;zoom_off();fos_port_free(picture);picture=0;fos_port_palette();}
/* ---- zoom: SET steps Fit -> about 2x -> full size -> Fit; the dial presses pan; MENU goes back to Fit.
 * Decoding the finished JPEG takes seconds, so a progress box is shown; panning inside the window held in memory is instant. */
static int zoom_progress(int pct){
 static int shown=-1;if(pct==shown)return 0;shown=pct;
 uint8_t *px=cv->px;int pitch=cv->pitch;
 for(int y=214;y<266;y++)memset(px+y*pitch+160,PX_BLACK,400);
 char t[24];snprintf(t,sizeof(t),"ZOOM  %d%%",pct);ui_photo_text(&fos_font_sm,360,236,t,0,1);
 for(int y=248;y<254;y++){memset(px+y*pitch+180,PX_PANEL,360);memset(px+y*pitch+180,PX_RED,360*pct/100);}
 fos_port_present(px+214*pitch,pitch,214,266);return 0;
}
static void zclamp(void){
 int mx=zoom.dw>720?zoom.dw-720:0,my=zoom.dh>480?zoom.dh-480:0;
 if(zvx<0)zvx=0;
 if(zvx>mx)zvx=mx;
 if(zvy<0)zvy=0;
 if(zvy>my)zvy=my;
}
static void zoom_render(void){
 uint8_t *rgb=fos_port_alloc(720*480*3);if(!rgb){message="Not enough memory to zoom";return;}
 fos_zoom_view(&zoom,zvx,zvy,rgb);
 struct fos_preview q;
 if(fos_quantise_rgb(rgb,720,480,&q)){memcpy(picture,q.pixels,720*480);picture_palette(&q);fos_preview_free(&q);have_picture=1;}
 fos_port_free(rgb);
}
static int zoom_open_level(int level,int cx,int cy){
 char path[512];snprintf(path,sizeof(path),"%sPIXELJPG/%s",fos_port_card(),names[selection]);
 fos_zoom_free(&zoom);
 if(!fos_zoom_open(&zoom,path,level==1?1:0,orientation,cx,cy,zoom_progress)){zlevel=0;return 0;}
 zlevel=level;return 1;
}
static void zoom_cycle(void){
 if(quick_review||!have_picture||!total)return;                 /* needs the finished JPEG */
 if(zlevel==0){
  if(!zoom_open_level(1,-1,-1))return;
  zvx=(zoom.dw-720)/2;zvy=(zoom.dh-480)/2;
 }else if(zlevel==1){
  int cx=(zvx+360)*2,cy=(zvy+240)*2;
  if(!zoom_open_level(2,cx,cy))return;
  zvx=cx-360;zvy=cy-240;
 }else{zoom_off();load_picture();return;}
 zclamp();zoom_render();
}
static void zoom_pan(int dx,int dy){
 zvx+=dx*120;zvy+=dy*120;zclamp();
 if(!fos_zoom_holds(&zoom,zvx,zvy)){
  int cx=zvx+360,cy=zvy+240;
  if(!zoom_open_level(zlevel,cx,cy))return;
  zvx=cx-360;zvy=cy-240;zclamp();
 }
 zoom_render();
}
/* ---- the roll that made this picture (the film number is in the name, Psssffff.JPG), bottom right: it slides
 * in when the picture opens and a little canister turns beside the name while it is new or a bake is running */
static void roll_overlay(const char *name,int now,int moving){
 if(!(name[0]=='P'&&name[1]>='0'&&name[1]<='9'&&name[2]>='0'&&name[2]<='9'&&name[3]>='0'&&name[3]<='9'))return;
 int film=(name[1]-'0')*100+(name[2]-'0')*10+(name[3]-'0');if(film>=fos_nfilms)return;
 const struct fos_film *f=&fos_films[film];
 char text[48];snprintf(text,sizeof(text),"%s%s%s",f->brand,f->brand[0]?" ":"",f->name);
 int tw=fos_text_w(&fos_font_sm,text,0);if(tw>330)tw=330;
 int age=now-view_t0,slide=0;if(age<450){int s=450-age;slide=s*s*260/(450*450);}
 int x0=704-(tw+54)+slide,y0=404,w=tw+54,h=36;
 uint8_t *px=cv->px;int pitch=cv->pitch;
 for(int y=y0;y<y0+h;y++)for(int x=x0;x<x0+w&&x<720;x++)if(x>=0)px[y*pitch+x]=PX_BLACK;
 for(int y=y0;y<y0+h;y++)if(x0>=0&&x0<720)px[y*pitch+x0]=PX_RED;                    /* accent edge */
 /* the canister: steel caps, a white label with a red band, and a highlight that travels across as it turns */
 int cx=x0+12,cy=y0+4,hl=moving?(now/70)%16:6;
 for(int y=0;y<28;y++)for(int x=0;x<18;x++){
  int X=cx+x,Y=cy+y;if(X<0||X>=720)continue;
  uint8_t c=(y<3||y>=25)?PX_G4:(y>=11&&y<16)?PX_RED:PX_WHITE;
  if(c!=PX_G4&&(x==0||x==17))c=PX_G2;                                              /* roundness at the edges */
  else if(c==PX_WHITE&&(x-1)==hl)c=PX_G5;else if(c==PX_RED&&(x-1)==hl)c=PX_AMBER2;
  px[Y*pitch+X]=c;
 }
 if(x0+34<720)ui_photo_text(&fos_font_sm,x0+34,y0+24,text,0,0);
}
void gallery_key(int k){
 if(live_mode&&k!=FK_MENU&&k!=FK_HOME&&k!=FK_SET)live_mode=0;
 if(k==FK_MENU||k==FK_HOME){if(viewing&&zlevel){zoom_off();load_picture();dirty=1;return;}if(viewing){viewing=0;page_first=-1;fos_port_palette();}else ui_go(SCR_HOME,0,GO_POP);dirty=1;return;}
 if(k==FK_SET&&total&&!viewing){viewing=1;load_picture();dirty=1;return;}
 if(k==FK_SET&&viewing){if(live_mode)return;zoom_cycle();dirty=1;return;}
 if(viewing&&zlevel&&(k==FK_LEFT||k==FK_RIGHT||k==FK_UP||k==FK_DOWN)){zoom_pan(k==FK_LEFT?-1:k==FK_RIGHT?1:0,k==FK_UP?-1:k==FK_DOWN?1:0);dirty=1;return;}
 if(k==FK_LEFT||k==FK_UP||k==FK_WHEEL_L){if(selection>0)selection--;if(viewing)load_picture();}
 if(k==FK_RIGHT||k==FK_DOWN||k==FK_WHEEL_R){if(selection<total-1)selection++;if(viewing)load_picture();}
 dirty=1;
}
/* The bake bar along the bottom of a review picture: a rounded, glossy red tube that eases toward the real percent; paused it pulses, and before progress exists
 * (preparing the review) a block sweeps back and forth. Only its rows are redrawn, about 8 times a second. */
static int bar_last,bar_dirty,bar_fill16;
#define BAR_X 20
#define BAR_W 620
#define BAR_Y 444
#define BAR_H 12
/* A rounded, lit tube: each row has two tones (dithered when they differ) for the glossy top, the body and the shaded underside,
 * and an inset so the ends are semicircles. The fixed palette has one red, so the dark red is PX_SPARE. */
static const uint8_t bar_inset[BAR_H]={4,2,1,1,0,0,0,0,1,1,2,4};
static void tube(uint8_t *px,int pitch,int x,int w,const uint8_t tone[BAR_H][2]){
 for(int y=0;y<BAR_H;y++){
  int in=bar_inset[y],n=w-2*in;if(n<=0)continue;
  uint8_t *d=px+(BAR_Y+y)*pitch+x+in;
  for(int i=0;i<n;i++)d[i]=((x+in+i+y)&1)?tone[y][0]:tone[y][1];
 }
}
static const uint8_t tube_red[BAR_H][2]={
 {PX_SPARE,PX_SPARE},{PX_WHITE,PX_RED},{PX_RED,PX_WHITE},{PX_RED,PX_RED},{PX_RED,PX_RED},{PX_RED,PX_RED},
 {PX_RED,PX_RED},{PX_RED,PX_RED},{PX_RED,PX_SPARE},{PX_SPARE,PX_SPARE},{PX_SPARE,PX_SPARE},{PX_SPARE,PX_SPARE}};
static const uint8_t tube_grey[BAR_H][2]={
 {PX_G2,PX_G2},{PX_G5,PX_G5},{PX_G4,PX_G4},{PX_G3,PX_G3},{PX_G3,PX_G3},{PX_G3,PX_G3},
 {PX_G3,PX_G3},{PX_G3,PX_G3},{PX_G3,PX_G2},{PX_G2,PX_G2},{PX_G1,PX_G1},{PX_G1,PX_G1}};
static const uint8_t tube_track[BAR_H][2]={
 {PX_BLACK,PX_BLACK},{PX_BLACK,PX_BLACK},{PX_PANEL,PX_PANEL},{PX_PANEL,PX_PANEL},{PX_PANEL,PX_PANEL},{PX_PANEL,PX_PANEL},
 {PX_PANEL,PX_PANEL},{PX_PANEL,PX_PANEL},{PX_PANEL,PX_PANEL},{PX_PANEL,PX_G1},{PX_G1,PX_G2},{PX_G1,PX_G1}};

/* a small mark on a picture: a green disc with a tick when it is fully baked, a red disc while it still waits for its bake */
static void badge_px(uint8_t *px,int pitch,int cx,int cy,int baked){
 for(int dy=-12;dy<=12;dy++)for(int dx=-12;dx<=12;dx++){
  int d2=dx*dx+dy*dy;if(d2>132)continue;
  px[(cy+dy)*pitch+cx+dx]=d2>100?PX_BLACK:baked?PX_GREEN:PX_RED;
 }
 if(!baked)return;
 for(int k=0;k<=4;k++){int x=cx-6+k*6/4,y=cy+k*5/4-1;for(int w=0;w<2;w++)px[(y+w)*pitch+x]=px[(y+w)*pitch+x+1]=PX_WHITE;}     /* the short stroke down */
 for(int k=0;k<=8;k++){int x=cx-1+k,y=cy+4-k*8/8;for(int w=0;w<2;w++)px[(y+w)*pitch+x]=px[(y+w)*pitch+x+1]=PX_WHITE;}        /* and the long one up */
}
static void badge_tile(int cx,int cy,int baked){
 fos_disc(cv,cx*8,cy*8,12*8,FC(RAMP_GRAY,0),OPAQUE);
 fos_disc(cv,cx*8,cy*8,10*8,baked?FC(RAMP_GREEN,200):FC(RAMP_RED,215),OPAQUE);
 if(baked){
  fos_line(cv,(cx-5)*8,cy*8,(cx-2)*8,(cy+4)*8,2*8,FC(RAMP_GRAY,255),OPAQUE);
  fos_line(cv,(cx-2)*8,(cy+4)*8,(cx+5)*8,(cy-4)*8,2*8,FC(RAMP_GRAY,255),OPAQUE);
 }
}
static void bake_bar(const struct fos_bake_info *bi,int now){
 uint8_t *px=cv->px;int pitch=cv->pitch;
 for(int y=BAR_Y-8;y<BAR_Y+BAR_H+6;y++)memset(px+y*pitch+BAR_X+BAR_W+4,PX_BLACK,720-(BAR_X+BAR_W+4)-14);   /* backing for the number */
 for(int y=BAR_Y-2;y<BAR_Y+BAR_H+2;y++)memset(px+y*pitch+BAR_X-2,PX_BLACK,BAR_W+4);
 tube(px,pitch,BAR_X,BAR_W,tube_track);
 if(!bi->active&&!bi->paused){
  int span=BAR_W-120,t=(now/5)%(2*span),pos=t<span?t:2*span-t;
  bar_fill16=0;
  tube(px,pitch,BAR_X+pos,120,tube_red);
  return;                                                         /* no number until there is progress */
 }
 int target=bi->percent*16;
 if(target<bar_fill16)bar_fill16=target;
 else if(target>bar_fill16){int step=(target-bar_fill16)/4;bar_fill16+=step?step:1;}
 int fill=BAR_W*bar_fill16/1600;if(fill<BAR_H&&bi->percent>0)fill=BAR_H;
 tube(px,pitch,BAR_X,fill,bi->paused&&((now/500)&1)?tube_grey:tube_red);
 {char num[8];snprintf(num,sizeof(num),"%d%%",bar_fill16/16);ui_photo_text(&fos_font_sm,700,BAR_Y+BAR_H+2,num,0,2);}   /* the number follows the bar */
}
int gallery_update(int dt){
 (void)dt;
 if(viewing){int now=fos_port_ms();
  if(live_mode){if(live_step(now))dirty=1;if(live_mode&&now-skel_t>=220){skel_t=now;dirty=1;}}
  if(have_picture&&now-bar_last>=(live_mode?220:125)){struct fos_bake_info bi;fos_port_bake_info(&bi);if(bi.queued||now-view_t0<2500){bar_last=now;bar_dirty=1;}}
  if(now-status_due>500){status_due=now;dirty=1;
  int revision=fos_port_bake_revision();if(revision!=review_revision){review_revision=revision;if(!viewing)dirty=1;if(!zlevel&&!live_mode)load_picture();}
 }return dirty||bar_dirty;}
 if(!tiles||!total)return dirty;
 int first=selection/6*6;
 if(next_tile<6&&first+next_tile<total){int now2=fos_port_ms();if(now2-skel_t>=150){skel_t=now2;dirty=1;}}   /* the skeleton tiles shimmer */
 if(first!=page_first){page_first=first;next_tile=0;memset(tile_ready,0,sizeof(tile_ready));dirty=1;tile_due=fos_port_ms()+400;}
 if(!ui_settled()||fos_port_ms()<tile_due||next_tile>=6)return dirty;
 int slot=next_tile++,i=page_first+slot;
 if(i<total){
  char path[512];snprintf(path,sizeof(path),"%sPIXELJPG/%s",fos_port_card(),names[i]);
  struct fos_preview cached;int angle;
  if(fos_preview_load(path,&cached,&angle)){
   for(int k=1;k<=cached.npal;k++){
    int r=cached.pal[k-1][0],g=cached.pal[k-1][1],b=cached.pal[k-1][2],best=1,dist=0x7FFFFFFF;
    for(int i=1;i<255;i++){
     int dr=r-((fos_pal[i]>>16)&255),dg=g-((fos_pal[i]>>8)&255),db=b-(fos_pal[i]&255);
     int d=dr*dr+dg*dg+db*db;if(d<dist){dist=d;best=i;}
    }tile_map[k]=best;
   }
   int w=cached.w,h=cached.h;if(angle==6||angle==8){w=cached.h;h=cached.w;}
   int dw=TILE_W,dh=h*TILE_W/w;
   if(dh>TILE_H){dw=w*TILE_H/h;dh=TILE_H;}
   memset(tiles+slot*TILE_W*TILE_H,fos_ix(C_BG),TILE_W*TILE_H);
   for(int y=0;y<dh;y++)for(int x=0;x<dw;x++){
    int u=x*w/dw,v=y*h/dh,sx=u,sy=v;
    if(angle==3){sx=cached.w-1-u;sy=cached.h-1-v;}
    else if(angle==6){sx=v;sy=cached.h-1-u;}
    else if(angle==8){sx=cached.w-1-v;sy=u;}
    tiles[(slot*TILE_H+(TILE_H-dh)/2+y)*TILE_W+(TILE_W-dw)/2+x]=tile_map[cached.pixels[sy*cached.w+sx]];
   }
   fos_preview_free(&cached);tile_ready[slot]=1;dirty=1;
  }
 }
 return dirty;
}
void gallery_draw(void){
 if(!dirty&&!ui_clobbered&&!bar_dirty)return;
 int partial=!dirty&&!ui_clobbered&&viewing&&have_picture&&!live_mode;
 dirty=ui_clobbered=bar_dirty=0;
 if(viewing&&have_picture){
  struct fos_bake_info bi;fos_port_bake_info(&bi);int now=fos_port_ms();
  if(partial){memcpy(cv->px+400*cv->pitch,picture+400*720,80*720);}
  else{memcpy(cv->px,picture,720*480);fos_port_photo(photo_pal,photo_n,256);if(live_mode)skeleton_over(cv->px,cv->pitch,now);
   if(zlevel){char zt[64];int fx=zoom.dw>=zoom.dh?zoom.dw*10/720:zoom.dh*10/480;snprintf(zt,sizeof(zt),"ZOOM %d.%dX   DIAL PAN   SET NEXT   MENU FIT",fx/10,fx%10);ui_photo_text(&fos_font_xs,20,22,zt,0,0);}
   else if(live_mode){char cap[40];snprintf(cap,sizeof(cap),"DEVELOPING PREVIEW  %d%%",fos_live.h?live_painted*100/fos_live.h:0);ui_photo_text(&fos_font_xs,20,22,cap,0,0);}
   else ui_photo_text(&fos_font_xs,20,22,quick_review?"SOURCE JPEG + LUT":names[selection],0,0);
   badge_px(cv->px,cv->pitch,694,26,!fos_port_bake_pending(names[selection]));}
  if(bi.queued)bake_bar(&bi,now);
  roll_overlay(names[selection],now,bi.queued||now-view_t0<2500);
  char status[96];fos_port_bake_status(status,sizeof(status));
  ui_photo_text(&fos_font_xs,20,471,status[0]?status:"DIAL BROWSE    MENU IMAGES",0,0);
  if(partial){ui_damage(400,480);return;}
 }else{
  fos_port_palette();fos_fill(cv,0,0,720,480,C_BG);
  fos_text(cv,&fos_font_lg,32,65,"Images",C_TEXT,OPAQUE,0);
  if(!total||viewing)fos_text(cv,&fos_font_sm,32,150,message,C_TEXT2,OPAQUE,0);
  else for(int slot=0;slot<6;slot++){
   int i=selection/6*6+slot;if(i>=total)break;
   int x=24+(slot%3)*232,y=102+(slot/3)*150,active=i==selection;
   fos_rrect(cv,x,y,216,138,16,active?C_PANEL_HI:C_PANEL,OPAQUE);
   if(active)fos_rrect_line(cv,x-5,y-5,226,148,21,4,C_ACCENT,OPAQUE);      /* bold outline on the selected picture */
   if(tile_ready[slot]&&tiles){
    for(int row=0;row<TILE_H;row++)memcpy(cv->px+(y+8+row)*cv->pitch+x+12,tiles+(slot*TILE_H+row)*TILE_W,TILE_W);
   }else if(tiles&&slot>=next_tile){                                        /* still to load: a skeleton with a light band */
    int now2=fos_port_ms(),bx=x+12+((now2/5+slot*70)%(TILE_W+160))-80,x1=x+12+TILE_W;
    fos_fill(cv,x+12,y+8,TILE_W,TILE_H,C_BG);
    int a=bx<x+12?x+12:bx,b=bx+80>x1?x1:bx+80;if(b>a)fos_fill(cv,a,y+8,b-a,TILE_H,C_PANEL_HI);
   }else fos_text(cv,&fos_font_xs,x+23,y+65,"SET TO PREVIEW",C_TEXT3,OPAQUE,0);
   badge_tile(x+216-24,y+22,!fos_port_bake_pending(names[i]));
   fos_text(cv,&fos_font_xs,x+12,y+130,names[i],active?C_TEXT:C_TEXT2,OPAQUE,0);
  }
  fos_text(cv,&fos_font_xs,32,442,"DIAL  BROWSE     SET  OPEN     MENU  HOME",C_TEXT3,OPAQUE,0);
 }
 ui_damage_all();
}

__attribute__((weak)) void fos_port_bake_info(struct fos_bake_info *info){
 memset(info,0,sizeof(*info));info->remaining=-1;
}
static int bake_status_dirty,bake_status_last;
/* ---- the look list (M-Fn on the back screen): names only, chosen with the joystick or the dial, each step takes effect at once */
static int looks_sel,looks_dirty;
void looks_enter(int arg){
 (void)arg;looks_sel=fos_port_get(FS_FILM);
 if(looks_sel<0||looks_sel>=fos_nfilms)looks_sel=0;
 looks_dirty=1;
}
static void looks_apply(void){fos_port_action(FA_LOAD,looks_sel,0);}
void looks_key(int k){
 const int n=fos_nfilms;if(n<1)return;
 if(k==FK_LEFT||k==FK_UP||k==FK_WHEEL_L){looks_sel=(looks_sel+n-1)%n;looks_apply();looks_dirty=1;return;}
 if(k==FK_RIGHT||k==FK_DOWN||k==FK_WHEEL_R){looks_sel=(looks_sel+1)%n;looks_apply();looks_dirty=1;return;}
 if(k==FK_SET||k==FK_MENU||k==FK_HOME||k==FK_PLAY)ui_leave();
}
int looks_update(int dt){(void)dt;return looks_dirty||ui_clobbered;}
void looks_draw(void){
 if(!looks_dirty&&!ui_clobbered)return;
 looks_dirty=ui_clobbered=0;
 const int n=fos_nfilms;
 fos_fill(cv,0,0,720,480,C_BG);
 fos_text(cv,&fos_font_lg,32,65,"Look",C_TEXT,OPAQUE,0);
 char counter[16];snprintf(counter,sizeof(counter),"%d / %d",looks_sel+1,n);
 fos_text_r(cv,&fos_font_sm,688,62,counter,C_TEXT3,OPAQUE,0);
 for(int r=-4;r<=4;r++){
  const int i=((looks_sel+r)%n+n)%n,y=118+(r+4)*32;
  const struct fos_film *f=&fos_films[i];
  char text[48];snprintf(text,sizeof(text),"%s%s%s",f->brand,f->brand[0]?" ":"",f->name);
  if(r==0){fos_rrect(cv,24,y-23,672,32,16,C_ACCENT,OPAQUE);fos_text(cv,&fos_font_md,44,y,text,C_TEXT,OPAQUE,0);}
  else fos_text(cv,&fos_font_md,44,y,text,r==-1||r==1?C_TEXT2:C_TEXT3,OPAQUE,0);
 }
 fos_text(cv,&fos_font_xs,32,442,"JOYSTICK OR DIAL  CHOOSE     SET  DONE",C_TEXT3,OPAQUE,0);
 ui_damage_all();
}

void bake_status_enter(int arg){
 (void)arg;bake_status_dirty=1;bake_status_last=fos_port_ms();
 for(int y=0;y<FOS_H;y++)for(int x=0;x<FOS_W;x++)
  ui_bg[y*FOS_W+x]=fos_ix_dither(RAMP_RED,(y*y/(FOS_H-1))*y*LV12(145)/((FOS_H-1)*(FOS_H-1)),x,y);
 ui_to_bg(1);ui_status_bar("DEVELOP");
 fos_text(cv,&fos_font_lg,32,83,"Develop",C_TEXT,OPAQUE,0);
 struct hint hints[]={{"MENU","Home"},{"PLAY","Images"},{"SHUTTER","Shoot"}};
 ui_hints_dark=1;ui_hints(hints,3);ui_hints_dark=0;ui_to_bg(0);
}
void bake_status_key(int key){
 if(key==FK_MENU||key==FK_HOME)ui_go(SCR_HOME,2,GO_POP);
 if(key==FK_PLAY)ui_go(SCR_PRINTS,0,GO_PUSH);
}
int bake_status_update(int dt){
 (void)dt;int now=fos_port_ms();
 if(now-bake_status_last>=250){bake_status_last=now;bake_status_dirty=1;}
 else if(now-bake_status_last>=90){struct fos_bake_info bi;fos_port_bake_info(&bi);if(bi.queued&&!bi.active&&!bi.paused){bake_status_last=now;bake_status_dirty=1;}}   /* the preparing sweep */
 return bake_status_dirty;
}
void bake_status_draw(void){
 if(!bake_status_dirty&&!ui_clobbered)return;
 bake_status_dirty=ui_clobbered=0;
 ui_bg_restore(0,0,FOS_W,FOS_H);
 struct fos_bake_info info;fos_port_bake_info(&info);
 char text[96];
 const char *state=!info.queued?(info.failed?"Queue finished with errors":"All caught up"):info.paused?"Paused for capture":info.active?"Baking JPEG":"Preparing image preview";
 fos_text(cv,&fos_font_md,34,125,state,C_TEXT,OPAQUE,0);
 if(info.queued){
  fos_text(cv,&fos_font_sm,34,156,info.current,C_TEXT2,OPAQUE,0);
  snprintf(text,sizeof(text),"%d%%",info.percent);
  fos_text_r(cv,&fos_font_lg,684,157,text,C_TEXT,OPAQUE,0);
  fos_rrect(cv,32,179,656,16,8,C_PANEL,OPAQUE);
  if(info.percent>0)fos_rrect(cv,32,179,656*info.percent/100<16?16:656*info.percent/100,16,8,C_ACCENT,OPAQUE);
  else if(!info.active&&!info.paused){int span=656-140,t=(fos_port_ms()/4)%(2*span),pos=t<span?t:2*span-t;fos_rrect(cv,32+pos,179,140,16,8,C_ACCENT,OPAQUE);}   /* preparing: a block sweeps the track */
  if(info.paused)snprintf(text,sizeof(text),"Resumes when the capture finishes");
  else if(info.remaining>=0)snprintf(text,sizeof(text),"About %d seconds remaining",info.remaining);
  else snprintf(text,sizeof(text),"Estimating time remaining...");
  fos_text(cv,&fos_font_sm,34,225,text,C_TEXT2,OPAQUE,0);
  snprintf(text,sizeof(text),"Queue: %d image%s including current",info.queued,info.queued==1?"":"s");
  fos_text(cv,&fos_font_sm,34,258,text,C_TEXT,OPAQUE,0);
  for(int i=0;i<info.next_count;i++){
   snprintf(text,sizeof(text),"Next %d   %s",i+1,info.next[i]);
   fos_text(cv,&fos_font_sm,34,294+i*25,text,C_TEXT2,OPAQUE,0);
  }
 }else{
  fos_text(cv,&fos_font_sm,34,168,"New photographs bake automatically in the background.",C_TEXT2,OPAQUE,0);
  fos_text(cv,&fos_font_sm,34,200,info.failed?"Check ML/FILMROLL.LOG for failure details.":"Open Images to browse your saved copies.",C_TEXT2,OPAQUE,0);
 }
 snprintf(text,sizeof(text),"This session: %d saved   /   %d failed",info.completed,info.failed);
 fos_text(cv,&fos_font_sm,34,397,text,C_TEXT2,OPAQUE,0);
 ui_damage_all();
}
