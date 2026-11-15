"""Composition preview at native resolutions, NOT a console/GPU screenshot.

Uses a desktop font as a stand-in for the console fonts. The real renderer is
in source/main.c. This preview documents spacing, colors and artwork placement.
"""
from pathlib import Path
from base64 import b64encode
from html import escape
import math

ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT/'build'/'preview'
OUT.mkdir(parents=True,exist_ok=True)
BG='#05111c'; CYAN='#29bedb'; WHITE='#e8f0f8'; SECOND='#91a8bf'; BORDER='#285370'

def datauri(path):
    return 'data:image/'+('jpeg' if path.suffix.lower()=='.jpg' else 'png')+';base64,'+b64encode(path.read_bytes()).decode()

def rect(x,y,w,h,c,r=0):
    return f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="{r}" fill="{c}"/>'
def circle(x,y,r,c):
    return f'<circle cx="{x}" cy="{y}" r="{r}" fill="{c}"/>'
def text(s,x,y,size=12,c=WHITE,anchor='start',weight=400):
    return f'<text x="{x}" y="{y+size*.86}" font-family="Source Sans 3,Segoe UI,sans-serif" font-size="{size}" font-weight="{weight}" text-anchor="{anchor}" fill="{c}">{escape(s)}</text>'
def line(x,y,x2,y2,c=CYAN,width=1.6):
    return f'<path d="M{x} {y}L{x2} {y2}" fill="none" stroke="{c}" stroke-width="{width}"/>'
def note(x,y,s=1,c=SECOND):
    return ''.join([line(x+5*s,y+3*s,x+5*s,y+13*s,c,1.6*s),line(x+13*s,y+s,x+13*s,y+11*s,c,1.6*s),line(x+5*s,y+3*s,x+13*s,y+s,c,2.5*s),circle(x+3*s,y+13*s,2.5*s,c),circle(x+11*s,y+11*s,2.5*s,c)])
def key(k,label,x,y,w=12):
    if len(k)==1:
        a=circle(x+6,y+6,6,WHITE)+text(k,x+6,y,10.2,BG,'middle')
    else:
        a=rect(x,y,w,12,SECOND,2)+rect(x+1,y+1,w-2,10,BG,1)+text(k,x+w/2,y+1,8.1,WHITE,'middle')
    return a+text(label,x+w+4,y,9.6)
def panel(w):
    return rect(0,0,w,240,'#07101d')+rect(3,3,w-6,234,BORDER,8)+rect(4,4,w-8,232,'#0c1e2e',7)+rect(8,8,w-16,224,BG,5)

logo=datauri(ROOT/'assets'/'shield-logo.png')
top=panel(400)+rect(8,42,384,1,CYAN)
top+=f'<image href="{logo}" x="13" y="7" width="35.2" height="35.2"/>'
top+=text('SHIELD',51,9,19.2)+text('~ DJ',115,9,19.2,CYAN)+text('STANDARD / by Yanis',52,29,9.3,SECOND)
top+=rect(293,20,24,13,CYAN)+rect(296,23,18,7,'#07101d')+rect(298,24,10,5,CYAN)+rect(318,23,3,7,CYAN)+text('70%',326,19,12.9)
top+=rect(354,17,1,18,BORDER)+text('09:13',384,19,11.4,WHITE,'end')
top+=circle(82,119,66,'#1e3e54')+circle(82,119,64.5,CYAN)+circle(82,119,63,'#07101d')
sample=Path('C:/Users/Shiel/OneDrive/Images/callie-v0-5vwbez2zd27e1.jpg')
if sample.exists():
    top+='<defs><clipPath id="disc"><circle cx="82" cy="119" r="62.2"/></clipPath></defs>'
    top+=f'<image href="{datauri(sample)}" x="19.3" y="56.3" width="125.4" height="125.4" preserveAspectRatio="xMidYMid slice" clip-path="url(#disc)"/>'
top+=circle(82,119,8.5,SECOND)+circle(82,119,7,'#07101d')
top+=note(164,57,.8,CYAN)+text('EN LECTURE',184,56,13.2,CYAN)
top+=text('Inkcoming!',164,76,18.6)+text('Wet Floor',164,103,12.9,SECOND)+text('Splatoon OST',164,122,11.7,SECOND)
top+=rect(164,160,218,6,'#193041',3)+rect(164,160,97,6,CYAN,3)+text('01:23',164,170,11.1)+text('03:06',382,170,11.1,WHITE,'end')
top+=rect(14,187,372,1,BORDER)
for i,label in enumerate(['Shuffle ON','Pitch 1.00x','Bass +9 dB','Reverb OFF']):
    x=16+i*95
    if i: top+=rect(x-6,194,.7,19,BORDER)
    if i==0:
        for pts in [(x,198,x+4,198),(x+4,198,x+11,207),(x+11,207,x+16,207),(x,207,x+4,207),(x+4,207,x+11,198),(x+11,198,x+16,198)]: top+=line(*pts)
        for y in [198,207]: top+=line(x+13,y-3,x+16,y)+line(x+13,y+3,x+16,y)
    elif i==1:
        for j,h in enumerate([6,12,18,12,6]): top+=rect(x+j*3,203-h/2,1.8,h,CYAN)
    elif i==2:
        for j,h in enumerate([3,6,5,2]):
            for k in range(h): top+=rect(x+j*4,210-k*3,2.5,2,WHITE)
    else:
        for j in range(12): top+=circle(x+8+7*math.sin(j*math.pi/6),203-7*math.cos(j*math.pi/6),1,WHITE)
    top+=text(label,x+21,198,9.9)
top+=text('SD  2.59 / 7.23 Go',16,218,12,WHITE)+text('LUM 1/5',384,219,10.8,SECOND,'end')

bottom=panel(320)+rect(16,14,7,5,CYAN,1)+rect(16,16,14,10,CYAN,1.5)+rect(17.3,17.5,11.4,7,BG,.5)
bottom+=text('BIBLIOTHÈQUE',38,11,16.2,CYAN)+text('sdmc:/Music/Splatoon',16,32,10.5,SECOND)+text('1/12',303,32,10.5,SECOND,'end')+rect(13,48,294,.8,BORDER)
for i,(title,duration) in enumerate(zip(['Inkcoming!','Calamari Inkantation','Now or Never!','Maritime Memory','Splattack!'],['03:06','02:14','03:33','02:28','02:51'])):
    y=54+22*i
    if i==0: bottom+=rect(12,y,296,21,'#0991b3',2)+rect(12,y,3,21,CYAN,1)
    bottom+=note(19,y+3,.85,WHITE if i==0 else SECOND)+text(title,41,y+3,12.3)+text(duration,299,y+4,10.8,WHITE if i==0 else SECOND,'end')
    if i<4: bottom+=rect(16,y+21,288,1,'#183044')
bottom+=rect(14,166,292,.8,BORDER)
for k,label,x,y,w in [('A','Pause',16,170,12),('B','Stop',96,170,12),('L/R','Piste',159,170,22),('X','Shuffle',238,170,12),('Y','Bass',16,187,12),('SELECT','Reverb',78,187,35),('<>','Pitch',166,187,20),('START','Quitter',236,187,31)]: bottom+=key(k,label,x,y,w)
bottom+=text('L+R Verrou   Haut/Bas Luminosite',16,203,8.7,SECOND)
bottom+=rect(14,215,292,.8,BORDER)+text('BAT 70%',16,218,10.2,SECOND)+text('TEMP 29 C',158,218,10.2,SECOND)

def svg(w,h,body):
    return f'<svg xmlns="http://www.w3.org/2000/svg" width="{w}" height="{h}" viewBox="0 0 {w} {h}">{body}</svg>'
(OUT/'top.svg').write_text(svg(400,240,top),encoding='utf-8')
(OUT/'bottom.svg').write_text(svg(320,240,bottom),encoding='utf-8')
combined=rect(0,0,864,1150,'#030b12')+text('SHIELD DJ / STANDARD',32,20,26,CYAN)+text('Aperçu de composition — police simulée, pas une capture console',32,57,15,SECOND)
combined+='<g transform="translate(32,100) scale(2)">'+top+'</g>'
combined+='<g transform="translate(112,610) scale(2)">'+bottom+'</g>'
combined+=text('Pochette et métadonnées d’exemple. Le lecteur utilise celles de tes fichiers.',32,1120,13,SECOND)
(OUT/'ui-composition.svg').write_text(svg(864,1150,combined),encoding='utf-8')
print(OUT/'ui-composition.svg')
