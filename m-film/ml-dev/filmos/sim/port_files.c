/* the film OS's card on a PC: a folder (FOS_CARD, else out/card/) and stdio */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include "../fos.h"

void *fos_port_open(const char *path, int write) { return fopen(path, write ? "wb" : "rb"); }
int  fos_port_read(void *f, void *buf, int size) { return (int) fread(buf, 1, size, f); }
int  fos_port_write(void *f, const void *buf, int size) { return (int) fwrite(buf, 1, size, f); }
int  fos_port_seek(void *f, int offset) { return fseek(f, offset, SEEK_SET); }
void fos_port_close(void *f) { if (f) fclose(f); }

const char *fos_port_model() { return "Canon EOS M"; }

const char *fos_port_card()
{
    const char *c = getenv("FOS_CARD");
    return c ? c : "out/card/";
}

int fos_port_list(const char *dir, int (*fn)(void *ctx, const char *name, int is_dir), void *ctx)
{
    DIR *d = opendir(dir);
    if (!d) return -1;
    struct dirent *e;
    int n = 0;
    while ((e = readdir(d)))
    {
        if (e->d_name[0] == '.') continue;
        char path[512];
        struct stat st;
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        int is_dir = !stat(path, &st) && S_ISDIR(st.st_mode);
        n++;
        if (!fn(ctx, e->d_name, is_dir)) break;
    }
    closedir(d);
    return n;
}

int fos_port_mkdir(const char *path)
{
    struct stat st;
    if (!stat(path, &st)) return S_ISDIR(st.st_mode) ? 0 : -1;
    /* FOS_SHORT_NAMES: a card that takes 8.3 names only */
    const char *name = strrchr(path, '/');
    name = name ? name + 1 : path;
    if (getenv("FOS_SHORT_NAMES") && (strlen(name) > 8 || strchr(name, ' '))) return -1;
    return mkdir(path, 0777);
}
