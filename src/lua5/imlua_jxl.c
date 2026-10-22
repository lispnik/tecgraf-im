/** \file
 * \brief jxl format Lua 5 Binding
 *
 * See Copyright Notice in im_lib.h
 */

#include <stdlib.h>
#include <stdio.h>

#include "im.h"
#include "im_image.h"
#include "im_format_jxl.h"

#include <lua.h>
#include <lauxlib.h>

#include "imlua.h"
#include "imlua_aux.h"


static int imlua_FormatRegisterJXL(lua_State *L)
{
  (void)L;
  imFormatRegisterJXL();
  return 0;
}

static const struct luaL_Reg imlib[] = {
  {"FormatRegisterJXL", imlua_FormatRegisterJXL},
  {NULL, NULL},
};


static int imlua_jxl_open (lua_State *L)
{
  imFormatRegisterJXL();
  imlua_register_lib(L, imlib);   /* leave "im" table at the top of the stack */
  return 1;
}

int luaopen_imlua_jxl(lua_State *L)
{
  return imlua_jxl_open(L);
}
