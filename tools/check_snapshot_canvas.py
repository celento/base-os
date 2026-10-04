"""Check ordinary snap-probe screenshots, including the previously partial footer."""
from pathlib import Path
import json
import numpy as np
from PIL import Image
COLORS=((216,58,58),(242,201,76),(42,167,200),(168,74,192))
def check(directory):
    images=sorted(directory.glob('*.png'))
    assert images
    canvas=None
    for path in images:
        pixels=np.array(Image.open(path).convert('RGB'))
        if canvas is None:
            ys,xs=np.where(np.all(pixels==COLORS[0],axis=2))
            for y,x in zip(ys,xs):
                for scale in (1,2,3,4):
                    if y+100*scale>pixels.shape[0] or x+160*scale>pixels.shape[1]:continue
                    if x and tuple(pixels[y,x-1])==COLORS[0]:continue
                    if y and tuple(pixels[y-1,x])==COLORS[0]:continue
                    if all(tuple(pixels[y+2*scale,x+(8*i+2)*scale])==color for i,color in enumerate(COLORS)):
                        canvas=(int(x),int(y),scale);break
                if canvas:break
            assert canvas,path
        x,y,scale=canvas
        phase=0
        for bit in range(32):
            color=tuple(pixels[y+14*scale,x+(bit*5+2)*scale])
            assert color in ((0,0,0),(255,255,255)),(path,bit,color)
            if color==(255,255,255):phase|=1<<bit
        footer=COLORS[1] if phase==1 else (63,163,91)
        actual=pixels[y+94*scale:y+100*scale,x:x+160*scale]
        assert np.all(actual==footer),(path,phase,np.unique(actual.reshape(-1,3),axis=0))
    return {'directory':str(directory),'screenshots':len(images),'canvas':canvas,'footer_pixels_per_screenshot':160*6*canvas[2]*canvas[2],'passed':True}
if __name__=='__main__':
    import sys
    assert len(sys.argv)>1, "Usage: check_snapshot_canvas.py SCREENSHOT_DIRECTORY [...]"
    result=[check(Path(path)) for path in sys.argv[1:]]
    print(json.dumps(result,indent=2))
