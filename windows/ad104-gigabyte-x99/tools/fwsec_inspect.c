/* SPDX-License-Identifier: GPL-2.0-only */
/* Offline parser. No PCI/MMIO/UEFI services or GPU execution. */
#include "../sha256.h"
#include <stdio.h>
#include <stdlib.h>
#include "../vendor/vbios_610.h"
static void digest(const u8 *data,size_t size) {
  UINT8 hash[32];GuSha256(data,size,hash);
  for(unsigned i=0;i<32;i++)printf("%02x",hash[i]);
}
int main(int argc,char **argv) {
  if(argc!=2)return 2;
  FILE *f=fopen(argv[1],"rb");if(!f)return 2;
  u8 *rom=malloc(1048576);if(!rom){fclose(f);return 2;}
  if(fread(rom,1,1048576,f)!=1048576||fgetc(f)!=EOF){fclose(f);free(rom);return 2;}fclose(f);
  struct gpu_unlock_fwsec fw;int result=gpu_unlock_vbios_fwsec(rom,1048576,&fw);
  if(result){free(rom);return 1;}
  printf("{\"parser_result\":0,\"physical_prom_sha256\":\"");digest(rom,1048576);
  printf("\",\"fwsec_image_sha256\":\"");digest(rom+fw.image_off,fw.image_size);
  printf("\",\"fwsec\":{");
#define FIELD(name) printf("\"" #name "\":%u,",(unsigned)fw.name)
  FIELD(image_off);FIELD(image_size);FIELD(imem_pa);FIELD(imem_size);FIELD(imem_va);
  FIELD(dmem_pa);FIELD(data_off);FIELD(dmem_size);FIELD(pkc_off);FIELD(interface_off);
  FIELD(ucode_id);FIELD(engine_mask);FIELD(sig_count);FIELD(sig_versions);FIELD(sig_off);
  printf("\"sig_bytes\":%u}}\n",(unsigned)fw.sig_bytes);free(rom);return 0;
}
