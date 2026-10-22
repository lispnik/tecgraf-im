/** \file
 * \brief camraw format Lua 5 Binding
 *
 * See Copyright Notice in im_lib.h
 */

#include <stdlib.h>
#include <stdio.h>

#include "im.h"
#include "im_image.h"
#include "im_format_camraw.h"

#include <lua.h>
#include <lauxlib.h>

#include "imlua.h"
#include "imlua_aux.h"


static int imlua_FormatRegisterCAMRAW(lua_State *L)
{
  (void)L;
  imFormatRegisterCAMRAW();
  return 0;
}

static const struct luaL_Reg imlib[] = {
  {"FormatRegisterCAMRAW", imlua_FormatRegisterCAMRAW},
  {NULL, NULL},
};


static int imlua_camraw_open (lua_State *L)
{
  imFormatRegisterCAMRAW();
  imlua_register_lib(L, imlib);   /* leave "im" table at the top of the stack */
  return 1;
}

int luaopen_imlua_camraw(lua_State *L)
{
  return imlua_camraw_open(L);
}
