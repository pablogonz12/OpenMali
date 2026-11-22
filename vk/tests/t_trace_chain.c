/* SPDX-License-Identifier: MIT */
/* A diagnostic must never change what the program does. MALI_TRACE's fatal-signal handler observes and then CHAINS to the
 * action the program had installed itself. Under FEX (x86 emulation) FEX's own SIGBUS/SIGSEGV handlers do routine work
 * (unaligned atomics, self-modifying code); replacing them with SA_RESETHAND killed the game with
 * "FATAL signal 7 code=1 BUS_ADRALN" in [anon:FEXMemJIT] right after vkCreateDevice.
 *
 *   t_trace_chain --direct <icd.so>
 *
 * Every case runs in its own child (the constructor only runs at dlopen): the child installs its own handler FIRST, then
 * loads the ICD with MALI_TRACE set, then triggers the signal.
 * 1. SIGSEGV fixed up in place: the handler mprotects the faulting page and returns; the store must succeed on retry and
 *    the process survive. Two faults, two handler calls: SA_RESETHAND must be gone.
 * 2. SIGBUS handled with siglongjmp() (FEX's own pattern): the process must survive and land after the trigger.
 * 3. SIG_IGN stays ignored: raise(SIGBUS) must not kill the process.
 * 4. no handler of its own: the process still dies of the signal (the SIG_DFL path restores the default and re-raises),
 *    and the FATAL line is in the file before it does.
 */
#define _GNU_SOURCE
#define VK_NO_PROTOTYPES
#include <dlfcn.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int g_fail;
#define CHECK(c, ...)                                                                              \
   do {                                                                                            \
      if (!(c)) {                                                                                  \
         printf("FAIL %s:%d %s: ", __FILE__, __LINE__, #c);                                       \
         printf(__VA_ARGS__);                                                                      \
         printf("\n");                                                                             \
         g_fail = 1;                                                                             \
      }                                                                                            \
   } while (0)

static volatile sig_atomic_t g_hits;
static volatile sig_atomic_t g_addr_ok;
static void *g_page;
static sigjmp_buf g_jb;

/* SIGSEGV: make the page writable and return: the kernel retries the store (FEX's "fix up and return" shape). */
static void
fixup_segv(int sig, siginfo_t *si, void *uc)
{
   (void)sig;
   (void)uc;
   g_hits++;
   if (si && si->si_addr == g_page)
      g_addr_ok = 1;
   if (mprotect(g_page, 4096, PROT_READ | PROT_WRITE) != 0)
      _exit(7);
}

/* SIGBUS: leave the faulting context entirely (FEX's SMC/atomic path). */
static void
jump_bus(int sig, siginfo_t *si, void *uc)
{
   (void)si;
   (void)uc;
   g_hits++;
   siglongjmp(g_jb, 1 + sig);
}

static void
install(int sig, int flags, void *fn)
{
   struct sigaction sa = { .sa_flags = flags };
   if (flags & SA_SIGINFO)
      sa.sa_sigaction = (void (*)(int, siginfo_t *, void *))fn;
   else
      sa.sa_handler = (void (*)(int))fn;
   sigemptyset(&sa.sa_mask);
   if (sigaction(sig, &sa, NULL) != 0) {
      perror("sigaction");
      _exit(8);
   }
}

static char *
slurp(const char *path)
{
   static char buf[16384];
   FILE *f = fopen(path, "r");
   if (!f)
      return NULL;
   size_t n = fread(buf, 1, sizeof(buf) - 1, f);
   buf[n] = 0;
   fclose(f);
   return buf;
}

/* child: install the handler shown by `mode`, load the ICD with MALI_TRACE, run the case; returns the child's exit code */
enum { M_FIXUP, M_JUMP, M_IGNORE, M_NONE };

static int
child(const char *so, const char *path, int mode, int *termsig)
{
   pid_t c = fork();
   if (c == 0) {
      setenv("MALI_TRACE", path, 1);
      if (mode == M_FIXUP) {
         g_page = mmap(NULL, 4096, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
         if (g_page == MAP_FAILED)
            _exit(9);
         install(SIGSEGV, SA_SIGINFO | SA_NODEFER, (void *)fixup_segv);
      } else if (mode == M_JUMP) {
         install(SIGBUS, SA_SIGINFO | SA_NODEFER, (void *)jump_bus);
      } else if (mode == M_IGNORE) {
         install(SIGBUS, 0, (void *)SIG_IGN);
      } else if (mode == M_NONE) {
         /* with MALI_SAN=1 the sanitizer preloads its own SIGSEGV handler: reset it, so this case really exercises the
          * default action instead of chaining into ASan (which exits normally with abort_on_error=0) */
         install(SIGSEGV, 0, (void *)SIG_DFL);
      }
      void *lib = dlopen(so, RTLD_NOW | RTLD_LOCAL);
      if (!lib)
         _exit(3);
      if (mode == M_FIXUP) {   /* two faults: the second proves the handler is still installed (no SA_RESETHAND) */
         *(volatile int *)g_page = 1;
         if (mprotect(g_page, 4096, PROT_READ) != 0)
            _exit(15);   /* the first fault must have made the page writable */
         *(volatile int *)g_page = 2;
         _exit(g_hits == 2 && g_addr_ok ? 0 : 10);
      } else if (mode == M_JUMP) {
         if (sigsetjmp(g_jb, 1) == 0) {
            raise(SIGBUS);
            _exit(11);   /* the handler never ran */
         }
         _exit(g_hits == 1 ? 0 : 12);
      } else if (mode == M_IGNORE) {
         raise(SIGBUS);
         _exit(0);
      }
      raise(SIGSEGV);   /* M_NONE: really dying */
      _exit(13);
   }
   int ws = 0;
   if (c < 0 || waitpid(c, &ws, 0) != c)
      _exit(14);
   *termsig = WIFSIGNALED(ws) ? WTERMSIG(ws) : 0;
   return WIFEXITED(ws) ? WEXITSTATUS(ws) : -1;
}

int
main(int argc, char **argv)
{
   const char *so = NULL;
   for (int i = 1; i < argc; i++)
      if (!strcmp(argv[i], "--direct") && i + 1 < argc)
         so = argv[++i];
   if (!so) {
      fprintf(stderr, "usage: t_trace_chain --direct <icd.so>\n");
      return 2;
   }
   char path[128];
   snprintf(path, sizeof(path), "/tmp/t_trace_chain.%d.log", (int)getpid());
   int sig = 0;

   /* 1. a SIGSEGV the program fixes up itself */
   unlink(path);
   int rc = child(so, path, M_FIXUP, &sig);
   CHECK(rc == 0, "SIGSEGV fix-up did not survive: exit %d signal %d (the trace handler replaced ours)", rc, sig);
   char *t = slurp(path);
   CHECK(t && strstr(t, "FATAL signal 11"), "no FATAL signal 11 line: %s", t ? t : "(no file)");

   /* 2. a SIGBUS the program longjmps out of */
   unlink(path);
   rc = child(so, path, M_JUMP, &sig);
   CHECK(rc == 0, "SIGBUS longjmp did not survive: exit %d signal %d", rc, sig);
   t = slurp(path);
   CHECK(t && strstr(t, "FATAL signal 7"), "no FATAL signal 7 line: %s", t ? t : "(no file)");

   /* 3. SIG_IGN stays ignored */
   unlink(path);
   rc = child(so, path, M_IGNORE, &sig);
   CHECK(rc == 0, "an ignored SIGBUS killed the process: exit %d signal %d", rc, sig);

   /* 4. nobody else handles it: the process still dies, and the trace says why */
   unlink(path);
   rc = child(so, path, M_NONE, &sig);
   CHECK(sig == SIGSEGV, "the no-handler case must die of SIGSEGV: exit %d signal %d", rc, sig);
   t = slurp(path);
   CHECK(t && strstr(t, "FATAL signal 11"), "no FATAL line on the SIG_DFL path: %s", t ? t : "(no file)");
   unlink(path);

   printf("%s\n", g_fail ? "FAIL" : "OK t_trace_chain");
   return g_fail;
}
