/** \file
 * \brief webp format Lua 5 Binding
 *
 * See Copyright Notice in im_lib.h
 */

#include <stdlib.h>
#include <stdio.h>

#include "im.h"
#include "im_image.h"
#include "im_format_webp.h"

#include <lua.h>
#include <lauxlib.h>

#include "imlua.h"
#include "imlua_aux.h"


static int imlua_FormatRegisterWebP(lua_State *L)
{
  (void)L;
  imFormatRegisterWebP();
  return 0;
}

static const struct luaL_Reg imlib[] = {
  {"FormatRegisterWebP", imlua_FormatRegisterWebP},
  {NULL, NULL},
};


static int imlua_webp_open (lua_State *L)
{
  imFormatRegisterWebP();
  imlua_register_lib(L, imlib);   /* leave "im" table at the top of the stack */
  return 1;
}

int luaopen_imlua_webp(lua_State *L)
{
  return imlua_webp_open(L);
}
