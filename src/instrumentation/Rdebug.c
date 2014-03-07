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

#include <Defn.h>

#include <Rdebug.h>

#ifdef ENABLE_SCOPING_DEBUG
//#undef ENABLE_SCOPING_DEBUG

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

void debugScope_activate(char* scopeName) {
    // standard linear list adding
    activeScopesLinList* newScope = malloc(sizeof(activeScopesLinList));
    newScope->next = activeScopes;
    strncpy(newScope->scopeName, scopeName, SCOPENAME_MAX_SIZE);
    (newScope->scopeName)[SCOPENAME_MAX_SIZE] = '\0'; // safety string termination

    activeScopes = newScope;
}

void debugScope_enableOutput() {
  globalEnable = TRUE;
}

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
		printf("This was root Scope!\n");
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
		    printf("This was root Scope!\n");
		    currentScope = NULL;
		} else {
		    currentScope = endingScope->parent;
		}
		// either way: the previous scope has ended
		free(endingScope);
		continue;
	    }
	    // currentScope == targetScope
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
	    vprintf(output,argumentpointer);
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
	    //printf("ERROR: loadJump - target unknown\n");
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

void debugScope_saveloadJump(jmp_buf givenJumpInfo,int jumpValue) {
    if (jumpValue == 0) { // if setjmp returns 0, the jump was setup
	debugScope_saveJump(givenJumpInfo);
    } else { // for everything else, we returned from jump
	debugScope_loadJump(givenJumpInfo);
    }
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


#endif // ENABLE_SCOPING_DEBUG
