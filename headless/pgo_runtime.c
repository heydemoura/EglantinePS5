// SPDX-License-Identifier: GPL-3.0-or-later
// EDEN_PGO=generate (tools/build-headless-native.sh): the console's side of LLVM's profile
// runtime. compiler-rt's own platform file wants <link.h> and writes through a file API the
// console lacks; the counters' sections are found by the linker's start/stop symbols as on
// Linux, and the profile leaves as one buffer (eden_pgo_dump, called from headless/main.cpp).
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "InstrProfiling.h"
#include "InstrProfilingInternal.h"

#define START(section) INSTR_PROF_SECT_START(section)
#define STOP(section) INSTR_PROF_SECT_STOP(section)
extern __llvm_profile_data START(INSTR_PROF_DATA_COMMON) COMPILER_RT_VISIBILITY COMPILER_RT_WEAK;
extern __llvm_profile_data STOP(INSTR_PROF_DATA_COMMON) COMPILER_RT_VISIBILITY COMPILER_RT_WEAK;
extern char START(INSTR_PROF_CNTS_COMMON) COMPILER_RT_VISIBILITY COMPILER_RT_WEAK;
extern char STOP(INSTR_PROF_CNTS_COMMON) COMPILER_RT_VISIBILITY COMPILER_RT_WEAK;
// No coverage bitmaps are made (no -fcoverage-mcdc): an empty range, not a missing section.
static char eden_pgo_no_bitmap;
extern char START(INSTR_PROF_NAME_COMMON) COMPILER_RT_VISIBILITY COMPILER_RT_WEAK;
extern char STOP(INSTR_PROF_NAME_COMMON) COMPILER_RT_VISIBILITY COMPILER_RT_WEAK;
extern ValueProfNode START(INSTR_PROF_VNODES_COMMON) COMPILER_RT_VISIBILITY COMPILER_RT_WEAK;
extern ValueProfNode STOP(INSTR_PROF_VNODES_COMMON) COMPILER_RT_VISIBILITY COMPILER_RT_WEAK;

COMPILER_RT_VISIBILITY const __llvm_profile_data *__llvm_profile_begin_data(void) { return &START(INSTR_PROF_DATA_COMMON); }
COMPILER_RT_VISIBILITY const __llvm_profile_data *__llvm_profile_end_data(void) { return &STOP(INSTR_PROF_DATA_COMMON); }
COMPILER_RT_VISIBILITY const char *__llvm_profile_begin_names(void) { return &START(INSTR_PROF_NAME_COMMON); }
COMPILER_RT_VISIBILITY const char *__llvm_profile_end_names(void) { return &STOP(INSTR_PROF_NAME_COMMON); }
COMPILER_RT_VISIBILITY char *__llvm_profile_begin_counters(void) { return &START(INSTR_PROF_CNTS_COMMON); }
COMPILER_RT_VISIBILITY char *__llvm_profile_end_counters(void) { return &STOP(INSTR_PROF_CNTS_COMMON); }
COMPILER_RT_VISIBILITY char *__llvm_profile_begin_bitmap(void) { return &eden_pgo_no_bitmap; }
COMPILER_RT_VISIBILITY char *__llvm_profile_end_bitmap(void) { return &eden_pgo_no_bitmap; }
COMPILER_RT_VISIBILITY ValueProfNode *__llvm_profile_begin_vnodes(void) { return &START(INSTR_PROF_VNODES_COMMON); }
COMPILER_RT_VISIBILITY ValueProfNode *__llvm_profile_end_vnodes(void) { return &STOP(INSTR_PROF_VNODES_COMMON); }
COMPILER_RT_VISIBILITY ValueProfNode *CurrentVNode = &START(INSTR_PROF_VNODES_COMMON);
COMPILER_RT_VISIBILITY ValueProfNode *EndVNode = &STOP(INSTR_PROF_VNODES_COMMON);
COMPILER_RT_VISIBILITY int __llvm_write_binary_ids(ProfDataWriter *Writer) { (void)Writer; return 0; }

// The symbol instrumented objects ask the runtime for.
int __llvm_profile_runtime = 0;

// Writes the counters so far to `path` (a .profraw for llvm-profdata merge). 0 when written.
int eden_pgo_dump(const char *path) {
  const uint64_t size = __llvm_profile_get_size_for_buffer();
  char *const buffer = malloc(size);
  if (buffer == NULL) return -1;
  int result = __llvm_profile_write_buffer(buffer);
  FILE *const file = result == 0 ? fopen(path, "wb") : NULL;
  if (file == NULL) result = -1;
  else {
    if (fwrite(buffer, 1, size, file) != size) result = -1;
    fclose(file);
  }
  free(buffer);
  fprintf(stderr, "EDEN_PGO profile of %llu bytes to %s: %s\n", (unsigned long long)size, path, result == 0 ? "written" : "failed");
  return result;
}

// From EDEN_PGO_DUMP_AT seconds after launch (env.txt; 200 by default) and every 30 s after:
// the test run is ended with a kill, so the last dump before it is the profile.
static void *eden_pgo_thread(void *unused) {
  (void)unused;
  sleep(5);
  const char *const at = getenv("EDEN_PGO_DUMP_AT");
  const unsigned first = at != NULL ? (unsigned)strtoul(at, NULL, 10) : 200u;
  sleep(first > 5 ? first - 5 : 0);
  for (;;) {
    eden_pgo_dump("/data/eglantineps5/logs/default.profraw");
    sleep(30);
  }
  return NULL;
}
__attribute__((constructor)) static void eden_pgo_start(void) {
  pthread_t thread;
  if (pthread_create(&thread, NULL, eden_pgo_thread, NULL) == 0) pthread_detach(thread);
}

// InstrProfilingUtil.c's two helpers for value profiling (the rest of that file is file I/O).
COMPILER_RT_VISIBILITY uint32_t lprofBoolCmpXchg(void **Ptr, void *OldV, void *NewV) {
  return __sync_bool_compare_and_swap(Ptr, OldV, NewV);
}
COMPILER_RT_VISIBILITY void *lprofPtrFetchAdd(void **Mem, long ByteIncr) {
  return (void *)__sync_fetch_and_add((intptr_t *)Mem, (intptr_t)ByteIncr);
}
