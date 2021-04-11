/**
 * A Linux-specific malloc/free wrapper
 *
 * prerequisite: libdw, which is part of elfutils (can be installed via apt-get install libdw-devel)
 * for getting DWARF debug info from binaries
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <execinfo.h>				/* for backtrace */
#include <unistd.h>

#include <elfutils/libdwfl.h>		/* DWARF functions for file/line number */
#include <libiberty/demangle.h>

static void* (*s_glibcMalloc)(size_t len);
static void (*s_glibcFree)(void *ptr);
static size_t s_numRecords = 0;
static int s_doneInit = 0;
static int s_doingBacktrace = 0;
static int s_debugPrint = 0;
static char* s_summaryFileName = NULL;

#define STACK_DEPTH 32
#define INIT_BUF_SIZE 65536
static char s_slabAllocBuf[INIT_BUF_SIZE];
static int s_INIT_BUF_OFFSET = 0;

typedef struct _AllocRecord
{
	size_t len;
	int numTracePtrs;
	void* trace[STACK_DEPTH];
	struct _AllocRecord *prev;
	struct _AllocRecord *next;
} AllocRecord;

static AllocRecord* s_record;

static void addRecord(AllocRecord* record, size_t len);
static void removeRecord(AllocRecord* record);
static void dumpStats();

/*
   __attribute__((constructor)) is a GNU extension to C++ -
   makes a given function automatically load on .so load

   __attribute__((destructor)) makes the function invoked on .so shutdown
   HOWEVER, note that coreutils binaries such as "ls" do atexit(close_stdout),
   so you can't write to stdout in the library destructor

  from dlsym man page:

       RTLD_NEXT
              Find the next occurrence of the desired symbol in the search order af‐
              ter the current object.  This allows one to provide a wrapper around a
              function  in  another shared object, so that, for example, the defini‐
              tion of a function in a preloaded shared  object  (see  LD_PRELOAD  in
              ld.so(8))  can find and invoke the "real" function provided in another
              shared object (or for that matter, the "next" definition of the  func‐
              tion in cases where there are multiple layers of preloading).


https://askubuntu.com/questions/966407/where-do-i-find-the-core-dump-in-ubuntu-16-04lts

On Ubuntu:

sudo sysctl -w kernel.core_pattern=/tmp/core.%u.%p.%t # to enable core generation
ulimit -c unlimited

restoring settings:

systemctl restart apport # to restore default apport settings
# which, by the way, were "|/usr/share/apport/apport %p %s %c %d %P" (without quotes)


 */
__attribute__((constructor))
static void libraryInit()
{
	char* debugPrintStr;

	s_summaryFileName = getenv("NOP_MALLOC_STATS");
	if (!s_summaryFileName)
	{
		fprintf(stderr, "NOP_MALLOC_STATS environment variables is not, exiting!\n");
		exit(1);
	}
	debugPrintStr = getenv("NOP_MALLOC_PRINT");
	if (debugPrintStr && atoi(debugPrintStr) == 1)
	{
		s_debugPrint = 1;
	}
	s_glibcMalloc = dlsym(RTLD_NEXT, "malloc");
 	s_glibcFree = dlsym(RTLD_NEXT, "free");

 	s_doneInit = 1;
}

__attribute__((destructor))
static void libraryShutdown()
{
	dumpStats();
}


/* overrides built-in malloc, with stats tracking and optional debug printing */
void* malloc(size_t len)
{
	void* result;

	if (s_doneInit)
	{
		/* pass through to normalloc malloc/free during backtrace */
		if (s_doingBacktrace)
		{
			result = s_glibcMalloc(len);
		}
		else
		{
			/* We are done with initialization, let's wrap malloc with our tracking */
			result = s_glibcMalloc(len + sizeof(AllocRecord));
			if (result)
			{
				addRecord((AllocRecord*)result, len);
				result = ((AllocRecord*)result) + 1; 
			}
			if (s_debugPrint)
			{
				fprintf(stderr, "malloc(%zd) = %p\n", len, result);
			}
		}
	}
	else
	{
		/* We are still initializating, let's do primitive slab allocation with no real freeing*/

		if (s_INIT_BUF_OFFSET + len > INIT_BUF_SIZE)
		{
			/* No memory left for initialization */
			return NULL;
		}
		result = s_slabAllocBuf + s_INIT_BUF_OFFSET;
		s_INIT_BUF_OFFSET += len;
	}
	return result;
}

/* overrides built-in free, with stats tracking and optional debug printing */
void free(void *ptr)
{
	AllocRecord* record;

	/* Freeing a NULL is a no-op, freeing memory within s_slabAllocBuf is also a no-op */
	if (!ptr || (ptr >= (void*)s_slabAllocBuf && ptr <= (void*)(s_slabAllocBuf + INIT_BUF_SIZE)))
	{
		return;
	}

	if (s_doingBacktrace)
	{
		s_glibcFree(ptr);
		return;
	}

	record = ((AllocRecord*)ptr) - 1;
	removeRecord(record);

	s_glibcFree(record);
	if (s_debugPrint)
	{
		fprintf(stderr, "free(%p)\n", ptr);
	}
}

void addRecord(AllocRecord* record, size_t len)
{
	++s_numRecords;
	record->len = len;
	s_doingBacktrace = 1;
	record->numTracePtrs = backtrace(record->trace, STACK_DEPTH);
	s_doingBacktrace = 0;
	record->prev = s_record;
	record->next = NULL;
	if (s_record)
	{
		s_record->next = record;
	}
	s_record = record;
}

void removeRecord(AllocRecord* record)
{
	--s_numRecords;
	if (record->prev)
	{
		record->prev->next = record->next;
	}
	else
	{
		if (record == s_record)
		{
			s_record = record->next;
		}
	}
	if (record->next)
	{
		record->next->prev = record->prev;
	}
	else
	{
		if (record == s_record)
		{
			s_record = record->prev;
		}
	}

}

void dumpStats()
{
	char *debugInfoPath = NULL;
	Dwfl_Callbacks callbacks =
	{
		.find_elf = dwfl_linux_proc_find_elf,
		.find_debuginfo = dwfl_standard_find_debuginfo,
		.debuginfo_path = &debugInfoPath
	};

	s_doingBacktrace = 1;

	FILE* statsFile = fopen(s_summaryFileName, "a");
	if (!statsFile)
	{
		exit(2);
	}
	Dwfl* dwfl = dwfl_begin(&callbacks);
	if (dwfl_linux_proc_report(dwfl, getpid()) != 0)
	{
		fprintf(statsFile, "dwfl_linux_proc_report: %s\n", dwfl_errmsg (-1));
		exit(3);
	}
	dwfl_report_end(dwfl, NULL, NULL);

	fprintf(statsFile, "%zd records\n", s_numRecords);
	while (s_record)
	{
		fprintf(statsFile, "\n%zd bytes:\n", s_record->len);

		for (int i=0; i<s_record->numTracePtrs; ++i)
		{
			Dwarf_Addr addr = (Dwarf_Addr)s_record->trace[i];
			Dwfl_Module* module = dwfl_addrmodule(dwfl, addr);
			const char* functionName = dwfl_module_addrname(module, addr);
			if (functionName == NULL)
			{
				functionName = "<UNKNOWN>";
			}
			else if (functionName[0] == '_' && functionName[1] == 'Z')
			{
				char* demangledFunctionName = cplus_demangle_v3(functionName, 0);
				if (demangledFunctionName)
				{
					functionName = demangledFunctionName;
				}
			}
			Dwfl_Line* line = dwfl_module_getsrc(module, addr);
			const char* fname = NULL;
			int lineNum = 0;
			if (line)
			{
				fname = dwfl_lineinfo(line, NULL, &lineNum, NULL, NULL, NULL);
			}
			if (fname)
			{
				fprintf(statsFile, "%s(%s:%d)\n", functionName, fname, lineNum);
			}
			else
			{
				fprintf(statsFile, "%s(%p)\n", functionName, (void*)addr);
			}
		}

		s_record = s_record->prev;
	}
	fclose(statsFile);
}
