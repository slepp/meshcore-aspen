function save(key,text)
  local ok,state=kv.put(key,text)
  return state
end
function read(key) return kv.get(key) or "empty" end
function alarm(name,seconds)
  timer.set(name,seconds)
  local state=timer.wait(name)
  return "Timer "..name.." "..state.state
end
function timer_state(name) return timer.get(name).state end
function timer_cancel(name) return timer.cancel(name).state end
function wait_channel()
  local message=mesh.wait{kind="channel",exact="ready",timeout_ms=3000}
  return message.nickname..": "..message.text
end
command("save","key:string:32,text:text:120","Save private scoped data","save","dm","!save plan lunch")
command("read","key:string:32","Read private scoped data","read","dm","!read plan")
command("alarm","name:string:32,seconds:int:1:60","Wait on a durable timer","alarm","dm","!alarm tea 2")
command("timer-state","name:string:32","Read durable timer state","timer_state","dm")
command("timer-cancel","name:string:32","Cancel timer","timer_cancel","dm")
command("wait-channel","","Wait for verified channel text","wait_channel","channel")
