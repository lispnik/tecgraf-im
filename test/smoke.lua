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

  -- Reach past "it loaded" for the process module: the decorrelation stretch
  -- returns a struct through the binding, which is a shape nothing else here
  -- exercises, and its constants have to have been registered too.
  if name == "imlua_process" then
    assert(im.DECORR_YRE ~= nil, "the DECORR_* constants were not registered")

    local rgb = im.ImageCreate(8, 8, im.RGB, im.BYTE)
    for y = 0, 7 do
      for x = 0, 7 do
        -- a correlated ramp, so there is a colour cloud to decorrelate
        local v = x * 8 + y * 4
        rgb[0][y][x] = 40 + v
        rgb[1][y][x] = 35 + v
        rgb[2][y][x] = 30 + v
      end
    end

    local ok, dst = im.ProcessDecorrelationStretchNew(rgb, im.DECORR_RGB, 1.5)
    assert(ok, "the decorrelation stretch reported failure")
    assert(dst:Width() == 8, "the new image came back the wrong size")

    local ok2, transform = im.ProcessDecorrelationCalcTransform(rgb, im.DECORR_YUV, 1.0)
    assert(ok2, "computing the transform reported failure")
    assert(#transform.matrix == 9, "the transform matrix did not cross the binding")
    assert(#transform.mean == 3, "the transform mean did not cross the binding")
    assert(transform.rank == 1 or transform.rank == 2 or transform.rank == 3,
           "the transform reported an impossible rank")

    -- and it must survive the round trip back into C
    assert(im.ProcessDecorrelationApplyTransform(rgb, dst, transform),
           "applying the transform reported failure")

    rgb:Destroy()
    dst:Destroy()
  end

  print(name .. " ok")
end

print("ok")
