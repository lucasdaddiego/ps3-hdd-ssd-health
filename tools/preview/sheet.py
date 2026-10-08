import sys
from PIL import Image
out, files = sys.argv[1], sys.argv[2:]
sheet = Image.new('RGB', (1920, 1080), (40, 40, 40))
for i, f in enumerate(files[:4]):
    im = Image.open(f).convert('RGB').resize((956, 538), Image.LANCZOS)
    sheet.paste(im, ((i % 2) * 964, (i // 2) * 542))
sheet.save(out)
