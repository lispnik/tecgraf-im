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

    -- The watershed, the edge-preserving filters and the deconvolution, each
    -- reached through the "New" wrapper in im_process.lua, because that is
    -- the half of the binding a C test cannot cover: the wrappers decide
    -- which source image the destination is built from, and getting that
    -- wrong is invisible until a caller passes two images of different sizes.
    assert(im.DIFFUSION_TUKEY ~= nil, "the DIFFUSION_* constants were not registered")

    -- Two touching discs, the case imProcessWatershedSegment exists for.
    local binary = im.ImageCreate(40, 24, im.BINARY, im.BYTE)
    for y = 0, 23 do
      for x = 0, 39 do
        local d1 = (x - 15)^2 + (y - 12)^2
        local d2 = (x - 25)^2 + (y - 12)^2
        binary[0][y][x] = (d1 <= 49 or d2 <= 49) and 1 or 0
      end
    end

    local ok3, regions, split = im.ProcessWatershedSegmentNew(binary, 8, true)
    assert(ok3, "the watershed segmentation reported failure")
    assert(regions == 2, "the watershed found " .. tostring(regions) .. " regions, not 2")
    assert(split:Width() == 40, "the new image came back the wrong size")

    -- And the shape measurements over the labels it produced.
    -- Zero indexed, like every other measurement table in this binding, so
    -- the two regions are at [0] and [1] and #t is 1 rather than 2.
    local okb, xmin, xmax, ymin, ymax = im.AnalyzeMeasureBoundingBox(split, regions)
    assert(okb, "MeasureBoundingBox reported failure")
    for r = 0, regions - 1 do
      assert(xmin[r] and xmax[r] and ymin[r] and ymax[r],
             "MeasureBoundingBox left region " .. r .. " unfilled")
      assert(xmax[r] >= xmin[r] and ymax[r] >= ymin[r],
             "MeasureBoundingBox returned an inverted box")
    end

    local okf, max_feret = im.AnalyzeMeasureFeret(split, regions)
    assert(okf, "MeasureFeret reported failure")
    assert(max_feret[0] > 0, "MeasureFeret returned a zero diameter")

    local okh, hull_area = im.AnalyzeMeasureConvexHull(split, regions)
    assert(okh, "MeasureConvexHull reported failure")
    assert(hull_area[0] > 0, "MeasureConvexHull returned a zero area")

    local gray = im.ImageCreate(40, 24, im.GRAY, im.BYTE)
    for y = 0, 23 do
      for x = 0, 39 do gray[0][y][x] = 100 end
    end

    local oki, _, _, mean = im.AnalyzeMeasureIntensity(split, gray, 0, regions)
    assert(oki, "MeasureIntensity reported failure")
    assert(math.abs(mean[0] - 100) < 0.001, "MeasureIntensity got the wrong mean")

    -- The filters, on the gray image.
    local okbf, filtered = im.ProcessBilateralFilterNew(gray, 1.5, 20.0)
    assert(okbf, "the bilateral filter reported failure")
    local okad = im.ProcessAnisotropicDiffusionNew(gray, 0.2, 20.0, 3, im.DIFFUSION_EXPONENTIAL)
    assert(okad, "anisotropic diffusion reported failure")
    local oknl = im.ProcessNonLocalMeansNew(gray, 2, 1, 20.0)
    assert(oknl, "non-local means reported failure")

    -- Deconvolution. The PSF is the second source and a different size from
    -- the image, so a wrapper that built the destination from it would make
    -- a 3x3 result -- which is exactly what this checks.
    local psf = im.ImageCreate(3, 3, im.GRAY, im.BYTE)
    for y = 0, 2 do
      for x = 0, 2 do psf[0][y][x] = (x == 1 and y == 1) and 1 or 0 end
    end

    local okrl, restored = im.ProcessRichardsonLucyNew(gray, psf, 3)
    assert(okrl, "Richardson-Lucy reported failure")
    assert(restored:Width() == 40 and restored:Height() == 24,
           "Richardson-Lucy built its destination from the PSF, not the image")

    binary:Destroy()
    split:Destroy()
    gray:Destroy()
    filtered:Destroy()
    psf:Destroy()
    restored:Destroy()
  end

  print(name .. " ok")
end

print("ok")
