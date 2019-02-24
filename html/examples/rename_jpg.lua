require 'lfs'

local dir_path = "d://downloads//rename"

print("Renaming:")

function file_exists(name)
  local f=io.open(name,"r")
  if f~=nil then 
    io.close(f) 
    return true 
  else 
    return false 
  end
end

-- redundant, just for the record
local file_rep = nil

for f in lfs.dir(dir_path) do
  if f ~= '.' and f ~= '..' then
    local attr = lfs.attributes(dir_path..'/'..f)
    if attr.mode == 'file' then
      if string.upper(string.sub(f,-3))=="JPG" then
        local d = os.date("*t", attr.modification)
        local new_f = "IMG_" .. string.format("%04d", d.year) .. string.format("%02d", d.month) .. string.format("%02d", d.day) .. "_" .. string.format("%02d", d.hour) .. string.format("%02d", d.min) .. string.format("%02d", d.sec) .. ".JPG"
        if file_exists(dir_path..'/'..new_f) then
          if file_rep then
            file_rep = file_rep + 1
          else
            file_rep = 1
          end
           new_f = "IMG_" .. string.format("%04d", d.year) .. string.format("%02d", d.month) .. string.format("%02d", d.day) .. "_" .. string.format("%02d", d.hour) .. string.format("%02d", d.min) .. string.format("%02d", d.sec) .. "_" .. file_rep .. ".JPG"
        else
          file_rep = nil
        end
        print(f .. "  =>  " .. new_f)
        os.rename (dir_path..'/'..f, dir_path..'/'..new_f)
      end
    end
  end
end
