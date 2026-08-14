/** \file
 * \brief heif format Lua 5 Binding
 *
 * See Copyright Notice in im_lib.h
 */

#include <stdlib.h>
#include <stdio.h>

#include "im.h"
#include "im_image.h"
#include "im_format_heif.h"

#include <lua.h>
#include <lauxlib.h>

#include "imlua.h"
#include "imlua_aux.h"


static int imlua_FormatRegisterHEIF(lua_State *L)
{
  (void)L;
  imFormatRegisterHEIF();
  return 0;
}

static const struct luaL_Reg imlib[] = {
  {"FormatRegisterHEIF", imlua_FormatRegisterHEIF},
  {NULL, NULL},
};


static int imlua_heif_open (lua_State *L)
{
  /* Registers both the HEIF and AVIF drivers. */
  imFormatRegisterHEIF();
  imlua_register_lib(L, imlib);   /* leave "im" table at the top of the stack */
  return 1;
}

int luaopen_imlua_heif(lua_State *L)
{
  return imlua_heif_open(L);
}
