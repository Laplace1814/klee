//===-- fd_init.c ---------------------------------------------------------===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#define _LARGEFILE64_SOURCE
#define _FILE_OFFSET_BITS 64
#include "fd.h"

#include "klee/klee.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

exe_file_system_t __exe_fs;

/* NOTE: It is important that these are statically initialized
   correctly, since things that run before main may hit them given the
   current way things are linked. */

/* XXX Technically these flags are initialized w.o.r. to the
   environment we are actually running in. We could patch them in
   klee_init_fds, but we still have the problem that uclibc calls
   prior to main will get the wrong data. Not such a big deal since we
   mostly care about sym case anyway. */


exe_sym_env_t __exe_env = { 
  {{ 0, eOpen | eReadable, 0, 0}, 
   { 1, eOpen | eWriteable, 0, 0}, 
   { 2, eOpen | eWriteable, 0, 0}},
  022,
  0,
  0
};

static void __create_new_dfile(exe_disk_file_t *dfile, unsigned size, 
                               const char *name, struct stat64 *defaults) {
  struct stat64 *s = malloc(sizeof(*s));
  if (!s)
    klee_report_error(__FILE__, __LINE__, "out of memory in klee_init_env", "user.err");

  const char *sp;
  char sname[256] = { 0 };

  int len = strlen(name);
  if (len >= 256) { 
    klee_report_error(__FILE__, __LINE__, "The file path is too long, the maximum length of the path is 256.", "user.err");
  } 
  strcpy(sname,name);
  if (256-len <= 6) { 
    klee_report_error(__FILE__, __LINE__, "The file path is too long, 6 bytes must be reserved to store stat.", "user.err");
  } 
  strcpy(sname,"-stat");

  assert(size);
  strcpy(dfile->file_name, name);
  dfile->size = size;
  dfile->contents = malloc(dfile->size);
  if (!dfile->contents)
    klee_report_error(__FILE__, __LINE__, "out of memory in klee_init_env", "user.err");
  klee_make_symbolic(dfile->contents, dfile->size, name);
  
  klee_make_symbolic(s, sizeof(*s), sname);

  /* For broken tests */
  if (!klee_is_symbolic(s->st_ino) && 
      (s->st_ino & 0x7FFFFFFF) == 0)
    s->st_ino = defaults->st_ino;
  
  /* Important since we copy this out through getdents, and readdir
     will otherwise skip this entry. For same reason need to make sure
     it fits in low bits. */
  klee_assume((s->st_ino & 0x7FFFFFFF) != 0);

  /* uclibc opendir uses this as its buffer size, try to keep
     reasonable. */
  klee_assume((s->st_blksize & ~0xFFFF) == 0);

  klee_prefer_cex(s, !(s->st_mode & ~(S_IFMT | 0777)));
  klee_prefer_cex(s, s->st_dev == defaults->st_dev);
  klee_prefer_cex(s, s->st_rdev == defaults->st_rdev);
  klee_prefer_cex(s, (s->st_mode&0700) == 0600);
  klee_prefer_cex(s, (s->st_mode&0070) == 0040);
  klee_prefer_cex(s, (s->st_mode&0007) == 0004);
  klee_prefer_cex(s, (s->st_mode&S_IFMT) == S_IFREG);
  klee_prefer_cex(s, s->st_nlink == 1);
  klee_prefer_cex(s, s->st_uid == defaults->st_uid);
  klee_prefer_cex(s, s->st_gid == defaults->st_gid);
  klee_prefer_cex(s, s->st_blksize == 4096);
  klee_prefer_cex(s, s->st_atime == defaults->st_atime);
  klee_prefer_cex(s, s->st_mtime == defaults->st_mtime);
  klee_prefer_cex(s, s->st_ctime == defaults->st_ctime);

  s->st_size = dfile->size;
  s->st_blocks = 8;
  dfile->stat = s;
}

static unsigned __sym_uint32(const char *name) {
  unsigned x;
  klee_make_symbolic(&x, sizeof x, name);
  return x;
}

exe_disk_file_t* gey_sym_file(char* fname) { 
  for (int i=0; i<__exe_fs.n_sym_files; ++i) { 
    if (!__exe_fs.sym_files[i].file_name || !__exe_fs.sym_files[i].contents) { 
      continue;
    } 
    if (!strcmp(fname, __exe_fs.sym_files[i].file_name)) { 
      return &__exe_fs.sym_files[i];
    } 
  } 
  return NULL;
} 

/* n_files: number of symbolic input files, excluding stdin
   file_length: size in bytes of each symbolic file, including stdin
   sym_stdout_flag: 1 if stdout should be symbolic, 0 otherwise
   save_all_writes_flag: 1 if all writes are executed as expected, 0 if 
                         writes past the initial file size are discarded 
			 (file offset is always incremented)
   max_failures: maximum number of system call failures */
void klee_init_fds(fsym_info* fsym, unsigned nfsym, unsigned n_files, unsigned file_length,
                   unsigned stdin_length, int sym_stdout_flag,
                   int save_all_writes_flag, unsigned max_failures) {
  unsigned k;
  char name[7] = "?-data";
  char fname[256] = { 0 };
  struct stat64 s;
  exe_disk_file_t* df = 0;
  int fsym_idx = 0;
  stat64(".", &s);

  *__exe_fs.fsymArray = *fsym;
  __exe_fs.nfsym = nfsym;

  __exe_fs.n_sym_files = nfsym + n_files;
  __exe_fs.sym_files = malloc(sizeof(*__exe_fs.sym_files) * __exe_fs.n_sym_files);
  if (__exe_fs.n_sym_files && !__exe_fs.sym_files)
    klee_report_error(__FILE__, __LINE__, "out of memory in klee_init_env", "user.err");

  //__exe_fs.sym_files memory initialization 
  memset(__exe_fs.sym_files, 0, sizeof(*__exe_fs.sym_files) * __exe_fs.n_sym_files);

  for (size_t i = 0; i < nfsym; i++) {  
    if (df = gey_sym_file(fsym[i].pfile_name)) {
      klee_warning("Two symbolic files with the same name cannot exist at the same time.");
      continue;
    } 
    __create_new_dfile(&__exe_fs.sym_files[fsym_idx], fsym[i].file_len, fsym[i].pfile_name, &s);
    fsym_idx++;
  } 

  for (k=0; k < n_files; k++) { 
    name[0] = 'A' + k;
    if (df = gey_sym_file(name)) {
      klee_warning("Two symbolic files with the same name cannot exist at the same time.");
      continue;
    } 
    __create_new_dfile(&__exe_fs.sym_files[fsym_idx], file_length, name, &s);
    fsym_idx++;
  } 
  
  /* setting symbolic stdin */
  if (stdin_length) {
    __exe_fs.sym_stdin = malloc(sizeof(*__exe_fs.sym_stdin));
    if (!__exe_fs.sym_stdin)
      klee_report_error(__FILE__, __LINE__, "out of memory in klee_init_env", "user.err");
    __create_new_dfile(__exe_fs.sym_stdin, stdin_length, "stdin", &s);
    __exe_env.fds[0].dfile = __exe_fs.sym_stdin;
  }
  else __exe_fs.sym_stdin = NULL;

  __exe_fs.max_failures = max_failures;
  if (__exe_fs.max_failures) {
    __exe_fs.read_fail = malloc(sizeof(*__exe_fs.read_fail));
    __exe_fs.write_fail = malloc(sizeof(*__exe_fs.write_fail));
    __exe_fs.close_fail = malloc(sizeof(*__exe_fs.close_fail));
    __exe_fs.ftruncate_fail = malloc(sizeof(*__exe_fs.ftruncate_fail));
    __exe_fs.getcwd_fail = malloc(sizeof(*__exe_fs.getcwd_fail));
    if (!(__exe_fs.read_fail && __exe_fs.write_fail && __exe_fs.close_fail
          && __exe_fs.ftruncate_fail && __exe_fs.getcwd_fail))
      klee_report_error(__FILE__, __LINE__, "out of memory in klee_init_env", "user.err");

    klee_make_symbolic(__exe_fs.read_fail, sizeof(*__exe_fs.read_fail), "read_fail");
    klee_make_symbolic(__exe_fs.write_fail, sizeof(*__exe_fs.write_fail), "write_fail");
    klee_make_symbolic(__exe_fs.close_fail, sizeof(*__exe_fs.close_fail), "close_fail");
    klee_make_symbolic(__exe_fs.ftruncate_fail, sizeof(*__exe_fs.ftruncate_fail), "ftruncate_fail");
    klee_make_symbolic(__exe_fs.getcwd_fail, sizeof(*__exe_fs.getcwd_fail), "getcwd_fail");
  }

  /* setting symbolic stdout */
  if (sym_stdout_flag) {
    __exe_fs.sym_stdout = malloc(sizeof(*__exe_fs.sym_stdout));
    if (!__exe_fs.sym_stdout)
      klee_report_error(__FILE__, __LINE__, "out of memory in klee_init_env", "user.err");
    __create_new_dfile(__exe_fs.sym_stdout, 1024, "stdout", &s);
    __exe_env.fds[1].dfile = __exe_fs.sym_stdout;
    __exe_fs.stdout_writes = 0;
  }
  else __exe_fs.sym_stdout = NULL;
  
  __exe_env.save_all_writes = save_all_writes_flag;
  __exe_env.version = __sym_uint32("model_version");
  klee_assume(__exe_env.version == 1);
}
