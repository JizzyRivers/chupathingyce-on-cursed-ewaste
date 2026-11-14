import struct, zlib, sys
def conv(src, dst, step=2):
    d=open(src,'rb').read()
    off=struct.unpack_from('<I',d,10)[0]; w,h=struct.unpack_from('<ii',d,18); bpp=struct.unpack_from('<H',d,28)[0]
    bp=bpp//8; stride=(w*bp+3)&~3; flip=h>0; h=abs(h)
    rows=[]
    for y in range(0,h,step):
        sy=h-1-y if flip else y
        r=d[off+sy*stride: off+sy*stride+w*bp]
        out=bytearray(b'\x00')
        for x in range(0,w,step):
            b,g,rr=r[x*bp],r[x*bp+1],r[x*bp+2]; out+=bytes((rr,g,b))
        rows.append(bytes(out))
    W=len(range(0,w,step)); H=len(rows)
    def chunk(t,data): return struct.pack('>I',len(data))+t+data+struct.pack('>I',zlib.crc32(t+data)&0xffffffff)
    png=b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',W,H,8,2,0,0,0))+chunk(b'IDAT',zlib.compress(b''.join(rows)))+chunk(b'IEND',b'')
    open(dst,'wb').write(png); print(src,w,h,bpp)
for a in sys.argv[1:]: conv(a, a[:-4]+'.png')
