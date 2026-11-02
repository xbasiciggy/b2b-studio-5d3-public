"""What the camera shows of the films beyond the built-in eight: the line
under the name on the shelf, and the canister label (struct fos_label).
The look itself comes from films/films.py; genfilms.py puts both together.

A label: style (band, cap or stripes), inks as (ramp, level), the small
line on top, the name, the big number (digits and H, P, T only) and the
small line under it. A B&W film's leader is grey.
"""

GRAY, METAL, AMBER, RED, ORANGE, CRIMSON, GREEN, VIOLET, BLUE = range(9)
BAND, CAP, STRIPES = range(3)

DESC = {
    "cinestill400t": "Your own LUT, cine colour",
    "portra160": "Fine grain, natural skin tones",
    "portra800": "Low light portraits, warm grain",
    "portra160nc": "Natural colour, soft contrast",
    "portra160vc": "Vivid colour for portraits",
    "portra400nc": "Neutral skin, quiet colour",
    "portra400vc": "More colour, same skin tones",
    "portra400uc": "Ultra colour, punchy everyday",
    "elite200": "Bright consumer colour",
    "elite400": "Everyday colour, more speed",
    "pro160c": "Crisp colour, cool and clean",
    "pro400h": "Airy pastels, fourth colour layer",
    "pro800z": "Night portraits, soft palette",
    "reala100": "True-to-life colour, fine grain",
    "superia100": "Clean daylight colour",
    "superia200": "Fuji greens, everyday snaps",
    "superia800": "Indoor speed, Fuji colour",
    "superia1600": "Night snaps, bold grain",
    "xtra800": "Fast all-rounder, cool shadows",
    "vista200": "Warm reds, holiday colour",
    "ultra100": "Saturated, punchy Agfa colour",
    "redscale100": "Shot through the back: red-orange",
    "velvia100": "Rich colour, softer than the 50",
    "provia100f": "Neutral slide, faithful colour",
    "provia400f": "Fast slide, clean colour",
    "provia400x": "Fast slide, finer grain",
    "astia100f": "Soft slide film for skin",
    "sensia100": "Bright consumer slide",
    "kodachrome25": "Legendary fine grain, deep reds",
    "kodachrome64": "Magazine colour of the 70s",
    "kodachrome200": "Faster Kodachrome, warm grain",
    "e100gx": "Warm, gentle Ektachrome",
    "e100vs": "Very saturated Ektachrome",
    "elitechrome200": "Everyday Kodak slide",
    "elitechrome400": "Fast Kodak slide, more grain",
    "extracolor100": "Extra saturated Kodak slide",
    "precisa100": "Cool, crisp Agfa slide",
    "elite100xpro": "Slide in C-41: wild contrast",
    "superia200xpro": "Cross-processed, yellow highlights",
    "xproslide200": "Cross-process blues and yellows",
    "tmax100": "Ultra fine grain, smooth tones",
    "tmax400": "Sharp, modern B&W",
    "tmax3200": "Push-ready, grainy nights",
    "bw400cn": "Chromogenic B&W, smooth grain",
    "hie": "Infrared: white leaves, dark skies",
    "delta100": "Fine, sharp modern B&W",
    "delta400": "Smooth grain at 400",
    "delta3200": "Pushes into the dark",
    "fp4": "Classic medium speed B&W",
    "panf50": "Slow, super fine grain",
    "hps800": "Fast classic Ilford",
    "xp2": "C-41 B&W, smooth tones",
    "acros100": "Fine grain, deep blacks",
    "neopan1600": "Fast and contrasty B&W",
    "apx25": "Very fine, slow Agfa B&W",
    "apx100": "Classic Agfa B&W",
    "retro80s": "Extended red, dark skies",
    "retro100": "Soft tonal B&W",
    "ortho25": "Orthochromatic: reds go dark",
    "ir400": "Infrared touch, glowing foliage",
    "kodak2383": "Cinema print: teal shadows",
    "kodak2393": "Cinema print, richer blacks",
    "fuji3510": "Fuji print film, soft colour",
    "fuji3513": "Fuji print for digital masters",
}


def L(style, bg, band, accent, text, num, top, word, num_s, sub="", leader=0):
    return dict(style=style, bg=bg, band=band, accent=accent, text=text, num=num,
                top=top, word=word, num_s=num_s, sub=sub, leader=leader)


W, K = (GRAY, 238), (GRAY, 22)          # white and black ink
YEL, KRED = (AMBER, 190), (RED, 150)


def portra(num, sub=""):
    return L(CAP, (GRAY, 226), YEL, KRED, K, K, "KODAK PROFESSIONAL", "PORTRA", num, sub)


def superia(num, band, word="SUPERIA", sub=""):
    return L(BAND, (GREEN, 140), band, W, W, W, "FUJIFILM", word, num, sub)


def ilford(word, num, accent, sub=""):
    return L(BAND, (GRAY, 236), (GRAY, 20), accent, W, (GRAY, 20), "ILFORD", word, num, sub, leader=52)


def rollei(word, num, band, sub=""):
    return L(BAND, K, band, (GRAY, 230), W, (GRAY, 230), "ROLLEI", word, num, sub, leader=50)


def provia(num, bg, sub):
    return L(STRIPES, bg, bg, (GREEN, 170), W, (GREEN, 190), "FUJICHROME", "PROVIA", num, sub)


LABELS = {
    "cinestill400t": L(STRIPES, (GRAY, 20), (GRAY, 20), KRED, W, W, "", "CINESTILL", "400T", "CINE"),
    "portra160": portra("160"),
    "portra800": portra("800"),
    "portra160nc": portra("160", "NC"),
    "portra160vc": portra("160", "VC"),
    "portra400nc": portra("400", "NC"),
    "portra400vc": portra("400", "VC"),
    "portra400uc": portra("400", "UC"),
    "elite200": L(BAND, YEL, (RED, 148), (RED, 148), W, K, "KODAK", "ELITE", "200", "COLOR"),
    "elite400": L(BAND, YEL, (RED, 148), (RED, 148), W, K, "KODAK", "ELITE", "400", "COLOR"),
    "pro160c": L(BAND, (GRAY, 236), (GREEN, 110), (GREEN, 110), W, (GREEN, 90), "FUJICOLOR", "PRO", "160", "160C"),
    "pro400h": L(BAND, (GRAY, 236), (GREEN, 110), (GREEN, 110), W, (GREEN, 90), "FUJICOLOR", "PRO", "400", "400H"),
    "pro800z": L(BAND, (GRAY, 236), (GREEN, 110), (GREEN, 110), W, (GREEN, 90), "FUJICOLOR", "PRO", "800", "800Z"),
    "reala100": L(STRIPES, (BLUE, 110), (BLUE, 110), (GREEN, 150), W, W, "FUJICOLOR", "REALA", "100"),
    "superia100": superia("100", (RED, 140)),
    "superia200": superia("200", (GREEN, 70)),
    "superia800": superia("800", (VIOLET, 100)),
    "superia1600": superia("1600", (GRAY, 24)),
    "xtra800": superia("800", (VIOLET, 100), "X-TRA", "SUPERIA"),
    "vista200": L(BAND, (RED, 150), (ORANGE, 175), W, K, W, "AGFA", "VISTA", "200"),
    "ultra100": L(BAND, (CRIMSON, 120), (ORANGE, 175), W, K, W, "AGFA", "ULTRA", "100", "COLOR"),
    "redscale100": L(BAND, (RED, 150), K, (ORANGE, 185), W, K, "LOMOGRAPHY", "REDSCALE", "100"),
    "velvia100": L(STRIPES, (VIOLET, 140), (VIOLET, 140), (VIOLET, 225), W, (VIOLET, 225), "FUJICHROME", "VELVIA", "100", "RVP 100"),
    "provia100f": provia("100", (BLUE, 140), "RDP III"),
    "provia400f": provia("400", (BLUE, 110), "RHP III"),
    "provia400x": provia("400", (BLUE, 80), "RXP"),
    "astia100f": L(STRIPES, (ORANGE, 150), (ORANGE, 150), (AMBER, 215), W, (AMBER, 215), "FUJICHROME", "ASTIA", "100", "RAP F"),
    "sensia100": L(STRIPES, (GREEN, 110), (GREEN, 110), (AMBER, 200), W, (AMBER, 200), "FUJICHROME", "SENSIA", "100"),
    "kodachrome25": L(BAND, YEL, (RED, 148), (RED, 148), W, K, "KODAK", "KODACHROME", "25", "KM"),
    "kodachrome64": L(BAND, YEL, (RED, 148), (RED, 148), W, K, "KODAK", "KODACHROME", "64", "KR"),
    "kodachrome200": L(BAND, YEL, (RED, 148), (RED, 148), W, K, "KODAK", "KODACHROME", "200", "KL"),
    "e100gx": L(STRIPES, K, K, YEL, W, W, "KODAK PROFESSIONAL", "EKTACHROME", "100", "E100GX"),
    "e100vs": L(STRIPES, K, K, (RED, 160), W, W, "KODAK PROFESSIONAL", "EKTACHROME", "100", "E100VS"),
    "elitechrome200": L(STRIPES, (BLUE, 120), (BLUE, 120), YEL, W, YEL, "KODAK", "ELITE", "200", "CHROME"),
    "elitechrome400": L(STRIPES, (BLUE, 95), (BLUE, 95), YEL, W, YEL, "KODAK", "ELITE", "400", "CHROME"),
    "extracolor100": L(STRIPES, (BLUE, 120), (BLUE, 120), YEL, W, W, "KODAK ELITE", "EXTRACOLOR", "100"),
    "precisa100": L(STRIPES, (BLUE, 100), (BLUE, 100), (ORANGE, 180), W, (ORANGE, 190), "AGFA", "PRECISA", "100", "CT"),
    "elite100xpro": L(BAND, K, (CRIMSON, 120), KRED, W, W, "KODAK", "ELITE", "100", "X-PRO"),
    "superia200xpro": L(BAND, (GREEN, 70), (CRIMSON, 120), W, W, W, "FUJIFILM", "SUPERIA", "200", "X-PRO"),
    "xproslide200": L(STRIPES, K, K, (VIOLET, 170), W, (VIOLET, 190), "LOMOGRAPHY", "X-PRO", "200", "SLIDE"),
    "tmax100": L(BAND, YEL, K, K, YEL, K, "KODAK PROFESSIONAL", "T-MAX", "100", "TMX", leader=50),
    "tmax400": L(BAND, YEL, K, K, YEL, K, "KODAK PROFESSIONAL", "T-MAX", "400", "TMY", leader=50),
    "tmax3200": L(BAND, YEL, K, K, YEL, K, "KODAK PROFESSIONAL", "T-MAX", "3200", "P3200", leader=50),
    "bw400cn": L(STRIPES, (GRAY, 232), (GRAY, 232), YEL, K, K, "KODAK PROFESSIONAL", "BW400CN", "400", "C-41", leader=50),
    "hie": L(STRIPES, (GRAY, 20), (GRAY, 20), KRED, W, (RED, 160), "KODAK", "INFRARED", "400", "HIE", leader=50),
    "delta100": ilford("DELTA", "100", (BLUE, 160), "PROFESSIONAL"),
    "delta400": ilford("DELTA", "400", (GREEN, 150), "PROFESSIONAL"),
    "delta3200": ilford("DELTA", "3200", (VIOLET, 150), "PROFESSIONAL"),
    "fp4": ilford("FP4", "125", KRED, "PLUS"),
    "panf50": ilford("PAN F", "50", (BLUE, 200), "PLUS"),
    "hps800": ilford("HPS", "800", (ORANGE, 170)),
    "xp2": ilford("XP2", "400", YEL, "SUPER"),
    "acros100": L(STRIPES, K, K, (GRAY, 200), W, W, "FUJIFILM", "ACROS", "100", "NEOPAN", leader=50),
    "neopan1600": L(BAND, (GRAY, 236), K, K, W, K, "FUJIFILM", "NEOPAN", "1600", "SUPER PRESTO", leader=50),
    "apx25": L(BAND, (GRAY, 236), (RED, 140), (RED, 140), W, K, "AGFA", "APX", "25", leader=50),
    "apx100": L(BAND, (GRAY, 236), (BLUE, 110), (BLUE, 110), W, K, "AGFA", "APX", "100", leader=50),
    "retro80s": rollei("RETRO", "80", (RED, 150), "80S"),
    "retro100": rollei("RETRO", "100", (AMBER, 180), "TONAL"),
    "ortho25": rollei("ORTHO", "25", (BLUE, 130), "PLUS"),
    "ir400": rollei("INFRARED", "400", (CRIMSON, 110), "IR"),
    "kodak2383": L(STRIPES, K, K, YEL, W, YEL, "KODAK", "VISION", "2383", "COLOR PRINT"),
    "kodak2393": L(STRIPES, K, K, YEL, W, YEL, "KODAK", "VISION", "2393", "PREMIER"),
    "fuji3510": L(STRIPES, (GREEN, 60), (GREEN, 60), (GRAY, 220), W, (GRAY, 220), "FUJIFILM", "ETERNA", "3510", "COLOR PRINT"),
    "fuji3513": L(STRIPES, (GREEN, 60), (GREEN, 60), (GRAY, 220), W, (GRAY, 220), "FUJIFILM", "ETERNA", "3513", "DI"),
}
