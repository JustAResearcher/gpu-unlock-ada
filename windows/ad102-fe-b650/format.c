#include "format.h"
#include <stdint.h>
typedef struct {char *data;size_t capacity,used;} OUTPUT;
static void put(OUTPUT *o,char c) {if(o->used+1<o->capacity)o->data[o->used]=c;o->used++;}
size_t GuFormatV(char *destination,size_t capacity,const char *format,va_list args) {
  OUTPUT o={destination,capacity,0};
  if(!format || (capacity && !destination))return SIZE_MAX;
  while(*format) {
    if(*format!='%'){put(&o,*format++);continue;}format++;
    unsigned width=0,length=0,base=10;int alternate=0,zero=0,negative=0;
    while(*format=='#' || *format=='0'){if(*format=='#')alternate=1;else zero=1;format++;}
    while(*format>='0' && *format<='9') {
      width=width*10+(unsigned)(*format++-'0');if(width>128)goto invalid;
    }
    if(*format=='l'){length=1;format++;if(*format=='l'){length=2;format++;}}
    else if(*format=='z'){length=3;format++;}
    char kind=*format;if(!kind)goto invalid;format++;
    if(kind=='s') {
      if(length || alternate || zero || width)goto invalid;
      const char *s=va_arg(args,const char *);if(!s)s="(null)";while(*s)put(&o,*s++);continue;
    }
    if(kind=='c' || kind=='%') {
      if(length || alternate || zero || width)goto invalid;
      put(&o,kind=='%'?'%':(char)va_arg(args,int));continue;
    }
    uint64_t value;
    if(kind=='d' || kind=='i') {
      int64_t signed_value;
      if(length==2)signed_value=va_arg(args,long long);
      else if(length==1)signed_value=va_arg(args,long);
      else if(length==3)signed_value=va_arg(args,ptrdiff_t);
      else signed_value=va_arg(args,int);
      negative=signed_value<0;value=negative?0-(uint64_t)signed_value:(uint64_t)signed_value;
    } else if(kind=='u' || kind=='x' || kind=='X') {
      if(length==2)value=va_arg(args,unsigned long long);
      else if(length==1)value=va_arg(args,unsigned long);
      else if(length==3)value=va_arg(args,size_t);
      else value=va_arg(args,unsigned int);
      if(kind!='u')base=16;
    } else goto invalid;
    char digits[32];unsigned n=0;int prefix=alternate && base==16 && value;
    const char *alphabet=kind=='X'?"0123456789ABCDEF":"0123456789abcdef";
    do {digits[n++]=alphabet[value%base];value/=base;}while(value);
    unsigned total=n+(negative?1U:0U)+(prefix?2U:0U),pad=width>total?width-total:0;
    if(!zero)while(pad){put(&o,' ');pad--;}
    if(negative)put(&o,'-');if(prefix){put(&o,'0');put(&o,kind=='X'?'X':'x');}
    if(zero)while(pad){put(&o,'0');pad--;}
    while(n)put(&o,digits[--n]);
  }
  if(capacity)destination[o.used<capacity?o.used:capacity-1]=0;return o.used;
invalid:
  if(capacity)destination[0]=0;return SIZE_MAX;
}
