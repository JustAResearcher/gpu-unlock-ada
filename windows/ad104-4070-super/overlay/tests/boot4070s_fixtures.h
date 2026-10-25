/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../sha256.h"

enum { BOOT_GSP=2, BOOT_WINDOWS=5, BOOT_FILE_COUNT=6 };
static const UINTN BootSizes[BOOT_FILE_COUNT]={57672,64000,84277400,36864,84,3086680};
static const char *const BootNames[5]={
  "booter_load_ad102.bin","v67-ad102-610.43.03-four-plm.bin","gsp_ga10x.bin",
  "gsp_boot_ad102.bin","gsp_boot_desc_ad102.bin"
};
static const char *const BootFullNames[5]={
  "gpu-unlock/610.43.03/booter_load_ad102.bin","gpu-unlock/610.43.03/v67-ad102-610.43.03-four-plm.bin",
  "gpu-unlock/610.43.03/gsp_ga10x.bin","gpu-unlock/610.43.03/gsp_boot_ad102.bin",
  "gpu-unlock/610.43.03/gsp_boot_desc_ad102.bin"
};
static const char BootWindowsHash[]="2d60a2203865122dad22772136b3d905167d9dcf926d602bb5f48b3b6668abec";
static UINT8 *BootBytes[BOOT_FILE_COUNT];

static void BootFixtureError(const char *path,const char *reason) {
  fprintf(stderr,"FIXTURE_ERROR: %s: %s\n",path,reason);exit(2);
}
static UINT8 *BootReadFixture(const char *path,UINTN expected) {
  FILE *f=fopen(path,"rb");
  if(!f)BootFixtureError(path,"cannot open");
  UINT8 *p=malloc(expected);
  if(!p)BootFixtureError(path,"allocation failed");
  if(fread(p,1,expected,f)!=expected||fgetc(f)!=EOF)BootFixtureError(path,"wrong size or incomplete read");
  if(fclose(f))BootFixtureError(path,"close failed");
  return p;
}
static void BootBindAssets(void) {
  for(UINTN i=0;i<5;i++)GuAssets[i]=(GU_ASSET){BootFullNames[i],{BootSizes[i],BootBytes[i]}};
}
static void BootLoadFixtures(int argc,char **argv) {
  if(argc<3||argc>4)BootFixtureError("arguments","expected WindowsImage AssetsDirectory [case-name]");
  BootBytes[BOOT_WINDOWS]=BootReadFixture(argv[1],BootSizes[BOOT_WINDOWS]);
  for(UINTN i=0;i<5;i++) {
    char path[4096];int n=snprintf(path,sizeof(path),"%s/%s",argv[2],BootNames[i]);
    if(n<0||(size_t)n>=sizeof(path))BootFixtureError(argv[2],"path too long");
    BootBytes[i]=BootReadFixture(path,BootSizes[i]);
  }
  /* Real production profile.c and sha256.c, never a pointer-only stub. */
  BootBindAssets();
  if(!GuAd104AssetsValid())BootFixtureError(argv[2],"production five-asset SHA/name/size validation failed");
  if(!GuHashMatches(BootBytes[BOOT_WINDOWS],BootSizes[BOOT_WINDOWS],BootWindowsHash))
    BootFixtureError(argv[1],"current Windows image SHA256 mismatch");
  memset(GuAssets,0,sizeof(GuAssets));
  puts("FIXTURE_OK: five production asset pins and current Windows SHA256 verified");
}
static void BootFreeFixtures(void) {
  for(UINTN i=0;i<BOOT_FILE_COUNT;i++)free(BootBytes[i]);
}
