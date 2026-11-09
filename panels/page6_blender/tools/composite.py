import re,sys
from PIL import Image,ImageDraw,ImageFont
svg=open(sys.argv[1]).read(); orig=sys.argv[2]; body=sys.argv[3]; out=sys.argv[4]
INK=(18,18,20,255)
B=Image.open(body).convert('RGB').resize((1280,558),Image.LANCZOS)
im=Image.new('RGB',(1280,628)); im.paste(B,(0,70))
im.paste(Image.open(orig).convert('RGB').crop((0,0,1280,70)),(0,0))   # header kept exactly as given
SC=4
tw='/tmp/claude-0/-home-claude/10398005-bc7b-58eb-90a4-5e123a946874/scratchpad/fonts/tw.ttf'
for m in re.finditer(r'<text([^>]*)>([^<]*)</text>',svg):
    a,t=m.groups(); 
    if 'class="g"' in a or 'id="txt_' in a or 'btn' in a: continue
    fs=float(re.search(r'font-size="([\d.]+)"',a).group(1)); t=t.replace('&lt;','<').replace('&gt;','>').strip()
    if not t: continue
    f=ImageFont.truetype(tw,int(fs*SC))
    L=Image.new('RGBA',(int(f.getlength(t))+40,int(fs*SC*1.6)),(0,0,0,0)); d=ImageDraw.Draw(L)
    d.text((20,int(fs*SC*0.25)),t,font=f,fill=INK)
    asc=int(fs*SC*0.25)+f.getmetrics()[0]  # baseline y in layer
    tr=re.search(r'translate\(([\d.]+),([\d.]+)\) rotate\(-90\)',a)
    if tr:
        x,y=float(tr.group(1)),float(tr.group(2)); L=L.rotate(90,expand=True)
        w,h=L.size
        L=L.resize((w//SC,h//SC),Image.LANCZOS)
        im.paste(L,(int(x-asc/SC),int(y-(h-20)/SC)),L); continue
    x=float(re.search(r' x="([\d.]+)"',a).group(1)); y=float(re.search(r' y="([\d.]+)"',a).group(1))
    if y<70: continue
    w=f.getlength(t)/SC
    x0=x-w/2 if 'text-anchor="middle"' in a else x
    L=L.resize((L.width//SC,L.height//SC),Image.LANCZOS)
    im.paste(L,(int(x0-20/SC),int(y-asc/SC)),L)
im.save(out); print(out)
