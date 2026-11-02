#ifndef FOS_BAKE_H
#define FOS_BAKE_H
/* Returns success only after a complete new JPEG has been closed. */
int fos_bake(const char *source, const char *dest, int film);
const char *fos_bake_error(void);
int fos_bake_remove(const char *path);
#endif
