local function decimal(n,scale)
  scale=scale or 100
  local sign=n<0 and "-" or ""
  local v=((((n<0 and -n or n)*scale+0.5)//1)|0)
  return sign..tostring(v//scale).."."..(scale==100 and v%scale<10 and "0" or "")..tostring(v%scale)
end
command("snr","hop?:hex:6","Repeater SNR and RSSI averages; SNR P95")
command("rssi","hop?:hex:6","Repeater SNR and RSSI averages; SNR P95","snr")
function snr(hop)
  local report=node.packet_report(0,hop)
  if not report or report.name~="repeater_snr" then return "No repeater signal samples available" end
  if report.row_count==0 then
    if hop then return "No signal samples for "..hop.."; use a hop from !snr" end
    return "No repeater signal samples available"
  end
  local limit=ctx.limits.reply_bytes
  if hop and report.row_count==1 then
    local row=report.rows[1]
    local v=row.values
    local age=(ctx.node.uptime_ms-(v.last_seen_ms|0))%4294967296
    local detail=row.label..": SNR "..decimal(v.mean_db).."/"..decimal(v.p95_db).." dB avg/P95; "..
      "RSSI "..decimal(v.rssi_mean_dbm,10).." dBm avg; samples "..tostring(v.samples|0)..
      "/"..tostring(v.window|0).."; heard "..tostring(v.heard_total|0).."; age "..tostring(age//1000).."s"
    if #detail<=limit then return detail end
  end
  local out="SNR avg/P95 dB, RSSI avg dBm: "
  for i=1,report.row_count do
    if not hop and i>3 then break end
    local row=report.rows[i]
    local v=row.values
    local part=(i>1 and "; " or "")..row.label.." "..decimal(v.mean_db).."/"..decimal(v.p95_db).." "..decimal(v.rssi_mean_dbm,10)
    if #out+#part>limit then
      if hop and report.row_count>1 then return tostring(report.row_count).." hops match "..hop.."; use a longer prefix" end
      if i==1 then return "Reply too small; use a private DM" end
      break
    end
    out=out..part
  end
  return out
end
