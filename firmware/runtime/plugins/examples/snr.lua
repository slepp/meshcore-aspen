local function decimal(n)
  local sign=n<0 and "-" or ""
  local v=((n<0 and -n or n)*100+0.5)//1
  return sign..tostring(v//100).."."..(v%100<10 and "0" or "")..tostring(v%100)
end
command("snr","hop?:string:6","Nearby repeater SNR","snr")
function snr(hop)
  local report=node.packet_report(0)
  if not report or report.name~="repeater_snr" then return "No SNR samples available." end
  if hop then
    for i=1,report.row_count do
      local row=report.rows[i]
      if row.label==hop then
        local v=row.values
        local age=((ctx.node.uptime_ms%4294967296)-v.last_seen_ms+4294967296)%4294967296
        return row.label.." SNR avg "..decimal(v.mean_db).." P95 "..decimal(v.p95_db).." dB; "..
          tostring(v.samples).."/"..tostring(v.window).." samples; heard "..tostring(v.heard_total)..
          "; RSSI "..decimal(v.rssi_mean_dbm).." dBm; "..tostring(age//1000).."s ago"
      end
    end
    return "No SNR samples from "..hop..". Use the hop prefix shown by !snr."
  end
  if report.row_count==0 then return "No repeated RF packets measured yet." end
  local out="SNR avg/P95 dB: "
  for i=1,report.row_count do
    if i>3 then break end
    local row=report.rows[i]
    out=out..(i>1 and "; " or "")..row.label.." "..decimal(row.values.mean_db).."/"..decimal(row.values.p95_db)
  end
  return out.."; !snr HOP for samples/age"
end
