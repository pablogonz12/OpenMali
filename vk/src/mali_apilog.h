/* SPDX-License-Identifier: MIT */
/* MALI_API_LOG=<file>: opt-in, file-based trace of the command-buffer calls a workload makes (no signal handlers, no
 * stderr), one line per call, so a CTS/app sequence can be replayed in a test without guessing. Off unless the variable
 * is set. Usage in a test: MALI_API_LOG=/data/local/tmp/x.log ./t_x ... */
#ifndef MALI_APILOG_H
#define MALI_APILOG_H

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

static FILE *mali_apilog_f(FILE *keep)
{
   static FILE *f;
   static int done;
   if (!done) {
      done = 1;
      const char *p = getenv("MALI_API_LOG");
      f = p && *p ? fopen(p, "a") : NULL;   /* append: the ICD can be mapped twice (shim + loader) and two "w" opens truncate each other */
   }
   (void)keep;
   return f;
}

__attribute__((format(printf, 1, 2))) static void
mali_apilog(const char *fmt, ...)
{
   FILE *f = mali_apilog_f(NULL);
   if (!f) return;
   va_list ap;
   va_start(ap, fmt);
   vfprintf(f, fmt, ap);
   va_end(ap);
   fputc('\n', f);
   fflush(f);
}

static const char *mali_apilog_dir(void)
{
   static const char *p;
   static int done;
   if (!done) {
      done = 1;
      p = getenv("MALI_API_LOG");
      if (p && !*p) p = NULL;
   }
   return p;
}

/* Dump a blob next to the log: <MALI_API_LOG>.<name> */
static void
mali_apilog_dump(const char *name, const void *data, size_t n)
{
   const char *base = mali_apilog_dir();
   if (!base || !data || !n) return;
   char path[512];
   snprintf(path, sizeof path, "%s.%s", base, name);
   FILE *f = fopen(path, "wb");
   if (!f) return;
   fwrite(data, 1, n, f);
   fclose(f);
}

#endif
