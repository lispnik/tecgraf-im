/** \file
 * \brief fits format Lua 5 Binding
 *
 * See Copyright Notice in im_lib.h
 */

#include <stdlib.h>
#include <stdio.h>

#include "im.h"
#include "im_image.h"
#include "im_format_fits.h"

#include <lua.h>
#include <lauxlib.h>

#include "imlua.h"
#include "imlua_aux.h"


static int imlua_FormatRegisterFITS(lua_State *L)
{
  (void)L;
  imFormatRegisterFITS();
  return 0;
}

static const struct luaL_Reg imlib[] = {
  {"FormatRegisterFITS", imlua_FormatRegisterFITS},
  {NULL, NULL},
};


static int imlua_fits_open (lua_State *L)
{
  imFormatRegisterFITS();
  imlua_register_lib(L, imlib);   /* leave "im" table at the top of the stack */
  return 1;
}

int luaopen_imlua_fits(lua_State *L)
{
  return imlua_fits_open(L);
}
