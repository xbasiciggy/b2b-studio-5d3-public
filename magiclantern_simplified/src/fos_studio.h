#ifndef FOS_STUDIO_H
#define FOS_STUDIO_H
void studio_enter(int); void studio_key(int); int studio_update(int); void studio_draw(void);
void gallery_enter(int);void gallery_leave(void);void gallery_key(int);int gallery_update(int);void gallery_draw(void);
struct fos_bake_info {
 int queued,active,paused,percent,remaining,completed,failed;
 int next_count;
 char current[32],next[3][24];
};
void fos_port_bake_info(struct fos_bake_info *);
void bake_status_enter(int);void bake_status_key(int);int bake_status_update(int);void bake_status_draw(void);
#endif
