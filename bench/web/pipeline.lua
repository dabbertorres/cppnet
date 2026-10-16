-- Sends requests in batches of depth (the script's argument, e.g. `wrk ... -s pipeline.lua -- 16`), as TechEmpower's
-- plaintext test does.
init = function(args)
  local depth = tonumber(args[1]) or 1
  local batch = {}
  for i = 1, depth do batch[i] = wrk.format() end
  req = table.concat(batch)
end

request = function() return req end
