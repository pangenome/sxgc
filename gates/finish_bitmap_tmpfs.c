/* Gate-only Linux/glibc pathname adapter for ONE frozen finish binary whose
 * bitmap calls are open(..., O_CREAT | O_EXCL | O_RDWR, 0600) and unlink().
 * Keep the fd, mmap, size, exclusivity and lifetime real. No durability claim
 * for this one intermediate. All other paths have normal filesystem backing.
 * The launcher passes an empty private tmpfs directory fd, not a global path.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/magic.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <unistd.h>

static const char *logical_path;
static const char leaf[] = "frag.chi-bitmap";
static int ramdir = -1;
static void note(const char *s) { int saved = errno; ssize_t n = write(2, s, strlen(s)); (void)n; errno = saved; }
static void refuse(void) { note("GATE_BITMAP invalid private mapping\n"); _exit(70); }
__attribute__((constructor)) static void init(void) {
    const char *s = getenv("XSA_GATE_BITMAP_DIRFD"), *p = getenv("XSA_GATE_BITMAP_PATH");
    const char *tail = "/merged/frag.chi-bitmap";
    if (!s || !*s || !p || p[0]!='/' || strlen(p)<=strlen(tail) || strcmp(p+strlen(p)-strlen(tail),tail)) refuse();
    char *end; errno=0; long fd=strtol(s,&end,10);
    struct stat st; struct statfs fs;
    if (errno || *end || fd<3 || fd>INT_MAX || fstat(fd,&st) || !S_ISDIR(st.st_mode) ||
        st.st_uid!=geteuid() || (st.st_mode & 0777)!=0700 || fstatfs(fd,&fs) || fs.f_type!=TMPFS_MAGIC) refuse();
    ramdir=(int)fd; logical_path=p;
}
int open(const char *path,int flags,...) {
    mode_t mode=0;
    if ((flags & O_CREAT) || (flags & O_TMPFILE)==O_TMPFILE) {
        va_list ap; va_start(ap,flags); mode=va_arg(ap,mode_t); va_end(ap);
    }
    if (!logical_path || strcmp(path,logical_path)) return openat(AT_FDCWD,path,flags,mode);
    if ((flags & (O_CREAT|O_EXCL|O_ACCMODE))!=(O_CREAT|O_EXCL|O_RDWR) || (flags & O_TRUNC) || mode!=0600) refuse();
    struct stat st;
    if (!fstatat(AT_FDCWD,path,&st,AT_SYMLINK_NOFOLLOW)) { errno=EEXIST; return -1; }
    if (errno!=ENOENT) return -1;
    int fd=openat(ramdir,leaf,flags|O_NOFOLLOW,mode);
    if (fd>=0) note("GATE_BITMAP create=tmpfs exclusive=1\n");
    return fd;
}
int unlink(const char *path) {
    if (!logical_path || strcmp(path,logical_path)) return unlinkat(AT_FDCWD,path,0);
    int rc=unlinkat(ramdir,leaf,0);
    if (!rc) note("GATE_BITMAP unlink=tmpfs\n");
    return rc;
}
