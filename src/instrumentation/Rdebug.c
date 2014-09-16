/*
 * ------------------------------------------------------------------
 * This material is distributed under the GNU General Public License
 * Version 2. You may review the terms of this license at
 * http://www.gnu.org/licenses/gpl-2.0.html
 *
 * Copyright (c) 2013, Markus Kuenne
 * TU Dortmund University
 *
 * All rights reserved.
 * ------------------------------------------------------------------
 */


#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h> // for isspace(int)
#include <stdarg.h> // for va_start, va_end

#define R_USE_SIGNALS 1 // or else RCNTXT will not be declared
#include <Defn.h>

#include <Rdebug.h>

#ifdef HAVE_DEBUGSCOPES

//#define REPORT_END_OF_ROOT_SCOPE
#undef REPORT_END_OF_ROOT_SCOPE
/*
 * About REPORT_END_OF_ROOT_SCOPE:
 *
 * Normally, the "this was root scope"-warning is helpful
 * as most programs terminate the programs before ending the root scope
 * -- exiting via return or exit without calling debugscope_end("main").
 * Ending the root scope normally is a sign that something went wrong
 * (e.g. ending scopes that never started).
 *
 * However, the debugscopes in the R interpreter do not form a tree with
 * a single root so at some point at startup, the "last" scope will end
 * with no parent remaining. Later on, a new "root" scope is started.
 *
 * This normally results in the warning printed - even several times as
 * the interpreter itself is called for libraries. To prevent this, the
 * warning can be (as it was) disabled.
 * 
 * Note that the debugscopes have another safeguard against begin/end-
 * descrepancies: At each endScope the name of the scope is checked against
 * the scopename that is expected to be open at that point. Normally,
 * forgetting to close or open scopes (or to correct the stack after longjumps)
 * should result in a lot of error messages thus alerting the programmer
 * to the problem.
 */


void extractFunctionName(char* extraction, SEXP environment){
    int i;
    SEXP t = getAttrib(environment, R_SrcrefSymbol);
    if (!isInteger(t)) {
	t = deparse1w(environment, 0, DEFAULTDEPARSE);
    } else {
	PROTECT(t = lang2(install("as.character"), t));
	t = eval(t, R_BaseEnv);
	UNPROTECT(1);
    }
    PROTECT(t);
    for (i = 0; i < LENGTH(t); i++){
	//Rprintf("%s\n", CHAR(STRING_ELT(t, i))); /* translated */
	strncpy(extraction, CHAR(STRING_ELT(t, i)), SCOPENAME_MAX_SIZE);
	extraction[SCOPENAME_MAX_SIZE] = '\0'; // safety string termination
    }
    UNPROTECT(1);
}


static debugScope* currentScope = (debugScope*)NULL;
static activeScopesLinList* activeScopes = (activeScopesLinList*)NULL;
static jumpInfos_linlist* jumpInfos = (jumpInfos_linlist*)NULL;
static Rboolean globalEnable = FALSE;
static Rboolean contextOutPrepared = FALSE;
static FILE* outStream = NULL;
static FILE* contextOutStream = NULL;

void debugScope_activate(char* scopeName) {
    // standard linear list adding
    activeScopesLinList* newScope = malloc(sizeof(activeScopesLinList));
    newScope->next = activeScopes;
    strncpy(newScope->scopeName, scopeName, SCOPENAME_MAX_SIZE);
    (newScope->scopeName)[SCOPENAME_MAX_SIZE] = '\0'; // safety string termination

    activeScopes = newScope;
}

void debugScope_setFile(char* outFile){
    outStream = fopen(outFile,"w");
    if (NULL == outStream){ // opening failed
        fprintf(stderr,"Opening %s as debugscope-logfile failed\n",outFile);
        // and reset to stdout
        outStream = stdout;
    }else{
        printf("opened debugScope-outfile\n");
    }
    
}

void debugScope_prepareContextOutEnable(){
    contextOutPrepared=TRUE;
}

void debugScope_enableContextOut(){
    if(
        (TRUE==contextOutPrepared) &&
        (NULL == contextOutStream)
        )
    {
        contextOutStream = stdout;
    }
}


void debugScope_enableOutput() {
  globalEnable = TRUE;
  if (NULL == outStream){
      outStream = stdout;
  }
}

void debugScope_setContextFile(char* outFile){
    contextOutStream = fopen(outFile,"w");
    if (NULL == contextOutStream){ // opening failed
        fprintf(stderr,"Opening %s as contextscope-logfile failed\n",outFile);
        // and set to null
        contextOutStream = NULL;
    }else{
        // everything is fine
    }
} // debugScope_setContextFile
        

void debugScope_disableOutput() {
    DEBUGSCOPE_START("debugScope_disableOutput");
    globalEnable = FALSE;
    DEBUGSCOPE_END("debugScope_disableOutput");
}

void debugScope_deactivate(char* scopeName) {
    activeScopesLinList* parent = NULL;
    activeScopesLinList* scopeIterator = activeScopes;
    while (1) {
	if (scopeIterator == NULL) { // end of list reached, terminate
	    return;
	    // could also be "break" if further stuff is required
	}
	if (strcmp(scopeIterator->scopeName, scopeName) != 0) { // scopename mismatch
	    // iterate!
	    parent = scopeIterator;
	    scopeIterator = scopeIterator->next;
	    continue;
	}
	// at this point, we found an "active"-entry with this name
	activeScopesLinList* toDel = scopeIterator; // store the one to del
	scopeIterator = scopeIterator->next; // iterate to next
	free(toDel); // delete the found scope

	/*
	 * next, fix the pointer in the previous entry
	 *
	 * Note: ScopeIterator may be NULL at this point but this is a correct
	 * and sensible "next"-pointer
	 */
	if (parent == NULL) { // this was first in list
	    activeScopes = scopeIterator; // so set the head
	} else { // not first in list
	    parent->next = scopeIterator; // so fix in the last entry
	}
	continue; // iterate!
    }
}


Rboolean debugScope_isActive(char* scopeName) {
    if (!globalEnable){ // globally disabled
	return FALSE;
    }
    // this function just iterates the linked list. There may be faster solutions
    activeScopesLinList* scopeSearcher = activeScopes;
    while (scopeSearcher != NULL) { // still some list left
	if (strcmp(scopeSearcher->scopeName, scopeName) == 0) { // found
	    return TRUE;
	} else { // not (yet) found
	    scopeSearcher = scopeSearcher->next;
	}
    }
    // not found (finally)
    return FALSE;
}

Rboolean debugScope_isCurrentActive() {
    if (!globalEnable) { // globally disabled, so stop at once
	return FALSE;
    }
    if (currentScope == NULL) { // no current scope - should not happen
	// but just return "no"
	return FALSE;
    }

    return currentScope->enabled;
}




void debugScope_readFile(char* fileName) {
    DEBUGSCOPE_START("debugScope_readFile");
    FILE* configFile;
    configFile = fopen(fileName,"r");
    if (configFile == NULL) { // config file missing
	// only report if debug is on for this function
	DEBUGSCOPE_PRINT("Config File ");
	DEBUGSCOPE_PRINT(fileName);
	DEBUGSCOPE_PRINT(" could not be opened\n");
    } else { // file could be opened
	char extractedLine[SCOPENAME_MAX_SIZE+1];
	while (1) {
	    fgets(extractedLine, SCOPENAME_MAX_SIZE, configFile);
	    if (feof(configFile)) { // reached end of file
		break;
	    }
	    if (strncmp(extractedLine, "//", 2) == 0) { // comment line
		/*
		 * scope names starting with "//" are considered
		 * "commented out" and not to be activated!
		 */
		continue;
	    }
	    unsigned int lineLength = strlen(extractedLine);
	    while (
		   (lineLength > 0) &&
		   (isspace(extractedLine[lineLength-1]))
		   )
		{
		    extractedLine[lineLength-1] = '\0';
		    lineLength--;
		}

	    if (lineLength > 0) { // finally: activate
		DEBUGSCOPE_PRINT("Activating Scope '%s'\n", extractedLine);

		debugScope_activate(extractedLine);
	    }
	}
	// file was read completely
	fclose(configFile);
    }

    DEBUGSCOPE_END("debugScope_readFile");
}

//#define JUSTJUMP_TEST
#undef JUSTJUMP_TEST
/*
 * The Justjump-Test is a limited debug mode which is only used to check
 * whether longjumps from the R interpreter can be correctly detected.
 *
 * It maps the standard scope-start and scope-end methods to nothing
 * and only stores the jumpInfo in it's list (instead of full info).
 */


#ifndef JUSTJUMP_TEST

void debugScope_start(char* scopeName) {
    debugScope* newScope = malloc(sizeof(debugScope));
    if (newScope == NULL) { // malloc failed
	printf("ERROR: Malloc failed for debug scope %s\n",scopeName);
	return;
    }
    strncpy(newScope->scopeName, scopeName, SCOPENAME_MAX_SIZE);
    (newScope->scopeName)[SCOPENAME_MAX_SIZE] = '\0';
    //newScope->scopeName = scopeName;
    if (currentScope == NULL) { // first Scope
	newScope->parent = NULL;
	newScope->depth  = 0;
    } else {
	newScope->parent = currentScope;
	newScope->depth  = newScope->parent->depth + 1;
    }
    newScope->enabled = debugScope_isActive(scopeName);

    currentScope = newScope;

    if (currentScope->enabled) {
	debugScope_print("[%u] -> ENTER: %s\n",newScope->depth, scopeName);
	//printwhere();
	//R_OutputStackTrace(FILE *file);
	#define PRINT_STACKS_ON_DEBUGSSCOPESTART
	//#undef PRINT_STACKS_ON_DEBUGSSCOPESTART
	#ifdef PRINT_STACKS_ON_DEBUGSSCOPESTART
	{
            debugScope_flatStack();
        }
        #endif //  PRINT_STACKS_ON_DEBUGSSCOPESTART
        debugScope_flatStack();
    }
}


//#define TERMINATE_ON_SCOPING_PROBLEM
#undef TERMINATE_ON_SCOPING_PROBLEM

void debugScope_end(char* scopeName) {
    if (currentScope == NULL) {
	printf("Trying to exit scope %s but current Scope is NULL - this should not happen!\n",scopeName);
    } else { // current scope exists
	if (strcmp(scopeName, currentScope->scopeName) != 0) { // not equal
	    printf(
		   "Trying to exit scope %s but current Scope is %s - this should not happen!\n",
		   scopeName,
		   currentScope->scopeName
		   );
#ifdef TERMINATE_ON_SCOPING_PROBLEM
	    {
		static unsigned int scopingerrorcounter = 0;
		if (scopingerrorcounter > 200) {
		    exit(EXIT_FAILURE);
		} else {
		    scopingerrorcounter++;
		}
	    }
#endif // TERMINATE_ON_SCOPING_PROBLEM

	} else { // scopenames match
	    debugScope_print("[%u] <- EXIT: %s\n", currentScope->depth, scopeName);
	    debugScope* endingScope = currentScope;
	    if (endingScope->parent == NULL) {
#ifdef REPORT_END_OF_ROOT_SCOPE
		printf("This was root Scope!\n");
#endif // REPORT_END_OF_ROOT_SCOPE		
		currentScope = NULL;
	    } else {
		currentScope = endingScope->parent;
	    }
	    // either way: the previous scope has ended
	    free(endingScope);
	}
    }
}

void debugScope_saveJump(jmp_buf givenJumpInfo) {
    if (currentScope != NULL) { // safety check
	jumpInfos_linlist* newJumpInfo = malloc(sizeof(jumpInfos_linlist));
	if (newJumpInfo == NULL) {
	    printf(
		   "ERROR: malloc failed for jumpinfos in %s\n",
		   currentScope->scopeName
		   );
	    return;
	}
	memcpy(newJumpInfo->jumpInfo, givenJumpInfo, sizeof(jmp_buf));
	newJumpInfo->targetScope = currentScope;
	newJumpInfo->next        = jumpInfos;

	jumpInfos = newJumpInfo;
	debugScope_print(
			 "# Saved a jump target in %s\n",
			 currentScope->scopeName
			 );
    } else { // NULL
	printf("Current Scope is NULL - this should not happen!\n");
    }
}

void debugScope_loadJump(jmp_buf givenJumpInfo) {
    if (currentScope != NULL) { // safety check
	unsigned int countedJumpInfos = 0;
	jumpInfos_linlist* jumpInfoWalker = jumpInfos;
	while (1) { // forever - until something happens
	    if (jumpInfoWalker == NULL) { // iterated complete table - but not found
		printf("ERROR: loadJump but target unknown - %d entrys\n",countedJumpInfos);
		return;
	    }
	    if (memcmp(jumpInfoWalker->jumpInfo, givenJumpInfo, sizeof(jmp_buf)) != 0) { // not equal
		// not found - yet
		jumpInfoWalker = jumpInfoWalker->next;
		countedJumpInfos++;
		continue;
	    }
	    // found
	    break;
	}
	debugScope* targetScope = jumpInfoWalker->targetScope;
	if (currentScope == targetScope) {
	    debugScope_print("[%u] ^ LONGJUMP IN: %s\n",targetScope->depth, targetScope->scopeName);
	    return;
	}

	while (1) {
	    if (currentScope == NULL) {
		printf("ERROR: loadJump-target not found on scope-stack\n");
		return;
	    }
	    if (currentScope != targetScope) {
		debugScope* endingScope = currentScope;
		if (endingScope->parent == NULL) {
#ifdef REPORT_END_OF_ROOT_SCOPE
		    printf("This was root Scope!\n");
#endif // REPORT_END_OF_ROOT_SCOPE		    
		    currentScope = NULL;
		} else {
		    currentScope = endingScope->parent;
		}
		// either way: the previous scope has ended
		free(endingScope);
		continue;
	    }

	    break;
	}
	debugScope_print("[%u] <-- ENTER via longjump: %s\n",currentScope->depth, currentScope->scopeName);
    } else { // NULL
	printf("Current Scope is NULL - this should not happen!\n");
    }
}

void debugScope_print(char* output,...) {
    if (currentScope != NULL) { // safety check
	if (debugScope_isCurrentActive()) {
	    va_list argumentpointer;
	    va_start(argumentpointer,output);
	    vfprintf(outStream,output,argumentpointer);
	    va_end(argumentpointer);
	} else {
	    /* debug scope not enabled - do not print */
	}
    } else { // NULL
	printf("Current Scope is NULL - this should not happen!\n");
    }
}

#endif // JUSTJUMP_TEST

#ifdef JUSTJUMP_TEST
/*
 * this mode deactives the regular scopestarts and ends and just
 * tries to save and load jumpinfos - but is much more verbose than
 * the regular mode.
 *
 * It was mainly used for initial debugging of the jumpinfo-map
 */
void debugScope_start(char* scopeName) {
  /* do nothing */
}
void debugScope_end(char* scopeName) {
  /* do nothing */
}
void debugScope_print(char* output,...) {
  /* do nothing */
}

//#define PRINT_JUMP_INFO
#undef PRINT_JUMP_INFO
void printJumpInfo(jmp_buf givenJumpInfo) {
    unsigned char* jumpInfoBin = (unsigned char*) givenJumpInfo;
#ifdef PRINT_JUMP_INFO
    {
	unsigned int jumpInfoIterator;
	for (
	     jumpInfoIterator  = 0;
	     jumpInfoIterator  < sizeof(jumpInfos_linlist);
	     jumpInfoIterator++
	     )
	    {
		printf("%02X", jumpInfoBin[jumpInfoIterator]);
	    }
    }
#endif // PRINT_JUMP_INFO
}

void debugScope_saveJump(jmp_buf givenJumpInfo) {
    jumpInfos_linlist* newJumpInfo = malloc(sizeof(jumpInfos_linlist));
    if (newJumpInfo == NULL) {
	printf(
	       "ERROR: malloc failed for jumpinfos\n",
	       currentScope->scopeName
	       );
	return;
    } // null from malloc - failed

    // copy necessary information
    memcpy(newJumpInfo->jumpInfo, givenJumpInfo, sizeof(jmp_buf));

    // push to stack
    newJumpInfo->next = jumpInfos;
    jumpInfos = newJumpInfo;
    printf("# Saved a jump target:");
    printJumpInfo(givenJumpInfo);

    printf("\n");
}

void debugScope_loadJump(jmp_buf givenJumpInfo) {
    printf("# try to loadJump");
    printJumpInfo(givenJumpInfo);
    printf("\n");
    unsigned int countedJumpInfos = 0;
    jumpInfos_linlist* jumpInfoWalker = jumpInfos;
    while (1) { // forever - until something happens
	if (jumpInfoWalker == NULL) { // iterated complete table - but not found
	    printf("ERROR: loadJump but target unknown - %d entrys\n",countedJumpInfos);
	    return;
	}
	if (memcmp(jumpInfoWalker->jumpInfo, givenJumpInfo, sizeof(jmp_buf)) != 0) { // not equal
	    // not found - yet
	    jumpInfoWalker = jumpInfoWalker->next;
	    countedJumpInfos++;
	    continue;
	}
	// found
	break;
    }
    printf("loadJump - target found\n");
    printf("loadJump - target found, %d entrys iterated\n", countedJumpInfos);
}

#endif // JUSTJUMP_TEST

/*
 * Reflekting on the jumpinfo-table, when elements should be deleted.
 *
 * Technically, this jumpinfo-table is bound to grow over an Interpreter run.
 * It might sound like a good idea to delete "no longer needed"-entries.
 *
 * Unfortunately, there is no safe way to determine which entries are
 * no longer needed.
 *
 * We might iterate the R environments whether any of them still have the
 * jumpinfo (jmp_buf) in question but this would mean implementing some
 * kind of garbage collection.
 * We might delete entries when they are loaded - but it is possible to longjump
 * to the same jmp_buf more than once.
 * Even if we could do a complete collection of all jmp_buf in memory, there
 * is nothing preventing a programmer to buffer some of these on disk and re-load
 * them when needed.
 *
 * So, although it is obvious that some jmp_buf will never be used, again
 * (e.g. the setup_Rmainloop in main.c overwrites R_Toplevel.cjmpbuf later),
 * it can not be determined from just the debugscope-point of view.
 *
 * We could, however, implement some kind of "DEBUGSCOPE_CLEARJUMP" so the
 * programmer can mark jmp_bufs that will never be needed, again.
 *
 * Also, the same jmp_buf should not be needed more than once in the table.
 * However, killing dupes might be less efficient than just accepting them -
 * newer entries for the same jmp_buf will be found first, as we are
 * using a stack as map structure.
 */

 
/*
 * This function should no longer be used. If it is used, a violation of 
 * ISO 9899:1999 (chapter 7.13.1.1 at 4+5) is very likely
 *
 * According to this, the return value of setjmp has to be used directly and
 * storing it is not allowed. However, as the return value would be used
 * in the conditional *and* saveloadJump, it would need to be stored.
 *
 * The "cleaner" approach is to use saveJump and loadJump in the appropiate
 * blocks.
 */
/*
void debugScope_saveloadJump(jmp_buf givenJumpInfo,int jumpValue) {
    if (jumpValue == 0) { // if setjmp returns 0, the jump was setup
	debugScope_saveJump(givenJumpInfo);
    } else { // for everything else, we returned from jump
	debugScope_loadJump(givenJumpInfo);
    }
}
*/

void debugScope_printContextStack(){
    RCNTXT *cptr;
    if(NULL!=contextOutStream){
        for (
            cptr = R_GlobalContext; // start at global
            NULL != cptr; // still one left
            cptr = cptr->nextcontext // iterate to next
            )
        { // for all contexts - from global to last one
            if (
                (cptr->callflag & (CTXT_FUNCTION | CTXT_BUILTIN)) && 
                (TYPEOF(cptr->call) == LANGSXP)
                )
            {
                SEXP fun = CAR(cptr->call);
                fprintf(contextOutStream,"\"");
                if(SYMSXP == TYPEOF(fun)){
                    fprintf(contextOutStream,CHAR(PRINTNAME(fun)));
                }else{
                    fprintf(contextOutStream,"%p",fun);
                    //fprintf(contextOutStream,"<Anonymous>");
                }
                fprintf(contextOutStream,"\" ");
            } // if 
        } // for all contexts
        fprintf(contextOutStream,"\n");
    } // if contextOutStream != NULL
}


void debugScope_printStack() {
    printf("--- START Debug Scope Stack ---\n");
    debugScope* scopeIterator = currentScope;
    while (scopeIterator != NULL) {
	printf("* %s\n",scopeIterator->scopeName);
	scopeIterator = scopeIterator->parent;
    }
    printf("--- END Debug Scope Stack ---\n");
}

void debugScope_flatStack() {
    printf("Debug Scope Stack: ");
    debugScope* scopeIterator = currentScope;
    while (scopeIterator != NULL) {
	printf(" <-  %s",scopeIterator->scopeName);
	scopeIterator = scopeIterator->parent;
    }
    printf("--- \n");
}

void debugScope_printBeginPseudoContext(char* contextName){
    char output[3*SCOPENAME_MAX_SIZE];
    strncpy(output,oldContextPrefix,SCOPENAME_MAX_SIZE);
    SEXP from = CAR(R_GlobalContext->call);
    if(TYPEOF(from) == SYMSXP){
        strncat(output,translateChar(PRINTNAME(from)),SCOPENAME_MAX_SIZE);
    }else{
        strcat(output,"<Anonymous>");
    }
    if(NULL != contextOutStream){
        fprintf(contextOutStream,"-> %s ",output);
    }
    strncpy(output,currentContextPrefix,SCOPENAME_MAX_SIZE);
    strcat(output,contextName);
    if(NULL != contextOutStream){
        fprintf(contextOutStream,"-> %s\n ",output);
    }
    // set current prefix to "last seen"
    strncpy(oldContextPrefix,currentContextPrefix,SCOPENAME_MAX_SIZE);
}
        
/*
 * Grouping functions/contexts may be useful in two ways: 
 * - grouping functions from the same namespace. 
 *   That way, a programmer can see that a lot of calls (and maybe a lot of time) is spent in "his"
 *   functions (his own namespace) and decide to improve his code. It may also show
 *   that another namespace/package is responsible for most of the cycles
 *   and he can only improve overall performance by choosing another library/package/namespace
 *   or by reducing the number of calls to this
 * - grouping functions by program "phases".
 *   This time, the programmer has to specify points in the program in which
 *   the "phase" is changed (e.g. initialisation, calculation 1, calculation2, etc.)
 *   Instrumented R already uses "init::", "init2::", "ReplCons", "init3::" but
 *   as it turns out, there were no contexts opened in "init" or "init3" and
 *   the only main difference was between "init2::" (before starting user program)
 *   and "ReplCons::" (after starting user program). At least, this allowed to
 *   simply hide/delete all contexts in "init2::" and get a diagrom for the user program.
 *
 * We still have to decide on how to group by both criteria (priorizing phases?)
 * and how to split timeR-data onto different phases (As of now, timeR does not 
 * use phases-prefixes)
 * The later problem is also the reason for deactivating context prefixes (at the moment): 
 * They hinder when trying to merge context-scope data with timer-data.
 */
#undef PRINT_CONTEXT_PREFIX

void debugScope_printBeginContext(SEXP from, SEXP to){
    if(0!=ignoreNextContext){
        ignoreNextContext=0;
        return;
    }
    //if (SYMSXP != TYPEOF(fun2)){
    if(0==1){
        // do not print anonymous calls
    }else{
        char output[3*SCOPENAME_MAX_SIZE];

        output[0]='\0'; // have a valid cstring even without prefix!
#ifdef PRINT_CONTEXT_PREFIX        
        strncpy(output,oldContextPrefix,SCOPENAME_MAX_SIZE);
#endif // PRINT_CONTEXT_PREFIX
        
        if(TYPEOF(from) == SYMSXP){
            strncat(output,translateChar(PRINTNAME(from)),SCOPENAME_MAX_SIZE);
        }else{// anonymous - maybe try to lookup
            char buffer[SCOPENAME_MAX_SIZE];
            sprintf(buffer,"%p",from);
            strcat(output,buffer);
            //strcat(output,"<Anonymous>");
        }
        if(NULL!=contextOutStream){
            /*
             * note: we still want to keep information
             * about the last prefix - so we cannot cancel
             * this function early.
             */
            fprintf(contextOutStream,"-> %s ",output);
        }
        
        // to is always from current context
        output[0]='\0'; // have a valid cstring even without prefix!
#ifdef PRINT_CONTEXT_PREFIX        
        strncpy(output,currentContextPrefix,SCOPENAME_MAX_SIZE);
#endif // PRINT_CONTEXT_PREFIX
        if(TYPEOF(to) == SYMSXP){
            strncat(output,translateChar(PRINTNAME(to)),SCOPENAME_MAX_SIZE);
        }else{ // anonymous - maybe try to lookup
            char buffer[SCOPENAME_MAX_SIZE];
            sprintf(buffer,"%p",from);
            strcat(output,buffer);
            
            //strcat(output,"<Anonymous>");
        }
        if(NULL!=contextOutStream){
            fprintf(contextOutStream,"-> %s\n",output);
        }
        
        // set current prefix to "last seen"
        strncpy(oldContextPrefix,currentContextPrefix,SCOPENAME_MAX_SIZE);
        
    }
}

Rboolean debugScope_contextScopesIsActive(){
    if(NULL!=contextOutStream){
        return TRUE;
    }else{
        return FALSE;
    }
}
void debugScope_ignoreNextContext(){
  /*
   * eval.c::applyClosure seems to call "begincontext" just to have something
   * that error has access to. There is nothing evaluated and the context
   * is ended shortly after - and then begun again.
   *
   * This can result in a "fake" context-begin showing up on logging
   * so we need to cancel one. 
   *
   * This function is used to indicate that the next "begincontext" is to 
   * be ignored.
   */
  ignoreNextContext=1;
}


void debugScope_printEndContext(SEXP from, SEXP to){
    //#define PRINT_CONTEXT_RETURNS
    #undef PRINT_CONTEXT_RETURNS
    #ifdef PRINT_CONTEXT_RETURNS
    // return is necessary for graph construction
    // (at the moment. Could be improved in the future)
    { // printing context call
        printf("<- %s ",
                TYPEOF(to) == SYMSXP ? translateChar(PRINTNAME(to)) :
                "<Anonymous>");
        
        printf("<- %s\n",
                TYPEOF(from) == SYMSXP ? translateChar(PRINTNAME(from)) :
                "<Anonymous>");
    }
    #endif // PRINT_CONTEXT_RETURNS
    
}

void debugScope_setContextPrefix(const char* newPrefix){
    //strncpy(oldContextPrefix,currentContextPrefix,SCOPENAME_MAX_SIZE);
    strncpy(currentContextPrefix,newPrefix,SCOPENAME_MAX_SIZE);
}
    
    

#endif // HAVE_DEBUGSCOPES
