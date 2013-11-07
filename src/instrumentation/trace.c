/*
 * trace.c
 *
 *  This system emits trace codes to create a trace of a given run of an R program
 *
 *  Created on: Jan 24, 2011
 *      Author: r-core@purdue.edu
 */


#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#define R_USE_SIGNALS 1
#include <Defn.h>
#include <trace.h>

#include <time.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <stdarg.h>
#include <zlib.h>

#define HG_ID "000000000000+"  // dummy, don't change length

#ifdef TRACE_ZIPPED
  typedef gzFile TRACEFILE;
#else
  typedef FILE  *TRACEFILE;
#endif

typedef struct TraceInfo_ {
    char directory[MAX_DNAME];
    char trace_file_name[MAX_FNAME];

    TRACEFILE src_map_file;

    char trace_version[12];
} TraceInfo;

TraceInfo *trace_info;

static TRACEFILE bin_trace_file;

// Trace counters
//unsigned int fatal_err_cnt; -> main.c via Defn.h->trace.h
static unsigned int func_decl_cnt, null_srcref_cnt;
static unsigned int stack_err_cnt, stack_flush_cnt;
static unsigned int stack_height, max_stack_height;
static unsigned long int bytes_written, event_cnt;

extern unsigned long duplicate_object, duplicate_elts, duplicate1_elts;

extern unsigned long allocated_cell[];
extern unsigned int  allocated_cell_len;

// SEXP (*56)
extern unsigned long allocated_cons, allocated_prom, allocated_env; // bytes
extern unsigned long allocated_external, allocated_sexp, allocated_noncons; // bytes
extern unsigned long allocated_cons_current, allocated_cons_peak; // count

// Vector count
extern unsigned long allocated_vector, allocated_vector_size, allocated_vector_asize, allocated_vector_elts;
extern unsigned long allocated_vector_null, allocated_vector_size_null, allocated_vector_asize_null, allocated_vector_elts_null;
extern unsigned long allocated_vector_small, allocated_vector_size_small, allocated_vector_asize_small, allocated_vector_elts_small;
extern unsigned long allocated_vector_large, allocated_vector_size_large, allocated_vector_asize_large, allocated_vector_elts_large;
extern unsigned long allocated_vector_one, allocated_vector_size_one, allocated_vector_asize_one, allocated_vector_elts_one;

// String buffers count
extern unsigned long allocated_sb, allocated_sb_size, allocated_sb_elts;


extern unsigned long duplicate_object, duplicate_elts, duplicate1_elts;

extern unsigned long set_var, define_var, define_user_db, super_set_var, super_define_var;
extern unsigned long apply_define, super_apply_define;
extern unsigned long do_set_unique, do_set_allways;
extern unsigned long do_super_set_unique, do_super_set_allways;
extern unsigned long err_count_assign;
extern unsigned long allocated_list, allocated_list_elts;
extern unsigned long dispatchs, dispatchFailed;
extern int gc_count;
extern unsigned long context_opened;
unsigned long clos_call, spec_call, builtin_call;



//
// Context Stack
//
#define UPROM_TAG 9999
typedef enum {
    PC_PAIR, //marks that prologue / call pair should occur in the stack
    SPEC,
    BUILTIN,
    CLOSURE,
    CNTXT,
    PROM,
    PROL
} StackNodeType;

typedef struct ContextStackNode_ {
    StackNodeType type;
    uintptr_t ID;
    RCNTXT *cptr;
    struct ContextStackNode_ *next;
} ContextStackNode;

static ContextStackNode *cstack_top, *cstack_bottom;

char *StackNodeTypeName[] = {"PAIR", "SPEC", "BUIL", "CLOS", "CNTXT", "PROM", "PROL", "UNKN"};

// utility functions
void __attribute__((__format__(printf, 1, 2)))
  print_error_msg(const char *format, ...) {
    fprintf(stderr, "[Error] ");
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    if (format[strlen(format) - 1] != '\n')
	fprintf(stderr, "\n");
}


#ifdef TRACE_ZIPPED

/* borrow gzio replacement functions from connections.c
   which includes them from gzio.h */

gzFile R_gzopen (const char *path, const char *mode);
int R_gzclose (gzFile file);
int R_gzwrite (gzFile file, voidpc buf, unsigned len);

// gzprintf replacement because it's not even in gzio.h
static int __attribute__((format(printf, 2, 3)))
  gz_printf(TRACEFILE file, const char *format, ...) {
    char strbuf[2000];
    int res;
    va_list args;

    va_start(args, format);
    res = vsnprintf(strbuf, sizeof(strbuf), format, args);
    va_end(args);
    WRITE_FUN(file, strbuf, res);
    return res;
}

#endif

// emergency exit
static void trace_exit(int code) {
    goto_abs_top_context();
    trace_cnt_fatal_err();
    terminate_tracing();
    exit(code);
}

// Trace binary writes
static inline void WRITE_BYTE(TRACEFILE file, const unsigned char byte) {
    bytes_written += sizeof(char);
    WRITE_FUN(file, &byte, sizeof(char));
}

static inline void WRITE_2BYTES(TRACEFILE file, const unsigned int bytes) {
    bytes_written += 2*sizeof(char);
    WRITE_FUN(file, &bytes, 2*sizeof(char));
}

static inline void WRITE_ADDR(TRACEFILE file, const void *addr) {
    bytes_written += sizeof(void*);
    WRITE_FUN(file, &addr, sizeof(void*));
}

// unused
static inline void WRITE_UINT(TRACEFILE file, const unsigned int num) {
    bytes_written += sizeof(unsigned int);
    WRITE_FUN(file, &num, sizeof(unsigned int));
}

// unused
static inline void WRITE_INT(TRACEFILE file, const int num) {
    bytes_written += sizeof(int);
    WRITE_FUN(file, &num, sizeof(int));
}

static char *get_type_name(StackNodeType type) {
    const unsigned int max_type = sizeof(StackNodeTypeName) -1;
    if (type > max_type)
	type = max_type;
    return StackNodeTypeName[type];
}

int get_cstack_height() {
    unsigned int cnt = 0;
    ContextStackNode *cur;
    cur = cstack_top;
    while (cur != cstack_bottom) {
	cur = cur->next;
	cnt++;
    }

    // This double checks all counting
    if (cnt != stack_height) {
	print_error_msg("Stack height counter is off by %d\n", stack_height - cnt);
	trace_exit(1);
    }

    return cnt;
}

static void inc_stack_height() {
    stack_height++;
    if (stack_height > max_stack_height)
	max_stack_height = stack_height;
}

static void dec_stack_height() { stack_height--; }

// currently unused, inline silences warning
static inline void print_node(ContextStackNode *cur) {
    printf("[type:%s\tID:%lu\tCTX:%p]\t", get_type_name(cur->type), (unsigned long)cur->ID, cur->cptr);
}

static void print_cstack() {
    ContextStackNode *cur = cstack_top;
    while (cur != cstack_bottom) {
	printf("%s ", get_type_name(cur->type));
	cur = cur->next;
    }
    printf("\n");
    return;
}

static StackNodeType sxp_to_stacknodetype(SEXP sxp) {
    StackNodeType type = CLOSURE; // type uninitialized
    switch (TYPEOF(sxp)) {
    case SPECIALSXP:
	type = SPEC;
	break;
    case BUILTINSXP:
	type = BUILTIN;
	break;
    case CLOSXP:
    default:
	type = CLOSURE;
	break;
    }
    return type;
}

static ContextStackNode* alloc_cstack_node(StackNodeType t) {
    ContextStackNode* new_node =  malloc(sizeof(ContextStackNode));
    new_node->type = t;
    new_node->cptr = NULL;
    new_node->next = NULL;
    new_node->ID = 0;

    return new_node;
}

static void push_cstack(StackNodeType t, uintptr_t ID) {
    ContextStackNode *new_node;

    new_node = alloc_cstack_node(t);
    new_node->ID = ID;
    new_node->next = cstack_top;
    cstack_top = new_node;
    inc_stack_height();
}

inline static int peek_type() {
    return cstack_top->type;
}

inline static uintptr_t peek_id() {
    return cstack_top->ID;
}

inline static void patch_pc_pair(uintptr_t id) {
    if (peek_type() != PC_PAIR) {
	print_error_msg("Context stack missing a PC_PAIR element\n");
	stack_err_cnt++;
	print_cstack();
	trace_exit(3);
    }
    cstack_top->ID = id;
}

static int pop_cstack_node(uintptr_t *id, RCNTXT **cntx) {
    // Return the type of the top elemennt & set params if provided
    ContextStackNode *popped_node = cstack_top;
    int type = popped_node->type;
    if(id)
	*id = popped_node->ID;
    if(cntx)
	*cntx = popped_node->cptr;
    if (popped_node == cstack_bottom) {
	print_error_msg("Attempted to pop below the context stack bottom.\n");
	stack_err_cnt++;
	print_cstack();
	trace_exit(1);
    }
    cstack_top = popped_node->next;
    free(popped_node);
    dec_stack_height();
    return type;
}

static void pop_cstack(StackNodeType type, uintptr_t ID) {
    uintptr_t node_id;
    StackNodeType node_type = pop_cstack_node(&node_id, NULL);
    if ((node_type == type)) {
	if (type == PROL) { // close prologue
	    patch_pc_pair(ID); // PC_PAIR gets ID from the PROL_END (it isn't known at PROL_START)
	} else if (node_id == ID) { // close function or promise
	    // if call was part of a prologue/call pair then also pop the PC_PAIR marker
	    if ((type == SPEC || type == BUILTIN || type == CLOSURE)
		&& peek_type() == PC_PAIR && peek_id() == ID)
		pop_cstack_node(NULL, NULL); // What's this unbalanced pop ?
	} else {
	    print_error_msg("Context stack is out of alignment, type is: %s but ID's don't match %p != %p\n", get_type_name(type), ID2SEXP(node_id), ID2SEXP(ID));
	    stack_err_cnt++;
	    print_cstack();
	    trace_exit(0);
	}
    } else {
	print_error_msg("Stack pop type mismatch %s (top)!= %s (pop)\n", get_type_name(node_type), get_type_name(type));
	print_cstack();
	stack_err_cnt++;
	trace_exit(0);
    }
}


//
// Trace bytecode emitters
//
void trcR_internal_emit_simple_type(SEXP expr) {
    unsigned int delim;
    unsigned int type;
    unsigned int len;
    if (expr) {
	type = (unsigned int) TYPEOF(expr);
	switch (type) {
	case PROMSXP:
	    delim = (PRVALUE(expr) == R_UnboundValue) ? BINTRACE_UBND : BINTRACE_BND;
	    /* check if this is the first time this promise is emitted */
	    if (NEW_PROMISE(expr)) {
		delim |= BINTRACE_NEW_PROMISE;
		SET_NEW_PROMISE(expr, 0);
	    }
	    WRITE_BYTE(bin_trace_file, delim);
	    WRITE_ADDR(bin_trace_file, expr);
	    break;
	case INTSXP:
	case REALSXP:
	case VECSXP:
	case EXPRSXP:
	case STRSXP:
	case CPLXSXP:
	    WRITE_BYTE(bin_trace_file, type);
	    len = LENGTH(expr);
	    if(len > 0xFF) len = 0xFF;
	    WRITE_BYTE(bin_trace_file, len);
	    len = TRUELENGTH(expr);
	    if(len > 0xFF) len = 0xFF;
	    WRITE_BYTE(bin_trace_file, len);
	    break;
	default:
	    WRITE_BYTE(bin_trace_file, type);
	}
    } else { // for NULL expr
	type = BINTRACE_UNIT;
	WRITE_BYTE(bin_trace_file, type);
    }
    event_cnt++;
}

void trcR_internal_emit_bnd_promise(SEXP prom) {
    SEXP prval = PRVALUE(prom);
    //unsigned int type = (unsigned int) TYPEOF (prval);
    WRITE_BYTE(bin_trace_file, BINTRACE_BND_PROM_START);
    WRITE_ADDR(bin_trace_file, prom);
    trcR_internal_emit_simple_type(prval);
    WRITE_BYTE(bin_trace_file, BINTRACE_PROM_END);
    event_cnt++;
}

void trcR_internal_emit_unbnd_promise(SEXP prom) {
    WRITE_BYTE(bin_trace_file, BINTRACE_UBND_PROM_START);
    WRITE_ADDR(bin_trace_file, prom);
    push_cstack(PROM, (unsigned long) UPROM_TAG);
    event_cnt++;
}

void trcR_internal_emit_unbnd_promise_return(SEXP prom) {
    WRITE_BYTE(bin_trace_file, BINTRACE_PROM_END);
    pop_cstack(PROM, (unsigned long) UPROM_TAG);
    event_cnt++;
}


void trcR_internal_emit_error_type(unsigned int type) {
    WRITE_BYTE(bin_trace_file, type);
    event_cnt++;
}

void trcR_internal_emit_primitive_function(SEXP fun, unsigned int type, unsigned int bparam, unsigned int bparam_ldots) {
    if ((type == BINTRACE_SPEC_ID) || (type == (BINTRACE_SPEC_ID | BINTRACE_NO_PROLOGUE)))
	push_cstack(SPEC, SEXP2ID(fun));
    else
	push_cstack(BUILTIN, SEXP2ID(fun));
    WRITE_BYTE(bin_trace_file, type);
    WRITE_2BYTES(bin_trace_file, PRIMOFFSET(fun));
    if (bparam > 0xff) bparam = 0xff;
    if (bparam > 0xff) bparam_ldots = 0xff;
    WRITE_BYTE(bin_trace_file, bparam);
    WRITE_BYTE(bin_trace_file, bparam_ldots);
    event_cnt++;
}

void trcR_internal_emit_prologue_start() {
    push_cstack(PC_PAIR, 0);
    push_cstack(PROL, 0);
    WRITE_BYTE(bin_trace_file, BINTRACE_PROL_START);
    event_cnt++;
}

void trcR_internal_emit_prologue_end(SEXP clos) {
    pop_cstack(PROL, SEXP2ID(clos));
    WRITE_BYTE(bin_trace_file, BINTRACE_PROL_END);
    event_cnt++;
}

// FIXME: alt_addr should be (u)intptr_t
// FIXME: type is always CLOS_ID or CLOS_ID|NO_PROLOGUE
void trcR_internal_emit_closure(SEXP closure, unsigned int type, unsigned long int alt_addr) {
    WRITE_BYTE(bin_trace_file, type);
    if (SXPEXISTS(closure)) {
	WRITE_ADDR(bin_trace_file, closure);
    } else {
      WRITE_ADDR(bin_trace_file, (void *)alt_addr);
    }
    push_cstack(SXPEXISTS(closure) ? sxp_to_stacknodetype(closure) : CLOSURE, SEXP2ID(closure));

    if (alt_addr != D_UM_ADDR) {
	int max = (trcR_by_position > 0xFF) ? 0xFF : trcR_by_position;
	WRITE_BYTE(bin_trace_file, max);

	max = (trcR_by_keyword > 0xFF) ? 0xFF : trcR_by_keyword;
	WRITE_BYTE(bin_trace_file, max);

	max = (trcR_by_dots > 0xFF) ? 0xFF : trcR_by_dots;
	WRITE_BYTE(bin_trace_file, max);
    } else {
	// Is this true ? maybe empty closure have args ? at least the one of the true closure to come
	WRITE_BYTE(bin_trace_file, 0);
	WRITE_BYTE(bin_trace_file, 0);
	WRITE_BYTE(bin_trace_file, 0);

    }
    event_cnt++;
}

// FIXME: type should be (u)intptr_t
void trcR_internal_emit_empty_closure(SEXP eObj, unsigned long int type) {
    WRITE_BYTE(bin_trace_file, BINTRACE_CLOS_ID);
    WRITE_ADDR(bin_trace_file, (void *)type);
    push_cstack(SXPEXISTS(eObj) ? sxp_to_stacknodetype(eObj) : CLOSURE, SEXP2ID(eObj)); // I assume that, if eObj is null, we are emitting a closure.

    int max = 0; // Is this true ? maybe empty closure have args ? at least the one of the true closure to come
    WRITE_BYTE(bin_trace_file, max);
    WRITE_BYTE(bin_trace_file, max);
    WRITE_BYTE(bin_trace_file, max);

    trcR_internal_emit_function_return(eObj, NULL);
    event_cnt++;
}

void trcR_internal_emit_function_return(SEXP fun, SEXP ret_val) {
    pop_cstack(SXPEXISTS(fun) ? sxp_to_stacknodetype(fun) : CLOSURE,  SEXP2ID(fun));
    WRITE_BYTE(bin_trace_file, BINTRACE_FUNC_END);

    //emit the return value type
    trcR_internal_emit_simple_type(ret_val);
    event_cnt++;
}

void trcR_internal_change_top_context(void) {
    if (cstack_bottom->cptr != R_ToplevelContext) {
        if (cstack_top == cstack_bottom) {
            cstack_bottom->cptr = R_ToplevelContext;
        } else {
            print_error_msg("Can't change top level context. Stack height is greater than 0\n");
            stack_err_cnt++;
            trace_exit(1);
        }
    }
}

void trace_context_add() {
    ContextStackNode *new_node;

    new_node = alloc_cstack_node(CNTXT);
    new_node->cptr = R_GlobalContext;
    new_node->next = cstack_top;
    cstack_top = new_node;
    inc_stack_height();
}

void trcR_internal_trace_context_drop() {
    StackNodeType popped_type;
    uintptr_t popped_ID = 0;

    // close any open delimiters
    while (cstack_top->type != CNTXT) {
	popped_type = peek_type();
	popped_ID = peek_id();
	pop_cstack(popped_type, popped_ID);

	switch (popped_type) {
	case PROM:
	    WRITE_BYTE(bin_trace_file, BINTRACE_PROM_END);
	    break;
	case PROL:
	    WRITE_BYTE(bin_trace_file, BINTRACE_PROL_END);
	case PC_PAIR:
	    trcR_emit_empty_closure(ID2SEXP(popped_ID), C_D_ADDR);
	    break;
	case SPEC:
	case BUILTIN:
	case CLOSURE:
	    /* mark end of CLOS, SPEC or BLTIN func */
	    WRITE_BYTE(bin_trace_file, BINTRACE_FUNC_END);
	    // emit a return type
	    trcR_internal_emit_simple_type(NULL);
	    break;
	default:
	    print_error_msg("Unknown type during context drop: %d\n", cstack_top->type);
	    stack_err_cnt++;
	    break;
	}
    }

    // then pop the context
    if (cstack_top != cstack_bottom) {
	popped_type = pop_cstack_node(NULL, NULL);
	if (popped_type != CNTXT) {
	    print_error_msg("Attempted to context drop a non-context item: %d.\n", popped_type);
	    stack_err_cnt++;
	    trace_exit(2);
	}
    }
}

void trcR_internal_goto_top_context() {
    while (cstack_top->cptr != R_ToplevelContext) {
	if (cstack_top != cstack_bottom) {
	    trcR_internal_trace_context_drop();
	} else {
	    print_error_msg("Failed attempt to return to top level context.\n");
	    stack_err_cnt++;
	    trace_exit(1);
	}
    }
    return;
}

// This fully flushes the stack
void goto_abs_top_context() {
    stack_flush_cnt = get_cstack_height();
    while (cstack_top != cstack_bottom) {
	trcR_internal_trace_context_drop();
    }
    return;
}

//
// Trace maintenance functions
//
void initialize_trace_defaults(TR_TYPE mode) {
    char str[MAX_DNAME];
    FILE *fd;
    FUNTAB *func;

    if (mode == TR_NONE) return;
    trace_info = malloc(sizeof(TraceInfo));

    //set the trace directory name
    if (R_TraceDir) {
	strcpy(trace_info->directory, R_TraceDir);
    } else {
	int written;
	time_t t = time (0);
	struct tm *current_time = localtime(&t);
	char *fname = R_InputFileName ? R_InputFileName : "stdin";
	char *lst = strrchr(fname, '/');
	lst = lst ? lst + 1 : fname;
	strftime(str, 15, "%y%m%d_%H%M%S", current_time);
	written = sprintf(trace_info->directory, "data_%s_%s",
			  str, lst);
	if (R_InputFileName)
	    trace_info->directory[written - 2] = 0;
    }
    // create directory for results if needed
    if (mkdir(trace_info->directory, S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH) && errno != EEXIST)
	print_error_msg("Can't create directory: %s\n", trace_info->directory);

    //initialize
    R_KeepSource = TRUE;
    traceR_is_active = 0;
    strncpy(trace_info->trace_version, HG_ID, 12);
    //set the trace file name
    sprintf(trace_info->trace_file_name, "%s/%s", trace_info->directory, TRACE_NAME);

    //copy the R source file into the trace dir
    if (R_InputFileName) {
	sprintf(str, "cp %s %s/source.R", R_InputFileName, trace_info->directory);
	if (system(str))
	    print_error_msg("Problem copying input file: %s\n", R_InputFileName);
	// TODO instead of copying grab what's the parser read
    }

    // dump function table into a file
    sprintf(str, "%s/R_function_table.txt", trace_info->directory);
    fd = fopen(str, "w");
    if (fd == NULL)
	print_error_msg("Can't open %s for writing: %s", str, strerror(errno));
    func = R_FunTab;
    while (func->name != NULL) {
	fprintf(fd, "%d %s\n", func->eval % 10, func->name);
	func++;
    }
    fclose(fd);

    //open src map file for writing
    sprintf(str, "%s/%s", trace_info->directory, SRC_MAP_NAME);
    trace_info->src_map_file = FOPEN (str);
    if (!trace_info->src_map_file) {
	print_error_msg ("Couldn't open file '%s' for writing", str);
	trace_exit (1);
    }
}

void start_tracing() {
    if (!traceR_is_active) {
	traceR_is_active = 1;

	//open output file
	bin_trace_file = FOPEN(trace_info->trace_file_name);
	if (!bin_trace_file) {
	    print_error_msg ("Couldn't open file '%s' for writing", trace_info->trace_file_name);
	    trace_exit (1);
	}

	//Write trace header
	bytes_written = FPRINTF(bin_trace_file, "%.12s", trace_info->trace_version);

	// init context stack
	cstack_top = alloc_cstack_node(CNTXT);
	cstack_top->cptr = R_ToplevelContext;
	cstack_bottom = cstack_top;

	//init counters
	event_cnt = 0;
	stack_err_cnt = 0;
	stack_flush_cnt = 0;
	stack_height = 0;
	max_stack_height = 0;
    }
}

extern void close_memory_map();
extern void display_unused(FILE *);
void write_missing_results(FILE *out);

void write_trace_summary(FILE *out) {
    R_gc();
    char str[TIME_BUFF > MAX_DNAME? TIME_BUFF : MAX_DNAME];
    time_t current_time = time(0);
    struct tm *local_time = localtime(&current_time);
    struct rusage my_rusage;
    fprintf(out, "SourceName: %s\n", R_InputFileName ? R_InputFileName : "stdin");

    getcwd(str, MAX_DNAME);
    fprintf(out, "Workdir: %s\n", str);
    fprintf(out, "File: %s/%s\n", str, R_InputFileName ? R_InputFileName : "stdin");
    fprintf(out, "Args: "); write_commandArgs(out);
    // TODO print trace_type all/repl/bootstrap
    fprintf(out, "TracerVersion: %s\n", HG_ID);
    fprintf(out, "PtrSize: %lu\n", sizeof(void*));

    strftime (str, TIME_BUFF, "%c", local_time);
    fprintf(out, "TraceDate: %s\n", str);
    getrusage(RUSAGE_SELF, &my_rusage);
    fprintf(out, "RusageMaxResidentMemorySet: %ld\n", my_rusage.ru_maxrss);
    fprintf(out, "RusageSharedMemSize: %ld\n", my_rusage.ru_ixrss);
    fprintf(out, "RusageUnsharedDataSize: %ld\n", my_rusage.ru_idrss);
    fprintf(out, "RusagePageReclaims: %ld\n", my_rusage.ru_minflt);
    fprintf(out, "RusagePageFaults: %ld\n", my_rusage.ru_majflt);
    fprintf(out, "RusageSwaps: %ld\n", my_rusage.ru_nswap);
    fprintf(out, "RusageBlockInputOps: %ld\n", my_rusage.ru_inblock);
    fprintf(out, "RusageBlockOutputOps: %ld\n", my_rusage.ru_oublock);
    fprintf(out, "RusageIPCSends: %ld\n", my_rusage.ru_msgsnd);
    fprintf(out, "RusageIPCRecv: %ld\n", my_rusage.ru_msgrcv);
    fprintf(out, "RusageSignalsRcvd: %ld\n", my_rusage.ru_nsignals);
    fprintf(out, "RusageVolnContextSwitches: %ld\n", my_rusage.ru_nvcsw);
    fprintf(out, "RusageInvolnContextSwitches: %ld\n", my_rusage.ru_nivcsw);

    // :display_unused(out);
    write_allocation_summary(out);
    write_missing_results(out);
}

void write_allocation_summary(FILE *out) {
    fprintf(out, "SizeOfSEXP: %ld\n", sizeof(SEXPREC));
    fprintf(out, "Interp: %lu\n", allocated_cons);
    fprintf(out, "Context: %lu\n", context_opened);
    fprintf(out, "Calls: %lu %lu %lu %lu\n", clos_call, spec_call, builtin_call, clos_call + spec_call+ builtin_call);


    /* seems to be ignored by the Java tool, kept anyway */
    fprintf(out, "Allocated: %lu %lu %lu %lu %lu %lu %lu\n", allocated_cons, allocated_prom, allocated_env, allocated_external, allocated_sexp, allocated_noncons, allocated_cons + allocated_prom + allocated_env + allocated_external + allocated_sexp + allocated_noncons);

    /* this is what the Java tool actually wants to see */
    fprintf(out, "AllocatedCons: %lu\n", allocated_cons);
    fprintf(out, "AllocatedConsPeak: %lu\n", allocated_cons_peak * sizeof(SEXPREC)); // convert to bytes too
    fprintf(out, "AllocatedNonCons: %lu\n", allocated_noncons);
    fprintf(out, "AllocatedEnv: %lu\n", allocated_env);
    fprintf(out, "AllocatedPromises: %lu\n", allocated_prom);
    fprintf(out, "AllocatedSXP: %lu\n", allocated_sexp);
    fprintf(out, "AllocatedExternal: %lu\n", allocated_external);
    fprintf(out, "AllocatedList: %lu %lu\n", allocated_list, allocated_list_elts);

#define REPORT_VECTOR(n, __t)\
    fprintf(out, "AllocatedVectors" n ": %lu %lu %lu %lu\n", allocated_vector ## __t, allocated_vector_elts ## __t, allocated_vector_size ## __t, allocated_vector_asize ## __t)
    REPORT_VECTOR(,);
    REPORT_VECTOR("Null",_null);
    REPORT_VECTOR("Small", _small);
    REPORT_VECTOR("Large", _large);
    REPORT_VECTOR("One", _one);

    /* allocation counts per node class */
    for (unsigned int i = 0; i < allocated_cell_len; i++) {
      fprintf(out, "Class%uAllocs: %lu\n", i, allocated_cell[i]);
    }

    /* and now the version that the Java tool expects */
    fprintf(out, "AllocatedSmallVectors: %lu %lu %lu %lu\n", allocated_vector_small, allocated_vector_elts_small, allocated_vector_size_small, allocated_vector_asize_small);
    fprintf(out, "AllocatedLargeVectors: %lu %lu %lu %lu\n", allocated_vector_large, allocated_vector_elts_large, allocated_vector_size_large, allocated_vector_asize_large);
    fprintf(out, "AllocatedOneVectors: %lu %lu %lu %lu\n", allocated_vector_one, allocated_vector_elts_one, allocated_vector_size_one, allocated_vector_asize_one);
    fprintf(out, "AllocatedNullVectors: %lu %lu %lu %lu\n",  allocated_vector_null,  allocated_vector_elts_null,  allocated_vector_size_null,  allocated_vector_asize_null);

    fprintf(out, "AllocatedStringBuffer: %lu %lu %lu\n", allocated_sb, allocated_sb_elts, allocated_sb_size);

    fprintf(out, "GC: %d\n", gc_count);

    //fprintf(out, "AllocatedList: %lu %lu\n", allocated_list, allocated_list_elts);

    fprintf(out, "Dispatch: %lu %lu\n", dispatchs, dispatchFailed);
    fprintf(out, "Duplicate: %lu %lu %lu\n", duplicate_object, duplicate_elts, duplicate1_elts);
    fprintf(out, "Named: %lu %lu %lu %lu\n", named_elts, named_promoted, named_downgraded, named_keeped);
    fprintf(out, "ApplyDefine: %lu %lu\n", apply_define, super_apply_define);
    fprintf(out, "DefineVar: %lu %lu\n", define_var, super_define_var);
    fprintf(out, "SetVar: %lu %lu\n", set_var, super_set_var);
    fprintf(out, "DefineUserDb: %lu\n", define_user_db);
    fprintf(out, "ErrCountAssign: %lu\n", err_count_assign);
    fprintf(out, "ErrorEvalSet: %lu %lu\n", do_set_allways - do_set_unique, do_super_set_allways - do_super_set_unique );
    fprintf(out, "AvoidedDup: %lu %lu\n", avoided_dup, need_dup);
}

void terminate_tracing() {
    // Stop tracing
    FCLOSE(bin_trace_file);
    traceR_is_active = 0;
    FCLOSE(trace_info->src_map_file);
    write_summary();
}

void write_summary() {
    FILE *summary_fp;
    char str[MAX_DNAME];
    sprintf(str, "%s/%s", trace_info->directory, SUMMARY_NAME);

    // Write a summary file
    summary_fp = fopen(str, "w");
    if (!summary_fp) {
	print_error_msg ("Couldn't open file '%s' for writing", str);
	return;
    }
    fprintf(summary_fp, "TraceDir: %s\n", trace_info->directory);
    fprintf(summary_fp, "FatalErrors: %u\n", fatal_err_cnt);
    fprintf(summary_fp, "TraceStackErrors: %u\n", stack_err_cnt);
    fprintf(summary_fp, "FinalContextStackHeight: %d\n", get_cstack_height());
    fprintf(summary_fp, "FinalContextStackFlushed: %d\n", stack_flush_cnt);
    fprintf(summary_fp, "MaxStackHeight: %d\n", max_stack_height);
    fprintf(summary_fp, "BytesWritten: %lu\n", bytes_written);
    fprintf(summary_fp, "EventsTraced: %lu\n", event_cnt);
    fprintf(summary_fp, "FuncsDecld: %u\n", func_decl_cnt);
    fprintf(summary_fp, "NullSrcrefs: %u\n", null_srcref_cnt);

    write_trace_summary(summary_fp);

    fclose(summary_fp);
}

static inline void print_ref(SEXP src, const char * file, long line, long col, long more1, long more2, long more3, long more4) {
    // TODO rename this 'moreX' in an more appropriate way
    FPRINTF(trace_info->src_map_file, "%#010lx %p %s %#lx %#lx %#lx %#lx %#lx %#lx\n",
	    bytes_written, src, file,
	    line, col,
	    more1, more2,
	    more3, more4);
}

void print_src_addr (SEXP src) {
    SEXP srcref, srcfile, filename;

    if (trace_info) {
	srcref = getAttrib(src, R_SrcrefSymbol);
	if (SXPEXISTS(srcref)) {
	    srcfile = getAttrib (srcref, R_SrcfileSymbol);
	    if (TYPEOF(srcfile) == ENVSXP) {
		filename = findVar(install ("filename"), srcfile);
		if (isString(filename) && length(filename)) {
		    /* print out the number of bytes written, address, srcfilename, starting line number, and starting column number */
		    // print the lazyLoads that occur during the bootstrap
		    print_ref(src, CHAR(STRING_ELT (filename, 0)),
			      INTEGER(srcref)[0], INTEGER(srcref)[1],
			      INTEGER(srcref)[2], INTEGER(srcref)[3],
			      INTEGER(srcref)[4], INTEGER(srcref)[5]);
		} else
		    print_error_msg("src_filename not string or 0 length\n");
	    } else
		print_error_msg("srcfile isn't an ENVSXP\n");
	} else { //srcref is NULL or R_NilValue
	    unsigned long int body;
	    unsigned int high, low;
	    null_srcref_cnt++;
	    body = (unsigned long int) BODY(src);
	    low = (unsigned int) body;
	    high = (unsigned int) (body >> 32);
	    print_ref(src, "UNKNOWN",
		      high, low,
		      high, low,
		      high, low);
	}
	func_decl_cnt++;
    }
    return;
}