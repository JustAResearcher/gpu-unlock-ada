#include "runtime.h"
void *memcpy(void *destination,const void *source,size_t size) {
  volatile unsigned char *d=destination;const volatile unsigned char *s=source;
  for(size_t i=0;i<size;i++)d[i]=s[i];return destination;
}
void *memset(void *destination,int value,size_t size) {
  volatile unsigned char *d=destination;for(size_t i=0;i<size;i++)d[i]=(unsigned char)value;
  return destination;
}
void *memchr(const void *source,int value,size_t size) {
  const unsigned char *s=source;for(size_t i=0;i<size;i++)if(s[i]==(unsigned char)value)return (void *)(s+i);
  return NULL;
}
int memcmp(const void *left,const void *right,size_t size) {
  const unsigned char *a=left,*b=right;
  for(size_t i=0;i<size;i++)if(a[i]!=b[i])return (int)a[i]-(int)b[i];return 0;
}
int strcmp(const char *left,const char *right) {
  const unsigned char *a=(const unsigned char *)left,*b=(const unsigned char *)right;
  while(*a && *a==*b){a++;b++;}return (int)*a-(int)*b;
}
size_t strlen(const char *s) {size_t n=0;while(s[n])n++;return n;}
