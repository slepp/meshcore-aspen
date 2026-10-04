-- Synthetic catalog fixtures, not live station or weather observations.
local stations={
 {name="Fixture North 01",grid="AA00aa",elevation=101},
 {name="Fixture North 02",grid="AA01ab",elevation=102},
 {name="Fixture North 03",grid="AA02ac",elevation=103},
 {name="Fixture North 04",grid="AA03ad",elevation=104},
 {name="Fixture North 05",grid="AA04ae",elevation=105},
 {name="Fixture North 06",grid="AA05af",elevation=106},
 {name="Fixture North 07",grid="AA06ag",elevation=107},
 {name="Fixture North 08",grid="AA07ah",elevation=108},
 {name="Fixture South 09",grid="AB00aa",elevation=209},
 {name="Fixture South 10",grid="AB01ab",elevation=210},
 {name="Fixture South 11",grid="AB02ac",elevation=211},
 {name="Fixture South 12",grid="AB03ad",elevation=212},
 {name="Fixture South 13",grid="AB04ae",elevation=213},
 {name="Fixture South 14",grid="AB05af",elevation=214},
 {name="Fixture South 15",grid="AB06ag",elevation=215},
 {name="Fixture South 16",grid="AB07ah",elevation=216},
 {name="Fixture West 17",grid="AC00aa",elevation=317},
 {name="Fixture West 18",grid="AC01ab",elevation=318},
 {name="Fixture West 19",grid="AC02ac",elevation=319},
 {name="Fixture West 20",grid="AC03ad",elevation=320},
 {name="Fixture West 21",grid="AC04ae",elevation=321},
 {name="Fixture West 22",grid="AC05af",elevation=322},
 {name="Fixture West 23",grid="AC06ag",elevation=323},
 {name="Fixture West 24",grid="AC07ah",elevation=324},
 {name="Fixture East 25",grid="AD00aa",elevation=425},
 {name="Fixture East 26",grid="AD01ab",elevation=426},
 {name="Fixture East 27",grid="AD02ac",elevation=427},
 {name="Fixture East 28",grid="AD03ad",elevation=428},
 {name="Fixture East 29",grid="AD04ae",elevation=429},
 {name="Fixture East 30",grid="AD05af",elevation=430},
 {name="Fixture East 31",grid="AD06ag",elevation=431},
 {name="Fixture East 32",grid="AD07ah",elevation=432}
}
function hello(name)
 reply("Hello "..name)
end
function station(index)
 local row=stations[index]
 reply(row.name.." ["..row.grid.."] elevation="..tostring(row.elevation))
end
function summary()
 reply(tostring(#stations).." synthetic station fixtures; no network lookup")
end
function difference(first,last)
 local delta=stations[last].elevation-stations[first].elevation
 if delta<0 then delta=-delta end
 reply("Fixture elevation gap: "..tostring(delta))
end
function product(left,right)
 reply(tostring(left*right))
end
function annotate(index,note)
 local text=stations[index].name..": "..note
 if #text>ctx.limits.reply_bytes then
   reply("Error: annotation exceeds the native reply bound")
 else
   reply(text)
 end
end
function reception()
 if ctx.radio.measured then
   reply("Measured RSSI="..tostring(ctx.radio.rssi_dbm)..
     " SNR="..tostring(ctx.radio.snr_db))
 else
   reply("RSSI/SNR unavailable")
 end
end
function caller()
 if ctx.sender.authenticated then
   reply("Authenticated native key: "..ctx.sender.public_key)
 else
   reply("Error: sender is not authenticated")
 end
end
command("hello","name:string:32","Greet a person")
command("station","index:int:1:32","Inspect one synthetic catalog row")
command("summary","","Describe the local fixture catalog")
command("difference","first:int:1:32,last:int:1:32","Compare fixture elevations")
command("product","left:int:-100:100,right:int:-100:100","Bounded integer product")
command("annotate","index:int:1:32,note:text:100","Annotate one synthetic catalog row")
command("reception","","Report this packet's measured radio metadata")
command("caller","","Show this request's authenticated public key")
