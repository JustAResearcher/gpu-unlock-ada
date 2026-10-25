/* SPDX-License-Identifier: GPL-2.0-only */
/* Exercise the production capture writer with fake EFI file services only. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/app_io.c"

GU_PLATFORM Gu;
BOOLEAN GuSafeToReturn(VOID){return TRUE;}

#if GU_ENABLE_UNLOCK
int main(void) {
  assert(GuCaptureOpen()==EFI_ACCESS_DENIED);
  assert(GuCaptureWrite("test")==EFI_ACCESS_DENIED);
  assert(GuCaptureClose()==EFI_ACCESS_DENIED);
  puts("experimental capture entry points denied");return 0;
}
#else
static EFI_FILE_PROTOCOL run_file,report_file,capture_file;
static EFI_SYSTEM_TABLE system_table;
static EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL console_output;
static UINT8 payload[CAPTURE_LIMIT_BYTES+CAPTURE_BUFFER_BYTES];
static UINTN payload_bytes;
static unsigned writes,flushes,closes,creates,console_calls,other_closes;
static BOOLEAN existing,nonempty,short_write,write_error,flush_error,close_error,create_error;

static EFI_STATUS EFIAPI fake_output(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *Self,CHAR16 *Text) {
  (void)Self;(void)Text;console_calls++;return EFI_DEVICE_ERROR;
}
static EFI_STATUS EFIAPI fake_close(EFI_FILE_PROTOCOL *Self) {
  if(Self==&capture_file){closes++;return close_error?EFI_ABORTED:EFI_SUCCESS;}
  assert(Self==&run_file || Self==&report_file);other_closes++;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI fake_open(EFI_FILE_PROTOCOL *Self,EFI_FILE_PROTOCOL **Out,
  CHAR16 *Name,UINT64 Mode,UINT64 Attributes) {
  assert(Self==&run_file && !Attributes);
  static const CHAR16 expected[]=L"TOPOLOGY.TXT";
  assert(!memcmp(Name,expected,sizeof(expected)));*Out=NULL;
  if(!(Mode&EFI_FILE_MODE_CREATE)) {
    assert(Mode==EFI_FILE_MODE_READ);
    if(existing){*Out=&capture_file;return EFI_SUCCESS;}
    return EFI_NOT_FOUND;
  }
  assert(Mode==(EFI_FILE_MODE_CREATE|EFI_FILE_MODE_READ|EFI_FILE_MODE_WRITE));
  creates++;*Out=&capture_file;return create_error?EFI_DEVICE_ERROR:EFI_SUCCESS;
}
static EFI_STATUS EFIAPI fake_info(EFI_FILE_PROTOCOL *Self,EFI_GUID *Guid,UINTN *Bytes,VOID *Buffer) {
  assert(Self==&capture_file && *Bytes>=sizeof(EFI_FILE_INFO));(void)Guid;
  memset(Buffer,0,sizeof(EFI_FILE_INFO));EFI_FILE_INFO *info=Buffer;
  info->Size=sizeof(*info);info->FileSize=nonempty?10:0;*Bytes=sizeof(*info);
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI fake_write(EFI_FILE_PROTOCOL *Self,UINTN *Bytes,VOID *Buffer) {
  assert(Self==&capture_file && *Bytes<=CAPTURE_BUFFER_BYTES);writes++;
  if(write_error)return EFI_WRITE_PROTECTED;
  if(short_write){*Bytes=0;return EFI_SUCCESS;}
  assert(payload_bytes+*Bytes<=sizeof(payload));
  memcpy(payload+payload_bytes,Buffer,*Bytes);payload_bytes+=*Bytes;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI fake_flush(EFI_FILE_PROTOCOL *Self) {
  assert(Self==&capture_file);flushes++;return flush_error?EFI_DEVICE_ERROR:EFI_SUCCESS;
}
static VOID setup(VOID) {
  Run=&run_file;Report=&report_file;Root=Parent=NULL;LogFault=FALSE;
  Table=&system_table;system_table.ConOut=&console_output;console_output.OutputString=fake_output;
  run_file.Open=fake_open;run_file.Close=report_file.Close=capture_file.Close=fake_close;
  capture_file.GetInfo=fake_info;capture_file.Write=fake_write;capture_file.Flush=fake_flush;
  assert(!Capture);CaptureFault=EFI_SUCCESS;CaptureUsed=CaptureBytes=0;
}
static VOID intact_report(VOID) {
  assert(!console_calls && !LogFault && GuLogReady());
}
static VOID failed(EFI_STATUS Expected) {
  assert(!Capture && GuCaptureClose()==Expected);intact_report();
  unsigned old_writes=writes,old_closes=closes;
  assert(GuCaptureWrite("later")==Expected);
  assert(writes==old_writes && closes==old_closes);
}
int main(int argc,char **argv) {
  assert(argc==2);setup();const char *scenario=argv[1];
  char block[CAPTURE_BUFFER_BYTES+2];memset(block,'a',sizeof(block));block[CAPTURE_BUFFER_BYTES]=0;
  if(!strcmp(scenario,"buffering")) {
    assert(GuCaptureOpen()==EFI_SUCCESS);
    assert(GuCaptureWrite("first")==EFI_SUCCESS && !writes && !flushes);
    assert(GuCaptureWrite(block)==EFI_SUCCESS && writes==1 && !flushes);
    assert(GuCaptureWrite("last")==EFI_SUCCESS && writes==1);
    assert(GuCaptureClose()==EFI_SUCCESS && writes==2 && flushes==1 && closes==1);
    assert(payload_bytes==CAPTURE_BUFFER_BYTES+9 && !memcmp(payload,"first",5));
    assert(!memcmp(payload+payload_bytes-4,"last",4));intact_report();
  } else if(!strcmp(scenario,"existing")) {
    existing=TRUE;memcpy(payload,"previous",8);payload_bytes=8;
    assert(GuCaptureOpen()==EFI_ALREADY_STARTED && !creates && closes==1 && !writes);
    assert(payload_bytes==8 && !memcmp(payload,"previous",8));failed(EFI_ALREADY_STARTED);
  } else if(!strcmp(scenario,"late-nonempty")) {
    nonempty=TRUE;assert(GuCaptureOpen()==EFI_COMPROMISED_DATA);
    assert(creates==1 && closes==1 && !writes);failed(EFI_COMPROMISED_DATA);
  } else if(!strcmp(scenario,"create-error-handle")) {
    create_error=TRUE;assert(GuCaptureOpen()==EFI_DEVICE_ERROR);
    assert(closes==1 && !writes);failed(EFI_DEVICE_ERROR);
  } else if(!strcmp(scenario,"chunk-limit")) {
    assert(GuCaptureOpen()==EFI_SUCCESS);block[CAPTURE_BUFFER_BYTES]='a';block[CAPTURE_BUFFER_BYTES+1]=0;
    assert(GuCaptureWrite(block)==EFI_BAD_BUFFER_SIZE && !writes && closes==1);failed(EFI_BAD_BUFFER_SIZE);
  } else if(!strcmp(scenario,"total-limit") || !strcmp(scenario,"exact-limit")) {
    assert(GuCaptureOpen()==EFI_SUCCESS);
    for(UINTN i=0;i<CAPTURE_LIMIT_BYTES/CAPTURE_BUFFER_BYTES;i++)assert(GuCaptureWrite(block)==EFI_SUCCESS);
    assert(payload_bytes==CAPTURE_LIMIT_BYTES && !flushes);
    if(!strcmp(scenario,"total-limit")) {
      assert(GuCaptureWrite("x")==EFI_BAD_BUFFER_SIZE && payload_bytes==CAPTURE_LIMIT_BYTES);
      assert(closes==1);failed(EFI_BAD_BUFFER_SIZE);
    } else {assert(GuCaptureClose()==EFI_SUCCESS && flushes==1 && closes==1);intact_report();}
  } else if(!strcmp(scenario,"null-chunk")) {
    assert(GuCaptureOpen()==EFI_SUCCESS);
    assert(GuCaptureWrite(NULL)==EFI_INVALID_PARAMETER && closes==1);failed(EFI_INVALID_PARAMETER);
  } else if(!strcmp(scenario,"duplicate-open")) {
    assert(GuCaptureOpen()==EFI_SUCCESS);
    assert(GuCaptureOpen()==EFI_ALREADY_STARTED && closes==1);failed(EFI_ALREADY_STARTED);
  } else if(!strcmp(scenario,"short-write") || !strcmp(scenario,"write-error")) {
    assert(GuCaptureOpen()==EFI_SUCCESS);
    short_write=!strcmp(scenario,"short-write");write_error=!strcmp(scenario,"write-error");
    close_error=TRUE;
    EFI_STATUS expected=short_write?EFI_DEVICE_ERROR:EFI_WRITE_PROTECTED;
    assert(GuCaptureWrite(block)==expected && writes==1 && closes==1 && !flushes);failed(expected);
  } else if(!strcmp(scenario,"flush-error") || !strcmp(scenario,"close-error")) {
    assert(GuCaptureOpen()==EFI_SUCCESS && GuCaptureWrite("pending")==EFI_SUCCESS);
    flush_error=!strcmp(scenario,"flush-error");close_error=TRUE;
    EFI_STATUS expected=flush_error?EFI_DEVICE_ERROR:EFI_ABORTED;
    assert(GuCaptureClose()==expected && writes==1 && flushes==1 && closes==1);failed(expected);
  } else if(!strcmp(scenario,"fallback-close") || !strcmp(scenario,"fallback-error")) {
    assert(GuCaptureOpen()==EFI_SUCCESS && GuCaptureWrite("pending")==EFI_SUCCESS);
    flush_error=!strcmp(scenario,"fallback-error");
    assert(GuIoClose()==(flush_error?EFI_DEVICE_ERROR:EFI_SUCCESS));
    assert(writes==1 && flushes==1 && closes==1 && other_closes==2);
    assert(!Capture && !Report && !Run && !console_calls && !LogFault);
  } else if(!strcmp(scenario,"missing-run")) {
    Run=NULL;assert(GuCaptureOpen()==EFI_NOT_READY && !creates);failed(EFI_NOT_READY);
  } else {assert(!"unknown capture I/O scenario");}
  printf("capture I/O %s: PASS; console callbacks=0\n",scenario);return 0;
}
#endif
