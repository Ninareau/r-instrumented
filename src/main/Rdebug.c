#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h> // for isspace(int)
#include <stdarg.h> // for va_start, va_end

#include <Rdebug.h>

#ifdef ENABLE_SCOPING_DEBUG

static debugScope* currentScope = (debugScope*)NULL;
static activeScopesLinList* activeScopes = (activeScopesLinList*)NULL;
static jumpInfos_linlist* jumpInfos = (jumpInfos_linlist*)NULL;

void debugScope_activate(char* scopeName){
  activeScopesLinList* newScope = malloc(sizeof(activeScopesLinList));
  newScope->next = activeScopes;
  strncpy(newScope->scopeName,scopeName,SCOPENAME_MAX_SIZE);
  (newScope->scopeName)[SCOPENAME_MAX_SIZE]='\0'; // safety string termination
  
  activeScopes = newScope;
}

int debugScope_isActive(char* scopeName){
  // this function just iterates the linked list. There may be faster solutions
  activeScopesLinList* scopeSearcher = activeScopes;
  while(NULL != scopeSearcher){ // still some list left
    if (0==strcmp(scopeSearcher->scopeName,scopeName)){ // found
      return (1==1);
    }else{ // not (yet) found
      scopeSearcher = scopeSearcher->next;
    }
  }
  // not found (finally)
  return (0!=0);
}
  
  
  

void debugScope_readFile(char* fileName){
  DEBUGSCOPE_START("debugScope_readFile");
  FILE* configFile;
  configFile = fopen(fileName,"r");
  if (NULL == configFile){ // config file missing
    // only report if debug is on for this function
    DEBUGSCOPE_PRINT("Config File ");
    DEBUGSCOPE_PRINT(fileName);
    DEBUGSCOPE_PRINT(" could not be opened\n");
  }else{ // file could be opened
    char extractedLine[SCOPENAME_MAX_SIZE+1];
    while(1==1){
      fgets(extractedLine,SCOPENAME_MAX_SIZE, configFile);
      if (feof(configFile)){ // reached end of file
        break;
      }
      if (0==strncmp(extractedLine,"//",2)){ // comment line
        /*
         * scope names starting with "//" are considered
         * "commented out" and not to be activated!
         */
        continue;
      }
      unsigned int lineLength = strlen(extractedLine);
      while(
        (lineLength >0) &&
        (isspace(extractedLine[lineLength-1]))
        )
        {
          extractedLine[lineLength-1]='\0';
          lineLength--;
        }
      
      if(lineLength>0){ // finally: activate
        DEBUGSCOPE_PRINT("Activating Scope '%s'\n",extractedLine);
        
        debugScope_activate(extractedLine);
      }
    }
    // file was read completely
    fclose(configFile);
  }
  
  DEBUGSCOPE_END("debugScope_readFile");
  
}

#define JUSTJUMP_TEST
//#undef JUSTJUMP_TEST
/*
 * The Justjump-Test is a limited debug mode which is only used to check
 * whether longjumps from the R interpreter can be correctly detected.
 *
 * It maps the standard scope-start and scope-end methods to nothing
 * and only stores the jumpInfo in it's list (instead of full info).
 */


#ifndef JUSTJUMP_TEST

void debugScope_start(char* scopeName){
  debugScope* newScope = malloc(sizeof(debugScope));
  if (NULL == newScope){ // malloc failed
    printf("ERROR: Malloc failed for debug scope %s\n",scopeName);
    return;
  }
  newScope->scopeName = scopeName;
  if (NULL == currentScope){ // first Scope
    newScope->parent = NULL;
    newScope->depth  = 0;
  }else{
    newScope->parent = currentScope;
    newScope->depth = newScope->parent->depth + 1;
  }
  newScope->enabled = debugScope_isActive(scopeName);

  currentScope = newScope;
  
  if (currentScope->enabled){
    printf("[%u] -> ENTER: %s\n",newScope->depth, scopeName);
  }
}
 
void debugScope_end(char* scopeName){
  if (NULL == currentScope){
    printf("Trying to exit scope %s but current Scope is NULL - this should not happen!\n",scopeName);
  }else{ // current scope exists
    if (0!=strcmp(scopeName,currentScope->scopeName)){ // not equal
      printf(
        "Trying to exit scope %s but current Scope is %s - this should not happen!\n",
        scopeName, 
        currentScope->scopeName
        );
    }else{ // scopenames match
      if (currentScope->enabled){
        printf("[%u] <- EXIT: %s\n",currentScope->depth, scopeName);
      }
      debugScope* endingScope = currentScope;
      if (NULL == endingScope->parent){
        printf("This was root Scope!\n");
        currentScope=NULL;
      }else{
        currentScope = endingScope->parent;
      }
      // either way: the previous scope has ended
      free(endingScope);
    }
  }
}

void debugScope_saveJump(jmp_buf givenJumpInfo){
  if(NULL != currentScope){ // safety check
    jumpInfos_linlist* newJumpInfo = malloc(sizeof(jumpInfos_linlist));
    if(NULL == newJumpInfo){
      printf(
        "ERROR: malloc failed for jumpinfos in %s\n",
        currentScope->scopeName
        );
      return;
    }
    memcpy(newJumpInfo->jumpInfo,givenJumpInfo,sizeof(jmp_buf));
    newJumpInfo->targetScope = currentScope;
    newJumpInfo->next = jumpInfos;
    
    jumpInfos = newJumpInfo;
    //debugScope_print(
    printf(
      "# Saved a jump target in %s\n",
      currentScope->scopeName
      );
  }else{ // NULL
    printf("Current Scope is NULL - this should not happen!\n");
  }
}  
  
void debugScope_loadJump(jmp_buf givenJumpInfo){
  if(NULL != currentScope){ // safety check
    //unsigned int countedJumpInfos = 0;
    jumpInfos_linlist* jumpInfoWalker = jumpInfos;
    while(1==1){ // forever - until something happens
      if(NULL == jumpInfoWalker){ // iterated complete table - but not found
        printf("ERROR: loadJump but target unknown\n");
        //printf("ERROR: loadJump but target unknown - %d entrys\n",countedJumpInfos);
        return;
      }
      if(0!=memcmp(jumpInfoWalker->jumpInfo, givenJumpInfo,sizeof(jmp_buf))){// not equal
        // not found - yet
        jumpInfoWalker = jumpInfoWalker->next;
        //countedJumpInfos++;
        continue;
      }
      // found
      break;
    }
    debugScope* targetScope = jumpInfoWalker->targetScope;
    printf("loadJump - with target %s\n",targetScope->scopeName);
    // todo: stack correction - remove scopes from linlist
    
    // todo: jumpInfo correction - remove jumpInfo(s?) from linlist
  }else{ // NULL
    printf("Current Scope is NULL - this should not happen!\n");
  }
}  
void debugScope_print(char* output,...){
  if (NULL != currentScope){ // safety check
    if ((1==1)==currentScope->enabled){
      va_list argumentpointer;
      va_start(argumentpointer,output);
      vprintf(output,argumentpointer);
      va_end(argumentpointer);
    }else{
      /* debug scope not enabled - do not print */
    }
  }else{ // NULL
    printf("Current Scope is NULL - this should not happen!\n");
  }
}   

#endif // JUSTJUMP_TEST

#ifdef JUSTJUMP_TEST 
void debugScope_start(char* scopeName){
  /* do nothing */
}
void debugScope_end(char* scopeName){
  /* do nothing */
}
void debugScope_print(char* output,...){
  /* do nothing */
}

#define PRINT_JUMP_INFO
//#undef PRINT_JUMP_INFO
void printJumpInfo(jmp_buf givenJumpInfo){
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
      printf("%02X",jumpInfoBin[jumpInfoIterator]);
    }
  }
  #endif // PRINT_JUMP_INFO
}

void debugScope_saveJump(jmp_buf givenJumpInfo){
  jumpInfos_linlist* newJumpInfo = malloc(sizeof(jumpInfos_linlist));
  if(NULL == newJumpInfo){
    printf(
      "ERROR: malloc failed for jumpinfos\n",
      currentScope->scopeName
      );
    return;
  } // null from malloc - failed
  
  // copy necessary information
  memcpy(newJumpInfo->jumpInfo,givenJumpInfo,sizeof(jmp_buf));
  
  // push to stack
  newJumpInfo->next = jumpInfos;
  jumpInfos = newJumpInfo;
  printf("# Saved a jump target:");
  printJumpInfo(givenJumpInfo);
    
  printf("\n");
}  
void debugScope_loadJump(jmp_buf givenJumpInfo){
  printf("# try to loadJump");
  printJumpInfo(givenJumpInfo);
  printf("\n");
  unsigned int countedJumpInfos = 0;
  jumpInfos_linlist* jumpInfoWalker = jumpInfos;
  while(1==1){ // forever - until something happens
    if(NULL == jumpInfoWalker){ // iterated complete table - but not found
      //printf("ERROR: loadJump - target unknown\n");
      printf("ERROR: loadJump but target unknown - %d entrys\n",countedJumpInfos);
      return;
    }
    if(0!=memcmp(jumpInfoWalker->jumpInfo, givenJumpInfo,sizeof(jmp_buf))){// not equal
      // not found - yet
      jumpInfoWalker = jumpInfoWalker->next;
      countedJumpInfos++;
      continue;
    }
    // found
    break;
  }
  printf("loadJump - target found\n");
  printf("loadJump - target found, %d entrys iterated\n",countedJumpInfos);
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
  
void debugScope_printStack(){
  printf("--- START Debug Scope Stack ---\n");
  debugScope* scopeIterator = currentScope;
  while(NULL != scopeIterator){
    printf("* %s\n",scopeIterator->scopeName);
    scopeIterator = scopeIterator->parent;
  }
  printf("--- END Debug Scope Stack ---\n");
}
  

#endif // ENABLE_SCOPING_DEBUG
