/*
 *  r-instrumented : Various measurements for R
 *  Copyright (C) 2014  TU Dortmund Informatik LS XII
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
 *
 *  mallocmeasure.c: memory allocation measurements
 */

#ifndef MALLOCMEASURE_H
#define MALLOCMEASURE_H

extern unsigned int mallocmeasure_quantum;
extern size_t mallocmeasure_values[];
extern size_t mallocmeasure_current_slot;

void mallocmeasure_finalize(void);
void mallocmeasure_reset(void);
void mallocmeasure_kill(void);

#endif
