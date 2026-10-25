#ifndef GU_OWNERSHIP_H
#define GU_OWNERSHIP_H
#include "port.h"
#include <Protocol/DriverBinding.h>
#define GU_MAX_DISPLAY_CHILDREN 8
typedef struct {
  EFI_HANDLE handle;
  EFI_GUID protocol;
  VOID *gop;
  UINT64 framebuffer;
  UINTN framebuffer_size;
  UINT8 path_sha256[32];
} GU_DISPLAY_CHILD;
typedef struct {
  EFI_HANDLE agent,image;
  VOID *base;
  UINT64 size;
  UINT32 version;
  EFI_DRIVER_BINDING_STOP stop;
  UINTN child_count;
  GU_DISPLAY_CHILD children[GU_MAX_DISPLAY_CHILDREN];
} GU_RELEASE_OWNER;
EFI_STATUS GuClaimController(VOID);
EFI_STATUS GuFindReleaseOwner(EFI_SYSTEM_TABLE *Table,GU_RELEASE_OWNER *Owner,BOOLEAN Log);
BOOLEAN GuSameReleaseOwner(const GU_RELEASE_OWNER *A,const GU_RELEASE_OWNER *B);
/* Caller holds TPL_NOTIFY through fresh resolution/validation and any BME clear. */
EFI_STATUS GuRefreshTargetPci(VOID);
#endif
