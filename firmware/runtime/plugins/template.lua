-- Start with named Lua functions. Package metadata is added as a Lua comment.
function hello(name)
  return "Hello " .. name
end
command("hello", "name:string:32", "Greet a name", "hello", "public", "!hello mesh")
