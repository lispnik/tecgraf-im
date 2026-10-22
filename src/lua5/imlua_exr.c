/** \file
 * \brief exr format Lua 5 Binding
 *
 * See Copyright Notice in im_lib.h
 */

#include <stdlib.h>
#include <stdio.h>

#include "im.h"
#include "im_image.h"
#include "im_format_exr.h"

#include <lua.h>
#include <lauxlib.h>

#include "imlua.h"
#include "imlua_aux.h"


static int imlua_FormatRegisterEXR(lua_State *L)
{
  (void)L;
  imFormatRegisterEXR();
  return 0;
}

static const struct luaL_Reg imlib[] = {
  {"FormatRegisterEXR", imlua_FormatRegisterEXR},
  {NULL, NULL},
};


static int imlua_exr_open (lua_State *L)
{
  imFormatRegisterEXR();
  imlua_register_lib(L, imlib);   /* leave "im" table at the top of the stack */
  return 1;
}

int luaopen_imlua_exr(lua_State *L)
{
  return imlua_exr_open(L);
}
