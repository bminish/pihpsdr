/* Copyright (C)
*  2026 - Brendan Minish, EI6IZ
*
*   This program is free software: you can redistribute it and/or modify
*   it under the terms of the GNU General Public License as published by
*   the Free Software Foundation, either version 3 of the License, or
*   (at your option) any later version.
*
*   This program is distributed in the hope that it will be useful,
*   but WITHOUT ANY WARRANTY; without even the implied warranty of
*   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*   GNU General Public License for more details.
*
*   You should have received a copy of the GNU General Public License
*   along with this program.  If not, see <https://www.gnu.org/licenses/>.
*
*/


#ifndef _RADE_NATIVE_MENU_H_
#define _RADE_NATIVE_MENU_H_

#include <gtk/gtk.h>

void rade_native_menu(GtkWidget *parent);

//
// Starts publishing piHPSDR's RADE state for the separate RADE Reporter
// program (see rade_native_menu.c). Call once the settings are restored.
//
void rade_native_state_start(void);

#endif
