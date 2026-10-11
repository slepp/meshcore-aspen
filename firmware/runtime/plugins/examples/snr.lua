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
    local age=(ctx.node.uptime_ms-(report.rows[1].values.last_seen_ms|0))%4294967296
    local tail="; age "..tostring(age//1000).."s"
    local detail=report:format("{label}: SNR {mean_db:2}/{p95_db:2} dB avg/P95; RSSI {rssi_mean_dbm:1} dBm avg; samples {samples:0}/{window:0}; heard {heard_total:0}","",1,limit-#tail)
    if detail then return detail..tail end
  end
  local header="SNR avg/P95 dB, RSSI avg dBm: "
  local count=hop and report.row_count or 3
  while count>0 do
    local text=report:format("{label} {mean_db:2}/{p95_db:2} {rssi_mean_dbm:1}","; ",count,limit-#header)
    if text then return header..text end
    if hop then break end
    count=count-1
  end
  if hop and report.row_count>1 then return tostring(report.row_count).." hops match "..hop.."; use a longer prefix" end
  return "Reply too small; use a private DM"
end
