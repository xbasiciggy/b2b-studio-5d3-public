"""Film stocks whose colour comes from the G'MIC film LUTs collected in
https://github.com/YahiaAngelo/Film-Luts (fetch them with getluts.py).

Every entry: key, the LUT's path in that repository (without .cube),
brand, name, box speed, and kind:

  neg     colour negative, C-41        slide   colour reversal, E-6 / K-14
  xpro    cross-processed              bw      black and white
  print   motion-picture print stock (the look of a cinema print)

films.py turns them into lab films: the colour from the LUT, the rest
(scanner exposure, halation, grain, softness) from the kind and speed.
The LUTs take ordinary sRGB pictures as input.
"""

REPO = "https://github.com/YahiaAngelo/Film-Luts"
ZIP = "https://codeload.github.com/YahiaAngelo/Film-Luts/zip/refs/heads/main"
DIR = "filmluts"                    # under films/luts/

STOCKS = [
    # colour negative
    ("portra160", "negative_new/kodak_portra_160", "Kodak", "Portra 160", 160, "neg"),
    ("portra800", "negative_new/kodak_portra_800", "Kodak", "Portra 800", 800, "neg"),
    ("portra160nc", "negative_old/kodak_portra_160_nc", "Kodak", "Portra 160NC", 160, "neg"),
    ("portra160vc", "negative_old/kodak_portra_160_vc", "Kodak", "Portra 160VC", 160, "neg"),
    ("portra400nc", "negative_old/kodak_portra_400_nc", "Kodak", "Portra 400NC", 400, "neg"),
    ("portra400vc", "negative_old/kodak_portra_400_vc", "Kodak", "Portra 400VC", 400, "neg"),
    ("portra400uc", "negative_old/kodak_portra_400_uc", "Kodak", "Portra 400UC", 400, "neg"),
    ("elite200", "negative_color/kodak_elite_color_200", "Kodak", "Elite Color 200", 200, "neg"),
    ("elite400", "negative_color/kodak_elite_color_400", "Kodak", "Elite Color 400", 400, "neg"),
    ("pro160c", "negative_new/fuji_160c", "Fujifilm", "Pro 160C", 160, "neg"),
    ("pro400h", "negative_new/fuji_400h", "Fujifilm", "Pro 400H", 400, "neg"),
    ("pro800z", "negative_new/fuji_800z", "Fujifilm", "Pro 800Z", 800, "neg"),
    ("reala100", "negative_color/fuji_superia_reala_100", "Fujifilm", "Reala 100", 100, "neg"),
    ("superia100", "negative_old/fuji_superia_100", "Fujifilm", "Superia 100", 100, "neg"),
    ("superia200", "negative_color/fuji_superia_200", "Fujifilm", "Superia 200", 200, "neg"),
    ("superia800", "negative_old/fuji_superia_800", "Fujifilm", "Superia 800", 800, "neg"),
    ("superia1600", "negative_old/fuji_superia_1600", "Fujifilm", "Superia 1600", 1600, "neg"),
    ("xtra800", "negative_color/fuji_superia_x-tra_800", "Fujifilm", "X-TRA 800", 800, "neg"),
    ("vista200", "negative_color/agfa_vista_200", "Agfa", "Vista 200", 200, "neg"),
    ("ultra100", "negative_color/agfa_ultra_color_100", "Agfa", "Ultra 100", 100, "neg"),
    ("redscale100", "negative_color/lomography_redscale_100", "Lomography", "Redscale 100", 100, "neg"),
    # colour reversal
    ("velvia100", "colorslide/fuji_velvia_100_generic", "Fujifilm", "Velvia 100", 100, "slide"),
    ("provia100f", "colorslide/fuji_provia_100f", "Fujifilm", "Provia 100F", 100, "slide"),
    ("provia400f", "colorslide/fuji_provia_400f", "Fujifilm", "Provia 400F", 400, "slide"),
    ("provia400x", "colorslide/fuji_provia_400x", "Fujifilm", "Provia 400X", 400, "slide"),
    ("astia100f", "colorslide/fuji_astia_100f", "Fujifilm", "Astia 100F", 100, "slide"),
    ("sensia100", "colorslide/fuji_sensia_100", "Fujifilm", "Sensia 100", 100, "slide"),
    ("kodachrome25", "colorslide/kodak_kodachrome_25", "Kodak", "Kodachrome 25", 25, "slide"),
    ("kodachrome64", "colorslide/kodak_kodachrome_64", "Kodak", "Kodachrome 64", 64, "slide"),
    ("kodachrome200", "colorslide/kodak_kodachrome_200", "Kodak", "Kodachrome 200", 200, "slide"),
    ("e100gx", "colorslide/kodak_e-100_gx_ektachrome_100", "Kodak", "Ektachrome E100GX", 100, "slide"),
    ("e100vs", "colorslide/kodak_ektachrome_100_vs", "Kodak", "Ektachrome E100VS", 100, "slide"),
    ("elitechrome200", "colorslide/kodak_elite_chrome_200", "Kodak", "Elite Chrome 200", 200, "slide"),
    ("elitechrome400", "colorslide/kodak_elite_chrome_400", "Kodak", "Elite Chrome 400", 400, "slide"),
    ("extracolor100", "colorslide/kodak_elite_extracolor_100", "Kodak", "ExtraColor 100", 100, "slide"),
    ("precisa100", "colorslide/agfa_precisa_100", "Agfa", "Precisa 100", 100, "slide"),
    # cross-processed
    ("elite100xpro", "negative_color/kodak_elite_100_xpro", "Kodak", "Elite 100 X-Pro", 100, "xpro"),
    ("superia200xpro", "colorslide/fuji_superia_200_xpro", "Fujifilm", "Superia 200 X-Pro", 200, "xpro"),
    ("xproslide200", "colorslide/lomography_x-pro_slide_200", "Lomography", "X-Pro Slide 200", 200, "xpro"),
    # black and white
    ("tmax100", "bw/kodak_t-max_100", "Kodak", "T-MAX 100", 100, "bw"),
    ("tmax400", "bw/kodak_t-max_400", "Kodak", "T-MAX 400", 400, "bw"),
    ("tmax3200", "bw/kodak_t-max_3200", "Kodak", "T-MAX P3200", 3200, "bw"),
    ("bw400cn", "bw/kodak_bw_400_cn", "Kodak", "BW400CN", 400, "bw"),
    ("hie", "bw/kodak_hie_hs_infra", "Kodak", "HIE Infrared", 400, "bw"),
    ("delta100", "bw/ilford_delta_100", "Ilford", "Delta 100", 100, "bw"),
    ("delta400", "bw/ilford_delta_400", "Ilford", "Delta 400", 400, "bw"),
    ("delta3200", "bw/ilford_delta_3200", "Ilford", "Delta 3200", 3200, "bw"),
    ("fp4", "bw/ilford_fp_4_plus_125", "Ilford", "FP4 Plus", 125, "bw"),
    ("panf50", "bw/ilford_pan_f_plus_50", "Ilford", "Pan F Plus", 50, "bw"),
    ("hps800", "bw/ilford_hps_800", "Ilford", "HPS 800", 800, "bw"),
    ("xp2", "bw/ilford_xp_2", "Ilford", "XP2 Super", 400, "bw"),
    ("acros100", "bw/fuji_neopan_acros_100", "Fujifilm", "Acros 100", 100, "bw"),
    ("neopan1600", "bw/fuji_neopan_1600", "Fujifilm", "Neopan 1600", 1600, "bw"),
    ("apx25", "bw/agfa_apx_25", "Agfa", "APX 25", 25, "bw"),
    ("apx100", "bw/agfa_apx_100", "Agfa", "APX 100", 100, "bw"),
    ("retro80s", "bw/rollei_retro_80s", "Rollei", "Retro 80S", 80, "bw"),
    ("retro100", "bw/rollei_retro_100_tonal", "Rollei", "Retro 100 Tonal", 100, "bw"),
    ("ortho25", "bw/rollei_ortho_25", "Rollei", "Ortho 25", 25, "bw"),
    ("ir400", "bw/rollei_ir_400", "Rollei", "Infrared 400", 400, "bw"),
    # cinema print stocks
    ("kodak2383", "print/kodak_2383_constlmap", "Kodak", "Vision 2383", 200, "print"),
    ("kodak2393", "print/kodak_2393_constlmap", "Kodak", "Vision 2393", 200, "print"),
    ("fuji3510", "print/fuji_3510_constlmap", "Fujifilm", "Eterna 3510", 200, "print"),
    ("fuji3513", "print/fuji_3513_constlmap", "Fujifilm", "Eterna 3513", 200, "print"),
]


def grain_for(iso, kind):
    """Grain amplitude and clump size for a box speed: the faster, the coarser."""
    steps = {25: 0, 50: 1, 64: 1.3, 80: 1.6, 100: 2, 125: 2.3, 160: 2.7, 200: 3,
             400: 4, 800: 5, 1600: 6, 3200: 7}[iso]
    amount = 0.006 + 0.0045 * steps
    if kind == "slide":
        amount *= 0.7
    if kind == "print":
        amount = 0.008
    return round(amount, 4), round(0.7 + 0.11 * steps, 2)


def film(key, path, brand, name, iso, kind):
    amount, size = grain_for(iso, kind)
    f = dict(
        name=f"{brand} {name}", iso=iso, kind=kind, balance=5500,
        lut=f"{DIR}/{path}.cube", lut_space="srgb",
        # the film's own colour when the LUT isn't there: a plain print
        ev=0.2 if kind == "neg" else 0.0, sat=1.0,
        contrast=(0.18, 0.18, 0.18), pivot=(0.0, 0.0, 0.0),
        black=(0.03, 0.03, 0.03), white=(0.96, 0.96, 0.96),
        halation=0.8, grain=amount, grain_size=size, soft=0.7,
    )
    if kind in ("slide", "xpro"):
        f.update(slide=True, contrast=(0.22, 0.22, 0.22), black=(0.015,) * 3, white=(0.98,) * 3,
                 halation=0.4, soft=0.5)
    if kind == "bw":
        f.update(mono=(0.30, 0.59, 0.11), contrast=(0.19,), pivot=(0.0,), black=(0.03,), white=(0.96,),
                 halation=0.4)
    if kind == "print":
        f.update(halation=0.5, soft=0.5)
    return f


FILMS = {s[0]: film(*s) for s in STOCKS}
