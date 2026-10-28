/* SPDX-License-Identifier: MIT */
#include "core.h"

void TextInit(TEXT *t, CHAR8 *data, UINTN cap) {
    t->data=data; t->cap=cap; t->len=0; t->failed=(BOOLEAN)(cap==0);
    if (cap) data[0]=0;
}
static void CharPut(TEXT *t, CHAR8 ch) {
    if(t->failed) return;
    if(t->len+1>=t->cap) {t->failed=TRUE; return;}
    t->data[t->len++]=ch; t->data[t->len]=0;
}
void TextPut(TEXT *t, const CHAR8 *s) {while(*s) CharPut(t,*s++);}
void TextDec(TEXT *t, UINT64 value) {
    CHAR8 reverse[21]; UINTN n=0;
    do {reverse[n++]=(CHAR8)('0'+value%10);value/=10;} while(value);
    while(n) CharPut(t,reverse[--n]);
}
void TextHex(TEXT *t, UINT64 value, UINTN digits) {
    static const CHAR8 alphabet[]="0123456789abcdef";
    if(digits>16) {t->failed=TRUE;return;}
    TextPut(t,"\"0x");
    while(digits) {--digits; CharPut(t,alphabet[(value>>(digits*4))&15]);}
    CharPut(t,'"');
}
void TextBytes(TEXT *t, const UINT8 *p, UINTN length) {
    static const CHAR8 alphabet[]="0123456789abcdef";
    UINTN i; CharPut(t,'"');
    for(i=0;i<length;i++) {CharPut(t,alphabet[p[i]>>4]);CharPut(t,alphabet[p[i]&15]);}
    CharPut(t,'"');
}
UINT16 Read16(const UINT8 *p) {return (UINT16)(p[0]|((UINT16)p[1]<<8));}
UINT32 Read32(const UINT8 *p) {return (UINT32)p[0]|((UINT32)p[1]<<8)|((UINT32)p[2]<<16)|((UINT32)p[3]<<24);}
UINT64 Read64(const UINT8 *p) {return (UINT64)Read32(p)|((UINT64)Read32(p+4)<<32);}
BOOLEAN BytesEqual(const VOID *a, const VOID *b, UINTN n) {
    const UINT8 *p=a,*q=b; UINTN i;
    for(i=0;i<n;i++) if(p[i]!=q[i]) return FALSE;
    return TRUE;
}
BOOLEAN DpMeasure(const UINT8 *p, UINTN cap, UINTN *length, BOOLEAN *multi) {
    UINTN off=0,n,nodes=0; *length=0; *multi=FALSE;
    if(!p||cap>PROBE_DP_MAX) return FALSE;
    while(off<cap && ++nodes<=256) {
        if(cap-off<4) return FALSE;
        n=Read16(p+off+2);
        if(n<4 || n>cap-off) return FALSE;
        if(p[off]==0x7f) {
            if(n!=4) return FALSE;
            if(p[off+1]==0xff) {*length=off+4;return TRUE;}
            if(p[off+1]!=1) return FALSE;
            *multi=TRUE;
        }
        off+=n;
    }
    return FALSE;
}
BOOLEAN DpParent(const UINT8 *parent, UINTN plen, const UINT8 *child, UINTN clen) {
    UINTN p=0,c=0; BOOLEAN pm=FALSE,cm=FALSE;
    if(!DpMeasure(parent,plen,&p,&pm)||!DpMeasure(child,clen,&c,&cm)) return FALSE;
    if(pm||cm||p!=plen||c!=clen||p<=4||c<p) return FALSE;
    return BytesEqual(parent,child,p-4);
}
BOOLEAN DpHasUsb(const UINT8 *path, UINTN length) {
    UINTN measured=0,offset=0;BOOLEAN multi=FALSE;
    if(!DpMeasure(path,length,&measured,&multi)||multi||measured!=length) return FALSE;
    while(offset+4<length) {
        UINTN n=Read16(path+offset+2);
        if(path[offset]==3 && ((path[offset+1]==5&&n==6)||
            (path[offset+1]==15&&n==11)||(path[offset+1]==16&&n>=10))) return TRUE;
        offset+=n;
    }
    return FALSE;
}
BOOLEAN ExactTarget(const UINT8 *c, UINTN length) {
    return (BOOLEAN)(length>=64 && Read16(c)==0x10de && Read16(c+2)==PROBE_DEVICE &&
        (c[14]&0x7f)==0 && c[11]==3 && Read16(c+44)==PROBE_SUBVENDOR && Read16(c+46)==PROBE_SUBDEVICE);
}
BOOLEAN RomPcirValid(const UINT8 *rom, UINTN size, UINT16 vendor, UINT16 device) {
    UINTN off=0,image_len,pcir,images=0; UINT8 indicator;
    if(!rom||size<512||size>PROBE_ROM_MAX) return FALSE;
    while(++images<=64) {
        if(size-off<0x1a || rom[off]!=0x55 || rom[off+1]!=0xaa) return FALSE;
        pcir=Read16(rom+off+0x18);
        if(pcir<0x1a || pcir>size-off || size-off-pcir<0x18) return FALSE;
        pcir+=off;
        if(!BytesEqual(rom+pcir,"PCIR",4)||Read16(rom+pcir+4)!=vendor||Read16(rom+pcir+6)!=device) return FALSE;
        if(Read16(rom+pcir+0x0a)<0x18) return FALSE;
        image_len=(UINTN)Read16(rom+pcir+0x10)*512;
        if(image_len<512||image_len>size-off||pcir-off+0x18>image_len) return FALSE;
        indicator=rom[pcir+0x15];
        if(indicator&0x80) return TRUE;
        off+=image_len;
        if(off>=size) return FALSE;
    }
    return FALSE;
}

static UINT32 Rotate(UINT32 x, UINTN n) {return (x>>n)|(x<<(32-n));}
static const UINT32 K[64]={
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
static void ShaBlock(UINT32 h[8], const UINT8 block[64]) {
    UINT32 w[64],a,b,c,d,e,f,g,hh,s0,s1,t1,t2; UINTN i;
    for(i=0;i<16;i++) w[i]=((UINT32)block[i*4]<<24)|((UINT32)block[i*4+1]<<16)|((UINT32)block[i*4+2]<<8)|block[i*4+3];
    for(i=16;i<64;i++) {s0=Rotate(w[i-15],7)^Rotate(w[i-15],18)^(w[i-15]>>3);s1=Rotate(w[i-2],17)^Rotate(w[i-2],19)^(w[i-2]>>10);w[i]=w[i-16]+s0+w[i-7]+s1;}
    a=h[0];b=h[1];c=h[2];d=h[3];e=h[4];f=h[5];g=h[6];hh=h[7];
    for(i=0;i<64;i++) {s1=Rotate(e,6)^Rotate(e,11)^Rotate(e,25);t1=hh+s1+((e&f)^((~e)&g))+K[i]+w[i];s0=Rotate(a,2)^Rotate(a,13)^Rotate(a,22);t2=s0+((a&b)^(a&c)^(b&c));hh=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2;}
    h[0]+=a;h[1]+=b;h[2]+=c;h[3]+=d;h[4]+=e;h[5]+=f;h[6]+=g;h[7]+=hh;
}
void Sha256(const UINT8 *p, UINTN n, UINT8 result[32]) {
    UINT32 h[8]={0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    UINT8 block[64]; UINTN remaining=n,i,j; UINT64 bits=(UINT64)n*8;
    while(remaining>=64) {ShaBlock(h,p);p+=64;remaining-=64;}
    for(i=0;i<64;i++) block[i]=0;
    for(i=0;i<remaining;i++) block[i]=p[i];
    block[remaining]=0x80;
    if(remaining>=56) {ShaBlock(h,block);for(i=0;i<64;i++) block[i]=0;}
    for(i=0;i<8;i++) block[63-i]=(UINT8)(bits>>(8*i));
    ShaBlock(h,block);
    for(i=0;i<8;i++) for(j=0;j<4;j++) result[4*i+j]=(UINT8)(h[i]>>(24-8*j));
}
