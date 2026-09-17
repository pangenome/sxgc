import Sxgc
open Sxgc

def dbgShow (T : Text) : IO Unit := do
  let ts := triplesOf T
  let S := scan (T.length + 1) ts
  let reqs := requirements T
  IO.println s!"T={T} N={T.length+1}"
  IO.println s!"triples: {ts.length} triples"
  IO.println s!"scan: {S}"
  IO.println s!"reqs: {reqs}"
  IO.println s!"suffixient(scan): {suffixient S T} | chi: {chi T} | |S|: {S.length}"

def main : IO Unit := do
  dbgShow [2]
  dbgShow [2,1]
  dbgShow [2,1,1]
  dbgShow [1,2]
