# film OS simulator: make && ./out/sim scenes/shelf.txt out/frames/shelf
SRC = sim.c port_files.c fonts.c films.c ../fos_gfx.c ../fos_film.c ../fos_ui.c ../fos_shelf.c ../fos_pages.c ../fos_hud.c \
      ../fos_quick.c ../fos_preview.c ../fos_studio.c ../tjpgd.c ../fos_dark.c ../fos_dev.c ../fos_lab.c ../fos_looks.c ../fos_jpeg.c
CFLAGS = -O2 -g -Wall -Wextra -Wno-unused-parameter -Wdouble-promotion -std=gnu99
# M-Film's version, as the camera build gets it (VERSION at the top of the repository)
CFLAGS += -DMFILM_VERSION='"$(shell tr -d " \r\n" < ../../../VERSION)"'

out/sim: $(SRC) ../*.h out/FOSFONT.BIN out/FOSFILMS.BIN
	mkdir -p out
	$(CC) $(CFLAGS) -o $@ $(SRC) -lm

# the fonts, as the camera finds them on its card (ML/DATA/FOSFONT.BIN)
out/FOSFONT.BIN: ../fos_font.c ../tools/fontblob.c ../fos_gfx.h
	mkdir -p out
	$(CC) -O2 -I.. -o out/fontblob ../tools/fontblob.c
	./out/fontblob $@

# the films beyond the built-in ones (ML/DATA/FOSFILMS.BIN) and the tables
# baked from their LUTs (films/getluts.py fetches those)
out/FOSFILMS.BIN: ../tools/genfilms.py ../tools/catalog.py ../../films/films.py ../../films/filmluts.py ../fos_film.c
	mkdir -p out
	python3 ../tools/genfilms.py $@ --tables out/lut

clean:
	rm -rf out

# developing film: ./out/devtest [-f film] [-r raw.u16] in.CR2 out.FOP out.ppm
DEV = devtest.c port_files.c films.c ../fos_dev.c ../fos_looks.c ../fos_film.c ../fos_gfx.c ../fos_jpeg.c
out/devtest: $(DEV) ../*.h out/FOSFILMS.BIN
	mkdir -p out
	$(CC) $(CFLAGS) -DFOS_DEV_TEST -o $@ $(DEV) -lm
