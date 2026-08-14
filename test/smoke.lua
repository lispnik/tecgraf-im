-- Load every Lua binding and exercise the core API.
--
-- This is the only check that covers the bindings and the PREFIX "" module
-- naming convention (imlua.so, not libimlua.so), which a C++ test binary
-- cannot reach. CTest invokes it as:
--
--   lua smoke.lua <lib-dir> <module-suffix> [optional-module ...]
--
-- Optional modules are those whose IM_BUILD_* option may be off; CMake passes
-- only the ones actually built.

local lib_dir = assert(arg[1], "usage: smoke.lua <lib-dir> <suffix> [module ...]")
local suffix = assert(arg[2], "usage: smoke.lua <lib-dir> <suffix> [module ...]")

package.cpath = lib_dir .. "/?" .. suffix .. ";" .. package.cpath

local im = require "imlua"
print("imlua loaded, version = " .. tostring(im.Version()))

local image = im.ImageCreate(16, 12, im.RGB, im.BYTE)
assert(image:Width() == 16, "unexpected width")
assert(image:Height() == 12, "unexpected height")
assert(image:ColorSpace() == im.RGB, "unexpected color space")
assert(image:DataType() == im.BYTE, "unexpected data type")

-- Attributes cross the binding boundary into the table reviewed in
-- src/im_attrib.cpp, so round-trip one of each shape.
-- GetAttribute returns a table of byte values unless the third argument asks
-- for a string.
image:SetAttribute("Description", im.BYTE, "smoke test")
assert(image:GetAttribute("Description", true) == "smoke test",
       "string attribute lost")
assert(image:GetAttribute("NoSuchAttribute") == nil,
       "missing attribute should read back as nil")

image:Destroy()

for i = 3, #arg do
  local name = arg[i]
  local module = require(name)
  assert(module ~= nil, name .. " loaded as nil")
  print(name .. " ok")
end

print("ok")
