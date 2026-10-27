/* SPDX-License-Identifier: MIT
 * Freestanding compiler helpers, built without optimization to avoid recursion.
 */
#include <Uefi.h>
VOID *memcpy(VOID *destination,const VOID *source,UINTN n) {
    UINT8 *d=destination;const UINT8 *s=source;UINTN i;
    for(i=0;i<n;i++) d[i]=s[i];return destination;
}
VOID *memset(VOID *destination,int value,UINTN n) {
    UINT8 *d=destination;UINTN i;
    for(i=0;i<n;i++) d[i]=(UINT8)value;return destination;
}
