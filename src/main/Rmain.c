/*
 *  R : A Computer Language for Statistical Data Analysis
 *  Copyright (C) 1995, 1996  Robert Gentleman and Ross Ihaka
 *  Copyright (C) 1997--2005  The R Core Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, a copy is available at
 *  http://www.r-project.org/Licenses/
 */

int Rf_initialize_R(int ac, char **av); /* in ../unix/system.c */

#include <Rinterface.h>
#include <Rdebug.h>

extern int R_running_as_main_program;   /* in ../unix/system.c */

int main(int ac, char **av)
{
    //DEBUGSCOPE_ENABLEOUTPUT();
    DEBUGSCOPE_ACTIVATE("debugScope_readFile");
    /*
    DEBUGSCOPE_READFILE("debug.conf");
    This has been moved to a command line option (--debugscope-file)
    see system.c for more information
    */
    DEBUGSCOPE_START("main");
    R_running_as_main_program = 1;
    DEBUGSCOPE_PRINT("Number of Arguments: %d\n",ac);
    Rf_initialize_R(ac, av);
    Rf_mainloop(); /* does not return */
    DEBUGSCOPE_END("main");
    return 0;
}

	/* Declarations to keep f77 happy */

int MAIN_(int ac, char **av)  {return 0;}
int MAIN__(int ac, char **av) {return 0;}
int __main(int ac, char **av) {return 0;}
